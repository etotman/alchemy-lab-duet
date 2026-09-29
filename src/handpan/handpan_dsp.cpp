/**
 * handpan_dsp.cpp — modal handpan: nine tone fields, one shell, one cavity.
 *
 * ── Why this shape ──────────────────────────────────────────────────────
 *
 * A handpan tone field is not a drum head. Its maker hammers the dimple
 * until three of its modes sit at f, 2f and 3f, and that tuned triple is
 * what the ear hears as "handpan". Above the triple sit shell modes that
 * nobody tunes; they are inharmonic, they are quiet, and they die in well
 * under a second — but strip them out and the instrument stops sounding
 * like steel.
 *
 * Each mode is one complex one-pole:
 *
 *      z[n] = z[n-1] * (r * e^{jw}) + x[n]        output = Im(z)
 *
 * Fed an impulse this is r^n * sin(wn): a decaying sinusoid starting from
 * zero, which is precisely what a struck mode does. Four multiplies, and
 * unlike a direct-form biquad it stays exact with the pole radius pinned
 * at 0.99998 for a twelve-second ring. Frequency modulation — the tension
 * nonlinearity — is a rotation of the coefficient pair, no trig needed.
 *
 * ── Everything is feedforward ───────────────────────────────────────────
 *
 * A strike drives the field it landed on, *and* every other field, *and*
 * the air cavity. The cavity drives the fields. Nothing drives anything
 * backwards. That is a deliberate limit: a resonant mode at r = 0.99998
 * has a steady-state gain of ~80,000 at its own frequency, so any feedback
 * path around the mode bank would need a loop gain under 1e-5 to stay
 * bounded — useless for coupling, and one bad knob position away from a
 * detonation. It costs almost nothing musically, because a handpan's halo
 * arrives *with* the strike (the shell transmits the impact in
 * microseconds) rather than building up afterwards. So: unconditionally
 * stable, at every setting, forever.
 *
 * ── Where each control physically acts ──────────────────────────────────
 *
 * Mallet hardness is the *width of the force pulse*, not an EQ: a hard
 * fingertip is a 0.3 ms impulse with energy up past 3 kHz, a soft thumb is
 * a 3 ms one that barely excites the twelfth. Strike position is a tilt on
 * the per-mode injection weights, applied to the incoming drive only — so
 * changing it never disturbs a note that is already ringing. Velocity
 * splits three ways: overall level, a spectral tilt toward the shell
 * modes, and the tension bloom.
 */

#include "handpan_dsp.h"

#include <cmath>
#include <cstring>

namespace handpan_dsp {

namespace {

/* ═══ Tunings ═══════════════════════════════════════════════════════════
 *
 * Nine semitone offsets from the root; [0] is the ding, [1..8] climb the
 * rim. Spelled as intervals rather than note names so the content is
 * checkable at a glance, and so the root knob transposes the whole
 * instrument without a second table.
 */
struct Scale
{
    const char* name;
    int8_t      step[kNumFields];
};

constexpr Scale kScales[kNumScales] = {
    /* D Kurd: D  A Bb  C  D  E  F  G  A   — aeolian, the classic 9-note pan */
    { "Kurd",     { 0,  7,  8, 10, 12, 14, 15, 17, 19 } },
    /* Celtic minor: D  A  C  D  E  F  G  A  C  — no second, very open      */
    { "Celtic",   { 0,  7, 10, 12, 14, 15, 17, 19, 22 } },
    /* Hijaz: D  A Bb C# D  E  F  G  A  — phrygian dominant, the augmented
       second between Bb and C# is the whole character of the scale        */
    { "Hijaz",    { 0,  7,  8, 11, 12, 14, 15, 17, 19 } },
    /* Pygmy: minor pentatonic, no semitones anywhere — nothing can clash  */
    { "Pygmy",    { 0,  3,  5,  7, 10, 12, 15, 17, 19 } },
    /* Dorian: natural minor with a raised sixth                            */
    { "Dorian",   { 0,  7,  9, 10, 12, 14, 15, 17, 19 } },
    /* Major (ionian), the bright "sabye"-family layout                     */
    { "Major",    { 0,  7,  9, 11, 12, 14, 16, 17, 19 } },
    /* Harmonic minor — minor with a leading tone                           */
    { "Harmonic", { 0,  7,  8, 11, 12, 14, 15, 19, 20 } },
    /* Insen: Japanese, 1 b2 4 5 b7 — sparse and unresolved                 */
    { "Insen",    { 0,  7, 10, 12, 13, 17, 19, 22, 24 } },
};

/* ═══ Shell modes ═══════════════════════════════════════════════════════
 *
 * Above the tuned 1:2:3 triple, a real tone field rings at ratios that no
 * one chose. These are representative; each field jitters them by a few
 * percent from a fixed hash, so no two fields on the instrument share an
 * upper structure — as true of a hand-hammered shell as it is useful for
 * keeping the sympathetic halo from sounding like a chorus pedal.
 */
constexpr float kInharmonic[6] = { 4.16f, 5.31f, 6.72f, 8.14f, 9.83f, 11.70f };

/* Relative injection level of each mode at a neutral strike. The tuned
 * triple carries the note; the shell modes are 20-30 dB down and are what
 * you hear as the metal rather than as pitch. */
constexpr float kHarmAmp[3]  = { 1.00f, 0.56f, 0.30f };
constexpr float kShellAmp[6] = { 0.130f, 0.100f, 0.072f, 0.050f, 0.036f, 0.026f };

/* T60 of a shell mode relative to the tuned modes at the same frequency —
 * they are heavily damped and essentially gone inside a second. This is
 * the value at Metal's default; Metal lengthens it toward the top. */
constexpr float kShellDamp = 0.22f;

/* ── Voicing knob curves ───────────────────────────────────────────────
 *
 * Every Voicing knob is mapped through three points: its value at 0, at
 * the default the voice was approved at, and at 1. The default point is
 * what the engine always did at that knob position, so the approved voice
 * is unchanged; the two ends are where the range was widened.
 *
 * The widening was measured, not guessed. With the old straight-line
 * mappings, sweeping Metal, Tilt or Contact from end to end changed the
 * output by 19-36 dB less than the sound itself — below hearing next to
 * the tuned modes — and Shimmer's beat rate spanned only 0.1-0.9 Hz.
 *
 * The reason is the same for Contact and Metal. A mode rings with the
 * whole force pulse integrated into it, which at a 0.8 ms pulse is about
 * 25x the pulse's own height, while the bare contact noise goes straight
 * to the output. So a "level" that looks generous on paper sits 30 dB
 * under the note. Metal's shell modes also start 18-32 dB down and sit
 * where the half-sine pulse is already rolling off. */
constexpr float kShimDef     = 0.45f;   /* knob defaults the voice was   */
constexpr float kMetalDef    = 0.50f;   /* approved at (render Defaults, */
constexpr float kTiltDef     = 0.55f;   /* preset factory values)        */
constexpr float kContactDef  = 0.55f;

/* Shimmer: relative split (fraction of the mode frequency), absolute beat
 * floor in Hz, and the weaker half's share of the pair. The pair stays
 * lopsided even at the top so the beat never nulls into a tremolo. */
constexpr float kShimRel[3]   = { 0.0006f, 0.00177f, 0.0110f };
constexpr float kShimFloor[3] = { 0.10f,   0.46f,    3.50f   };
constexpr float kShimUpper[3] = { 0.32f,   0.32f,    0.44f   };

/* Metal: shell-mode level in dB and their T60 relative to kShellDamp. */
constexpr float kMetalDb[3]   = { -60.f, -6.02f, 18.f };
constexpr float kMetalRing[3] = { 0.55f, 1.f,    2.8f };

/* Tilt: exponent of T60 against partial ratio. 0.7725 is the old default;
 * below zero the upper partials outlast the fundamental (glassy), at the
 * top everything above the fundamental is gone in a fraction of a second.
 *
 * Decay alone was not enough to hear: the octave and twelfth already sit
 * 8 and 18 dB under the fundamental, so changing how fast they fade
 * changed little. Tilt therefore also tilts their level, as ratio^k on
 * the tuned triple only (Metal owns the shell modes' level): at 0 the
 * octave comes up level with the fundamental, at 1 it is ~10 dB further
 * down, and at the default k is 0, so the approved voice is untouched. */
constexpr float kTiltExp[3]   = { -0.60f, 0.7725f, 2.40f };
constexpr float kTiltLvl[3]   = {  0.90f, 0.f,    -1.60f };

/* Contact: gain of the bare tak in dB. The old mapping was 0.30 x knob,
 * which put the tak 50-60 dB under the note at every setting. The default
 * is raised to +8 dB, which still leaves it about 40 dB down and masked,
 * so the approved voice does not change; the top is where it becomes the
 * audible click of skin on steel, peaking a few dB under the attack. */
constexpr float kContactDb[3] = { -30.f, 8.f, 38.f };

/* How much faster a high rim note decays than the ding. Small shells
 * store less energy; the exponent is on the frequency ratio. */
constexpr float kFieldDecayExp = 0.30f;

constexpr float kLn1000 = 6.90775528f;

/* How far past the pan's top and bottom notes a 1 V/oct request is still
 * taken at face value before it is folded by octaves back into range.
 * Two semitones: enough that a note a little flat or sharp of an end field
 * still lands on it, small enough that the plateau at each end stays well
 * under the gaps inside the scale itself. */
constexpr float kNoteFoldTolSt = 2.f;

/* ═══ Excitation ════════════════════════════════════════════════════════ */

/* Strike force pulse: a half-sine, which is what Hertzian contact between
 * a finger and a steel shell actually looks like. The *width* is the
 * mallet control — a hard fingertip is a brief impulse with energy well
 * past 3 kHz, a soft thumb is a long one that barely moves the twelfth.
 *
 * A half-sine is used rather than a raised cosine because a raised cosine
 * of length L has a hard spectral null at 1/L, and at any plausible thumb
 * width that null lands straight on the tuned twelfth and erases the
 * interval that defines the instrument. The half-sine's first null is at
 * 1.5/L and its rolloff is gentle, so softening the mallet tilts the
 * spectrum instead of punching a hole in it.
 *
 * The range is what a hand does: finger-on-steel contact is a fraction of
 * a millisecond, and even the fleshy heel of the thumb is not much over
 * one. Earlier values of 0.3-3.2 ms were modelling a mallet head. */
constexpr float kPulseMsHard = 0.14f;
constexpr float kPulseMsSoft = 1.30f;

/* Skin-on-steel contact noise: bandpass centre and decay. */
constexpr float kContactHzSoft = 1600.f;
constexpr float kContactHzHard = 5200.f;
constexpr float kContactMsSoft = 14.f;
constexpr float kContactMsHard = 5.f;

/* Simultaneous strikes we can be mid-pulse on. A pulse is under 4 ms, so
 * eight is far past anything ten fingers can do. */
constexpr uint8_t kNumExciters = 8;

constexpr uint8_t kStrikeQueueLen = 16; /* power of two */

/* Tension nonlinearity settling time. */
constexpr float kBloomMs = 150.f;

/* Below this a field is silent enough to skip entirely. */
constexpr float kEnergyFloor = 3e-5f;

/* Master trim. Calibrated so a full-velocity strike with the voicing wide
 * open lands just under the soft clip rather than in it — measured with
 * the offline renderer's `trim` scene, not guessed. Overridable at build
 * time so that measurement can be repeated without editing the source. */
#ifndef HANDPAN_OUT_TRIM
#define HANDPAN_OUT_TRIM 0.019f
#endif
constexpr float kOutTrim = HANDPAN_OUT_TRIM;

/* ═══ State ═════════════════════════════════════════════════════════════ */

float sr_      = 48000.f;
float inv_sr_  = 1.f / 48000.f;

/* --- mode bank, struct-of-arrays so the inner loop streams ------------- */
float m_zr_ [kTotalModes];   /* resonator state                            */
float m_zi_ [kTotalModes];
float m_cr_ [kTotalModes];   /* live coefficient (nominal, bloom applied)  */
float m_ci_ [kTotalModes];
float m_cr0_[kTotalModes];   /* nominal coefficient, r*cos(w) / r*sin(w)   */
float m_ci0_[kTotalModes];
float m_w_  [kTotalModes];   /* angular frequency, for the bloom rotation  */
float m_exd_[kTotalModes];   /* injection weight, direct strike            */
float m_exs_[kTotalModes];   /* injection weight, sympathetic / external   */
/* There is deliberately no separate output-gain array. A mode driven by a
 * unit impulse rings with peak amplitude 1 regardless of its pole radius,
 * so the injection weight *is* the mode's level in the mix, and a second
 * gain on the way out would square it — which is exactly the bug that had
 * the tuned twelfth sitting 21 dB below where it belonged. It also means
 * a mode struck directly and the same mode excited through the shell can
 * have different spectra, which is physically right: energy arriving
 * through the steel does not know where the hand landed. */

float f_hz_    [kNumFields]; /* fundamental of each field                  */
float f_energy_[kNumFields]; /* envelope follower, for the LED rings       */
float f_bloom_ [kNumFields]; /* current pitch offset, fraction             */
float f_panLa_ [kNumFields], f_panRa_[kNumFields];  /* even-index modes    */
float f_panLb_ [kNumFields], f_panRb_[kNumFields];  /* odd-index modes     */
float couple_  [kNumFields][kNumFields];            /* strike -> field     */

float bloom_decay_ = 0.f;
float energy_coeff_ = 0.f;

/* --- exciters ---------------------------------------------------------- */
struct Exciter
{
    int32_t left     = 0;    /* samples of force pulse remaining           */
    float   ph       = 0.f;  /* raised-cosine phase, 0..1                  */
    float   ph_inc   = 0.f;
    float   gain     = 0.f;
    uint8_t field    = 0;
    /* contact noise: one-pole-decayed white through a 2-pole bandpass */
    float   n_env    = 0.f;
    float   n_dec    = 0.f;
    float   n_gain   = 0.f;
    float   bp_lp    = 0.f;
    float   bp_bp    = 0.f;
    float   bp_f     = 0.f;
    float   bp_q     = 0.f;
};
Exciter ex_[kNumExciters];

/* --- Helmholtz cavity: one bandpass, driven by the strikes -------------- */
float cav_lp_ = 0.f, cav_bp_ = 0.f, cav_f_ = 0.f, cav_q_ = 0.35f;

/* --- output DC blocker -------------------------------------------------- */
float dc_xl_ = 0.f, dc_yl_ = 0.f, dc_xr_ = 0.f, dc_yr_ = 0.f;

uint32_t rng_ = 0x1234567u;

/* --- control-thread inputs ---------------------------------------------- */
volatile uint8_t p_scale_   = 0;
volatile float   p_root_    = 146.83f;   /* D3 */
volatile float   p_pos_     = 0.35f;
volatile float   p_mallet_  = 0.45f;
volatile float   p_decay_   = 7.0f;
volatile float   p_symp_    = 0.55f;
volatile float   p_damp_    = 0.f;
volatile float   p_shimmer_ = 0.45f;
volatile float   p_temper_  = 0.f;    /* 0 == a real handpan's 1:2:3 */
volatile float   p_metal_   = 0.50f;
volatile float   p_tilt_    = 0.55f;
volatile float   p_cavtune_ = 0.92f;
volatile float   p_cavlvl_  = 0.45f;
volatile float   p_bloom_   = 18.f;
volatile float   p_spread_  = 0.75f;
volatile float   p_dyn_     = 0.80f;
volatile float   p_contact_ = 0.55f;
volatile float   p_level_   = 0.85f;
volatile float   p_ext_     = 0.60f;

volatile uint32_t dirty_ = 1u;   /* bumped by any Set* that changes coeffs */
uint32_t          seen_  = 0u;

/* strike queue */
struct StrikeReq { uint8_t field; float vel; };
StrikeReq         sq_[kStrikeQueueLen];
volatile uint32_t sq_w_ = 0u;
uint32_t          sq_r_ = 0u;

/* audio-thread mirrors of the control params, refreshed on retune */
float a_symp_ = 0.f, a_cavlvl_ = 0.f, a_bloom_ = 0.f, a_dyn_ = 0.f;
float a_contact_ = 0.f, a_mallet_ = 0.f, a_pos_ = 0.f;
float a_level_ = 0.f, a_ext_ = 0.f;

uint8_t retune_cursor_ = 0;   /* free-running: one field rebuilt per block */

/* ═══ Helpers ═══════════════════════════════════════════════════════════ */

inline float Rnd()  /* white, -1..1 */
{
    rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
    return static_cast<float>(static_cast<int32_t>(rng_)) * 4.6566129e-10f;
}

/** Deterministic per-(field,mode) jitter in -1..1, so a given instrument is
 *  the same instrument every power-up. */
inline float Jitter(uint8_t field, uint8_t mode)
{
    uint32_t h = static_cast<uint32_t>(field) * 0x9E3779B9u
               + static_cast<uint32_t>(mode)  * 0x85EBCA6Bu;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return static_cast<float>(h & 0xFFFFu) * (1.f / 32768.f) - 1.f;
}

inline float Clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/** Piecewise-linear knob curve: y[0] at 0, y[1] at x_def, y[2] at 1. */
inline float Through(float x, float x_def, const float (&y)[3])
{
    x = Clampf(x, 0.f, 1.f);
    if (x <= x_def) return y[0] + (y[1] - y[0]) * (x / x_def);
    return y[1] + (y[2] - y[1]) * ((x - x_def) / (1.f - x_def));
}

/** Through() in dB, returned as a linear gain; exactly 0 at the knob's
 *  bottom stop so "off" is off. */
inline float ThroughDb(float x, float x_def, const float (&db)[3])
{
    if (x <= 0.f) return 0.f;
    return powf(10.f, Through(x, x_def, db) * 0.05f);
}

/**
 * Output limiter: exactly transparent below the knee, saturating above it.
 *
 * The cubic `1.5*(x - x^3/3)` form that the other firmwares here use is an
 * overdrive, not a limiter — its slope at the origin is 1.5, so it adds
 * third-order distortion to *every* sample at *every* level, not just the
 * ones that need catching. On a bank of high-Q resonators that is audible
 * on the loudest partials of every strike, and it is the wrong kind of
 * nonlinearity for this instrument: a real shell's nonlinearity is tension
 * modulation, which the model already does properly as the strike bloom.
 *
 * Below the knee this returns x untouched. Above it, u/(1+u) saturates
 * smoothly toward 1.0 with slope exactly 1 at the knee, so there is no
 * discontinuity in value or in slope to hear — and no exp() or tanh() in
 * the hot path.
 */
constexpr float kClipKnee = 0.60f;

inline float SoftClip(float x)
{
    const float a = x < 0.f ? -x : x;
    if (a <= kClipKnee) return x;

    const float u = (a - kClipKnee) * (1.f / (1.f - kClipKnee));
    const float y = kClipKnee + (1.f - kClipKnee) * (u / (1.f + u));
    return x < 0.f ? -y : y;
}

/** Equal-power pan from a -1..1 position. */
inline void Pan(float pos, float& l, float& r)
{
    const float th = (Clampf(pos, -1.f, 1.f) + 1.f) * 0.7853981634f; /* pi/4 */
    l = cosf(th);
    r = sinf(th);
}

/* ═══ Retuning ══════════════════════════════════════════════════════════
 *
 * Rebuilding a field costs a dozen exp() and sincos() calls, so we do one
 * field per audio block rather than all nine at once — nine blocks is
 * 4.5 ms at a 24-sample block, far below the ear's threshold for a knob
 * turn, and the per-block cost stays flat and predictable. It also keeps
 * every write to a coefficient pair on the audio thread, so a half-written
 * (cr, ci) can never be handed to the resonator loop.
 */
void RebuildField(uint8_t f)
{
    const float root   = Clampf(p_root_, 30.f, 900.f);
    const Scale& sc    = kScales[p_scale_ % kNumScales];
    const float hz     = root * exp2f(static_cast<float>(sc.step[f]) / 12.f);
    f_hz_[f]           = hz;

    /* Ring length. The knob sets the ding; smaller shells ring shorter. */
    const float damp   = 1.f - 0.965f * Clampf(p_damp_, 0.f, 1.f);
    const float t60_f  = Clampf(p_decay_, 0.15f, 20.f) * damp
                       * powf(root / hz, kFieldDecayExp);

    /* How steeply the top end closes down relative to the fundamental. */
    const float tilt   = Through(p_tilt_, kTiltDef, kTiltExp);
    const float tilt_k = Through(p_tilt_, kTiltDef, kTiltLvl);

    /* The beating pair split. Expressed as a fraction of the mode
     * frequency and then floored in absolute Hz, because what the ear
     * tracks is beats per second, and a purely proportional split makes
     * the ding shimmer far slower than the top notes. */
    const float shim   = Clampf(p_shimmer_, 0.f, 1.f);
    const float shim_rel   = Through(shim, kShimDef, kShimRel);
    const float shim_floor = Through(shim, kShimDef, kShimFloor);
    const float shim_upper = Through(shim, kShimDef, kShimUpper);

    /* Tuned-triple stretch. The exponent form is used rather than a linear
     * detune because it keeps the intervals musically coherent as it bends
     * — every partial moves by the same proportion of its own harmonic
     * number, which is how real inharmonicity behaves in a stiff plate. */
    const float temper = Clampf(p_temper_, -0.4f, 0.4f);

    /* Strike-position tilt: negative pushes energy into the fundamental
     * (the round thud of a thumb in the dimple), positive into the upper
     * modes (a fingertip out on the shoulder). */
    const float pos    = Clampf(p_pos_, 0.f, 1.f);
    const float ptilt  = -0.55f + 1.00f * pos;
    const float metal      = ThroughDb(p_metal_, kMetalDef, kMetalDb);
    const float shell_damp = kShellDamp * Through(p_metal_, kMetalDef, kMetalRing);

    /* The ding is the largest field and the one the cavity is tuned to:
     * more fundamental, less steel, longer ring. */
    const bool  ding   = (f == 0);
    const float dingf  = ding ? 1.10f : 1.f;
    const float dingm  = ding ? 0.72f : 1.f;

    const uint16_t base = static_cast<uint16_t>(f) * kModesPerField;

    for (uint8_t m = 0; m < kModesPerField; m++)
    {
        const uint16_t i = base + m;

        float ratio, amp, dampmul;

        if (m < 6)
        {
            /* Tuned triple, each as a beating pair. */
            const uint8_t h    = static_cast<uint8_t>(m >> 1);   /* 0,1,2 */
            const bool    upper = (m & 1u) != 0u;
            const float   n    = static_cast<float>(h + 1);

            /* n^(1+temper): 1, 2, 3 at temper 0, and the fundamental is
             * pinned exactly rather than left to powf's rounding, so the
             * note never drifts off the scale no matter how far the upper
             * partials are bent. */
            const float hr = (h == 0u) ? 1.f : powf(n, 1.f + temper);

            /* The pair's split is set as a *beat rate*, because beats per
             * second is what the ear actually tracks. Splitting is a
             * relative effect of the shell's asymmetry, so it grows with
             * frequency — but a purely relative split leaves the ding
             * beating once a minute, so there is an absolute floor under
             * it. Jitter keeps the three beats of a field drifting against
             * each other instead of pulsing in lockstep. */
            const float mhz_nom = hz * hr;
            const float jit     = 1.f + 0.40f * Jitter(f, static_cast<uint8_t>(m + 40));
            float beat_hz = shim_rel * mhz_nom;
            if (beat_hz < shim_floor) beat_hz = shim_floor;
            beat_hz *= jit;

            /* two modes at f(1 +/- s) beat at 2*s*f */
            const float split = (shim > 0.f)
                              ? (beat_hz / (2.f * mhz_nom)) : 0.f;

            ratio = hr * (upper ? (1.f + split) : (1.f - split));

            /* Deliberately unequal. Two modes of equal amplitude beat to a
             * complete null once a cycle, which reads as a tremolo rather
             * than as a living instrument; a real field's split pair is
             * lopsided and the two halves decay at different rates, so the
             * beat breathes and then drifts apart instead of pumping. */
            amp     = kHarmAmp[h] * (upper ? shim_upper : 1.f - shim_upper);
            dampmul = upper ? 0.86f : 1.f;

            /* Out on the shoulder the fundamental itself thins out. */
            if (h == 0) amp *= (1.f - 0.40f * pos) * dingf;
            else        amp *= powf(hr, tilt_k);
        }
        else
        {
            /* Untuned shell modes. */
            const uint8_t k = static_cast<uint8_t>(m - 6);
            ratio   = kInharmonic[k] * (1.f + 0.045f * Jitter(f, m));
            amp     = kShellAmp[k] * metal * dingm;
            dampmul = shell_damp;
        }

        const float mhz = hz * ratio;
        if (mhz > sr_ * 0.47f)
        {
            /* Above Nyquist: park it silent rather than alias. */
            m_cr0_[i] = m_ci0_[i] = m_cr_[i] = m_ci_[i] = 0.f;
            m_w_[i] = m_exd_[i] = m_exs_[i] = 0.f;
            m_zr_[i] = m_zi_[i] = 0.f;
            continue;
        }

        /* T60 falls with frequency; shell modes fall off a cliff. */
        const float t60 = Clampf(t60_f * powf(1.f / ratio, tilt) * dampmul,
                                 0.004f, 25.f);
        const float r   = expf(-kLn1000 / (t60 * sr_));
        const float w   = 6.2831853072f * mhz * inv_sr_;

        m_w_  [i] = w;
        m_cr0_[i] = r * cosf(w);
        m_ci0_[i] = r * sinf(w);
        m_cr_ [i] = m_cr0_[i];
        m_ci_ [i] = m_ci0_[i];

        /* Injection weights, which are also the mode's level in the mix.
         * Direct strikes carry the strike-position tilt; the sympathetic
         * path is flatter and weighted toward the shell modes. */
        m_exd_[i] = amp * powf(ratio, ptilt);
        m_exs_[i] = amp * powf(ratio, -0.15f) * (m < 6 ? 1.f : 1.35f);
    }

    /* Stereo placement. The ding sits centre; rim notes alternate left and
     * right as the scale climbs, spreading wider toward the top, which is
     * how the notes are actually laid out around the shell. The two halves
     * of every beating pair are nudged a hair apart, so the shimmer moves
     * in the stereo field instead of pumping in the middle. */
    const float spread = Clampf(p_spread_, 0.f, 1.f);
    float pos_s;
    if (f == 0)
    {
        pos_s = 0.f;
    }
    else
    {
        const float mag  = 0.34f + 0.66f * (static_cast<float>(f - 1) / 7.f);
        pos_s = ((f & 1u) ? -mag : mag) * spread;
    }
    Pan(pos_s - 0.09f * spread, f_panLa_[f], f_panRa_[f]);
    Pan(pos_s + 0.09f * spread, f_panLb_[f], f_panRb_[f]);
}

/** Coupling matrix: how much of a strike on `s` reaches field `f`.
 *  The ding shares the most shell with everything; neighbours on the rim
 *  share a weld seam and couple harder than notes on the far side. */
void RebuildCoupling()
{
    /* Calibrated against the spectrum of a single strike: at the default
     * setting the loudest sympathetic partial should sit roughly 30 dB
     * under the struck note's fundamental. Push this much past 0.2 and the
     * instrument stops having a melody — every strike plays a chord. */
    const float k = 0.20f * Clampf(p_symp_, 0.f, 1.f);

    for (uint8_t s = 0; s < kNumFields; s++)
    {
        for (uint8_t f = 0; f < kNumFields; f++)
        {
            if (s == f) { couple_[s][f] = 0.f; continue; }  /* direct path */

            float w;
            if (s == 0 || f == 0)
            {
                w = 0.62f;                    /* the ding touches everything */
            }
            else
            {
                const int d = (s > f) ? (s - f) : (f - s);
                w = (d == 1) ? 0.72f : (d == 2 ? 0.48f : 0.34f);
            }
            couple_[s][f] = k * w;
        }
    }
}

void RebuildGlobal()
{
    a_symp_    = Clampf(p_symp_,    0.f, 1.f);
    a_cavlvl_  = Clampf(p_cavlvl_,  0.f, 1.f);
    a_bloom_   = Clampf(p_bloom_,   0.f, 60.f);
    a_dyn_     = Clampf(p_dyn_,     0.f, 1.f);
    a_contact_ = ThroughDb(p_contact_, kContactDef, kContactDb);
    a_mallet_  = Clampf(p_mallet_,  0.f, 1.f);
    a_pos_     = Clampf(p_pos_,     0.f, 1.f);
    a_level_   = Clampf(p_level_,   0.f, 1.f);
    a_ext_     = Clampf(p_ext_,     0.f, 1.f);

    /* Helmholtz. A real gu port lands near the ding's fundamental and is
     * heavily damped — it is a bloom under the attack, not a pitch. */
    const float cf = Clampf(p_root_ * Clampf(p_cavtune_, 0.4f, 2.2f),
                            25.f, 800.f);
    cav_f_ = 2.f * sinf(3.14159265f * cf * inv_sr_);
    cav_q_ = 0.34f;

    RebuildCoupling();
}

/* ═══ Strike ════════════════════════════════════════════════════════════ */

void FireStrike(uint8_t field, float vel)
{
    if (field >= kNumFields) return;
    vel = Clampf(vel, 0.f, 1.f);
    if (vel <= 0.f) return;

    /* Velocity does three things on a real instrument, and all three are
     * audible: it is louder, it is brighter (a harder contact drives the
     * shell modes disproportionately), and it starts sharp. */
    const float dyn  = a_dyn_;
    const float hard = Clampf(a_mallet_ + dyn * 0.30f * (vel - 0.5f), 0.f, 1.f);

    /* Grab an exciter — the quietest one if all are busy. */
    uint8_t slot = 0;
    int32_t worst = 0x7FFFFFFF;
    for (uint8_t i = 0; i < kNumExciters; i++)
    {
        if (ex_[i].left <= 0 && ex_[i].n_env < 1e-4f) { slot = i; worst = -1; break; }
        if (ex_[i].left < worst) { worst = ex_[i].left; slot = i; }
    }

    Exciter& e = ex_[slot];

    const float pulse_ms = kPulseMsSoft + (kPulseMsHard - kPulseMsSoft) * hard;
    const int32_t len    = static_cast<int32_t>(pulse_ms * 0.001f * sr_ + 0.5f);

    e.left   = len > 2 ? len : 2;
    e.ph     = 0.f;
    e.ph_inc = 1.f / static_cast<float>(e.left);
    e.gain   = vel;
    e.field  = field;

    /* Contact noise — the "tak" of skin on steel. Harder and faster gives
     * a higher, shorter tick. */
    const float nhz = kContactHzSoft + (kContactHzHard - kContactHzSoft) * hard;
    const float nms = kContactMsSoft + (kContactMsHard - kContactMsSoft) * hard;
    e.n_env  = 1.f;
    e.n_dec  = expf(-1.f / (nms * 0.001f * sr_));
    e.n_gain = powf(vel, 1.f + 0.5f * dyn) * (0.22f + 0.78f * hard);
    e.bp_lp  = 0.f;
    e.bp_bp  = 0.f;
    e.bp_f   = 2.f * sinf(3.14159265f * Clampf(nhz, 20.f, sr_ * 0.42f) * inv_sr_);
    e.bp_q   = 0.85f;

    /* Note there is no separate "brighter when hit harder" term here: a
     * harder strike already shortens the force pulse above, which pushes
     * energy up into the shell modes exactly the way the real mechanism
     * does. Scaling the injection weights on top of that would be the same
     * effect applied twice. */

    /* Tension nonlinearity: start sharp, settle over ~150 ms. Scales with
     * velocity squared, which is roughly how the extra shell tension goes. */
    if (a_bloom_ > 0.05f)
    {
        const float cents = a_bloom_ * vel * vel;
        f_bloom_[field]   = cents * (1.f / 1200.f) * 0.69314718f; /* -> ln-ratio */
    }
}

} // namespace

/* ═══════════════════════════════════════════════════════════════════════
 *                              Public API
 * ═══════════════════════════════════════════════════════════════════════ */

const char* ScaleName(uint8_t scale) { return kScales[scale % kNumScales].name; }

void Init(float sample_rate)
{
    sr_     = sample_rate > 1000.f ? sample_rate : 48000.f;
    inv_sr_ = 1.f / sr_;

    std::memset(m_zr_,  0, sizeof(m_zr_));
    std::memset(m_zi_,  0, sizeof(m_zi_));
    std::memset(f_energy_, 0, sizeof(f_energy_));
    std::memset(f_bloom_,  0, sizeof(f_bloom_));

    for (uint8_t i = 0; i < kNumExciters; i++) ex_[i] = Exciter{};

    cav_lp_ = cav_bp_ = 0.f;
    dc_xl_ = dc_yl_ = dc_xr_ = dc_yr_ = 0.f;
    sq_r_ = sq_w_ = 0u;

    bloom_decay_  = expf(-1.f / (kBloomMs * 0.001f * sr_));
    energy_coeff_ = 1.f - expf(-1.f / (0.030f * sr_));

    RebuildGlobal();
    for (uint8_t f = 0; f < kNumFields; f++) RebuildField(f);
    retune_cursor_ = 0;
    seen_ = dirty_;
}

/* Every setter is a word write plus a dirty bump; the audio thread does the
 * arithmetic. The comparison is not an optimisation — a setter called at
 * poll rate with an unchanged value (SetHandDamp(0) every millisecond, say)
 * would otherwise raise the flag faster than the engine could act on it. */
#define HP_SET(field, expr)                                                   \
    do {                                                                      \
        const auto hp_v_ = (expr);                                            \
        if ((field) != hp_v_) { (field) = hp_v_; dirty_++; }                  \
    } while (0)

void SetScale(uint8_t s)        { HP_SET(p_scale_,   static_cast<uint8_t>(s % kNumScales)); }
void SetRootHz(float hz)        { HP_SET(p_root_,    Clampf(hz, 30.f, 900.f)); }
void SetPosition(float p)       { HP_SET(p_pos_,     Clampf(p, 0.f, 1.f)); }
void SetMallet(float m)         { HP_SET(p_mallet_,  Clampf(m, 0.f, 1.f)); }
void SetDecay(float s)          { HP_SET(p_decay_,   Clampf(s, 0.15f, 20.f)); }
void SetSympathy(float s)       { HP_SET(p_symp_,    Clampf(s, 0.f, 1.f)); }
void SetHandDamp(float d)       { HP_SET(p_damp_,    Clampf(d, 0.f, 1.f)); }
void SetShimmer(float s)        { HP_SET(p_shimmer_, Clampf(s, 0.f, 1.f)); }
void SetTemper(float t)         { HP_SET(p_temper_,  Clampf(t, -0.4f, 0.4f)); }
void SetMetal(float m)          { HP_SET(p_metal_,   Clampf(m, 0.f, 1.f)); }
void SetTilt(float t)           { HP_SET(p_tilt_,    Clampf(t, 0.f, 1.f)); }
void SetCavityTune(float r)     { HP_SET(p_cavtune_, Clampf(r, 0.4f, 2.2f)); }
void SetCavityLevel(float l)    { HP_SET(p_cavlvl_,  Clampf(l, 0.f, 1.f)); }
void SetBloom(float cents)      { HP_SET(p_bloom_,   Clampf(cents, 0.f, 60.f)); }
void SetSpread(float s)         { HP_SET(p_spread_,  Clampf(s, 0.f, 1.f)); }
void SetDynamics(float d)       { HP_SET(p_dyn_,     Clampf(d, 0.f, 1.f)); }
void SetContact(float c)        { HP_SET(p_contact_, Clampf(c, 0.f, 1.f)); }
void SetLevel(float l)          { HP_SET(p_level_,   Clampf(l, 0.f, 1.f)); }
void SetExtLevel(float l)       { HP_SET(p_ext_,     Clampf(l, 0.f, 1.f)); }

#undef HP_SET

void Strike(uint8_t field, float velocity)
{
    if (field >= kNumFields) return;
    const uint32_t w = sq_w_;
    sq_[w & (kStrikeQueueLen - 1)] = { field, velocity };
    sq_w_ = w + 1u;
}

uint8_t FieldForVolts(float volts)
{
    const Scale& sc = kScales[p_scale_ % kNumScales];
    float        st = volts * 12.f;

    /* The pan's compass, in semitones from the ding. Recomputed rather than
     * assuming the tables are sorted, which they happen to be. */
    float lo =  127.f;
    float hi = -127.f;
    for (uint8_t f = 0; f < kNumFields; f++)
    {
        const float o = static_cast<float>(sc.step[f]);
        if (o < lo) lo = o;
        if (o > hi) hi = o;
    }
    lo -= kNoteFoldTolSt;
    hi += kNoteFoldTolSt;

    /* Octave-fold anything outside that compass back into it.
     *
     * Without this the search below simply clamps, and every voltage past
     * the top field returns the top field — on a Kurd pan that is dead from
     * 1.58 V all the way up, which is most of a sequencer's travel. The
     * symptom is that the CV moves and the pitch does not.
     *
     * The fold is deliberately *bounded* to outside the compass. Letting
     * every field compete at every octave looks tidier and is wrong: the
     * fifth between the ding and the first rim note would be filled by
     * wrapped copies of the upper fields, so 0.09 V would play the sixth
     * field instead of the ding. Inside its own range the pan must quantise
     * to the note that is actually nearest, and only outside it may an
     * octave be borrowed. The tolerance keeps a request a whisker flat of
     * the ding from being thrown an octave up.
     *
     * Every scale here spans more than an octave, so one step always lands
     * inside; the counters are guards against a pathological input, not
     * part of the arithmetic. */
    for (uint8_t n = 0; n < 24u && st > hi; n++) st -= 12.f;
    for (uint8_t n = 0; n < 24u && st < lo; n++) st += 12.f;

    uint8_t best = 0;
    float   bd   = 1e9f;
    for (uint8_t f = 0; f < kNumFields; f++)
    {
        const float d = fabsf(st - static_cast<float>(sc.step[f]));
        if (d < bd) { bd = d; best = f; }
    }
    return best;
}

float FieldHz(uint8_t field)
{
    return field < kNumFields ? f_hz_[field] : 0.f;
}

float FieldEnergy(uint8_t field)
{
    if (field >= kNumFields) return 0.f;
    const float e = f_energy_[field] * 9.f;
    return e > 1.f ? 1.f : e;
}

float RingEnergy()
{
    float s = 0.f;
    for (uint8_t f = 0; f < kNumFields; f++) s += f_energy_[f];
    s *= 3.2f;
    return s > 1.f ? 1.f : s;
}

/* ═══ Audio ═════════════════════════════════════════════════════════════ */

void Process(const float* ext, float* out_l, float* out_r, size_t n)
{
    /* --- drain the strike queue ---------------------------------------- */
    while (sq_r_ != sq_w_)
    {
        const StrikeReq& s = sq_[sq_r_ & (kStrikeQueueLen - 1)];
        FireStrike(s.field, s.vel);
        sq_r_++;
    }

    /* --- amortised retune ----------------------------------------------
     * Global scalars only when something actually moved, but the tone
     * fields are rebuilt one per block, round robin, unconditionally.
     *
     * The obvious design — reset a cursor to zero whenever a parameter
     * changes and walk it to the end — is wrong on this hardware. Control
     * hooks run at 1 kHz and audio blocks at 2 kHz, so a parameter that
     * moves continuously (a palm mute ramping in, a knob being swept)
     * restarts the cursor every second block, and fields 2 through 8 are
     * never reached at all: half the instrument would mute and the other
     * half would not. A free-running cursor cannot starve. It costs a
     * dozen exp/sincos per block whether or not anything changed, which is
     * under a percent, and it bounds staleness at nine blocks — 4.5 ms,
     * well under the ear's threshold for a knob turn. */
    if (dirty_ != seen_)
    {
        seen_ = dirty_;
        RebuildGlobal();
    }
    RebuildField(retune_cursor_);
    retune_cursor_ = static_cast<uint8_t>((retune_cursor_ + 1u) % kNumFields);

    /* --- bloom: rotate the live coefficients off the nominal pair -------
     * The offset is at most a few tens of cents, so the small-angle form
     * needs no trig in the hot path. It must be normalised, though: the
     * unnormalised rotation (1, th) has magnitude sqrt(1 + th^2), and th is
     * w * offset, which for a high mode at full bloom reaches ~0.03 rad.
     * That is enough to push a long-ringing pole (r ~ 0.9998) past 1, and
     * the mode then grows every sample the bloom lasts — under repeated
     * hard strikes, all the way to NaN. 1 - th^2/2 is 1/sqrt(1 + th^2) to
     * fourth order, so the pole's radius stays r. */
    for (uint8_t f = 0; f < kNumFields; f++)
    {
        if (f_bloom_[f] <= 1e-6f) continue;

        const float d    = f_bloom_[f];
        const uint16_t b = static_cast<uint16_t>(f) * kModesPerField;
        for (uint8_t m = 0; m < kModesPerField; m++)
        {
            const uint16_t i = b + m;
            const float th = m_w_[i] * d;
            const float nk = 1.f - 0.5f * th * th;
            m_cr_[i] = (m_cr0_[i] - m_ci0_[i] * th) * nk;
            m_ci_[i] = (m_ci0_[i] + m_cr0_[i] * th) * nk;
        }

        f_bloom_[f] *= bloom_decay_;
        if (f_bloom_[f] <= 1e-6f)
        {
            f_bloom_[f] = 0.f;
            for (uint8_t m = 0; m < kModesPerField; m++)
            {
                m_cr_[b + m] = m_cr0_[b + m];
                m_ci_[b + m] = m_ci0_[b + m];
            }
        }
    }

    /* --- is anything driving the bank this block? ----------------------- */
    bool driving = (ext != nullptr);
    for (uint8_t i = 0; i < kNumExciters && !driving; i++)
        if (ex_[i].left > 0 || ex_[i].n_env > 1e-4f) driving = true;

    const float cav_lvl  = a_cavlvl_;
    const float cav_send = 0.55f * a_cavlvl_;   /* cavity -> fields */
    const float contact  = a_contact_;          /* bare tak in the mix  */

    for (size_t s = 0; s < n; s++)
    {
        /* ── excitation ─────────────────────────────────────────────── */
        float dir[kNumFields];
        float sym[kNumFields];
        for (uint8_t f = 0; f < kNumFields; f++) { dir[f] = 0.f; sym[f] = 0.f; }

        float cav_in = 0.f;
        float bare   = 0.f;

        for (uint8_t i = 0; i < kNumExciters; i++)
        {
            Exciter& e = ex_[i];
            if (e.left <= 0 && e.n_env <= 1e-4f) continue;

            float v = 0.f;

            if (e.left > 0)
            {
                /* Half-sine force pulse. Its width *is* the mallet. */
                v = sinf(3.14159265f * e.ph) * e.gain;
                e.ph += e.ph_inc;
                e.left--;
            }

            float nz = 0.f;
            if (e.n_env > 1e-4f)
            {
                /* Band-limited contact noise, 2-pole SVF. */
                const float w  = Rnd() * e.n_env * e.n_gain;
                const float hp = w - e.bp_lp - e.bp_q * e.bp_bp;
                e.bp_bp += e.bp_f * hp;
                e.bp_lp += e.bp_f * e.bp_bp;
                nz       = e.bp_bp;
                e.n_env *= e.n_dec;
            }

            const float drive = v + nz * 0.9f;
            bare   += nz;
            cav_in += v;                          /* the air feels the force */

            dir[e.field] += drive;
            const float* cw = couple_[e.field];
            for (uint8_t f = 0; f < kNumFields; f++) sym[f] += drive * cw[f];
        }

        /* External exciter: any audio at all, straight into every field. */
        if (ext)
        {
            const float x = ext[s] * a_ext_;
            for (uint8_t f = 0; f < kNumFields; f++) sym[f] += x;
            cav_in += x * 0.5f;
        }

        /* ── Helmholtz cavity ───────────────────────────────────────── */
        float cav = 0.f;
        if (cav_lvl > 0.f)
        {
            const float hp = cav_in - cav_lp_ - cav_q_ * cav_bp_;
            cav_bp_ += cav_f_ * hp;
            cav_lp_ += cav_f_ * cav_bp_;
            cav      = cav_bp_;

            /* The air column pushes back on every field. Feedforward only:
             * the cavity is driven by the strike, never by the fields. */
            const float back = cav * cav_send;
            for (uint8_t f = 0; f < kNumFields; f++) sym[f] += back;
        }

        /* ── the mode bank ──────────────────────────────────────────── */
        float l = 0.f, r = 0.f;

        for (uint8_t f = 0; f < kNumFields; f++)
        {
            if (!driving && f_energy_[f] < kEnergyFloor) continue;

            const float dd = dir[f];
            const float ss = sym[f];
            const uint16_t b = static_cast<uint16_t>(f) * kModesPerField;

            float acc_a = 0.f, acc_b = 0.f;

            for (uint8_t m = 0; m < kModesPerField; m++)
            {
                const uint16_t i = b + m;

                const float x  = dd * m_exd_[i] + ss * m_exs_[i];
                const float zr = m_zr_[i];
                const float zi = m_zi_[i];
                const float cr = m_cr_[i];
                const float ci = m_ci_[i];

                const float nr = zr * cr - zi * ci + x;
                const float ni = zr * ci + zi * cr;

                m_zr_[i] = nr;
                m_zi_[i] = ni;

                if (m & 1u) acc_b += ni; else acc_a += ni;
            }

            const float mono = acc_a + acc_b;
            const float mag  = mono < 0.f ? -mono : mono;
            f_energy_[f] += (mag - f_energy_[f]) * energy_coeff_;

            l += acc_a * f_panLa_[f] + acc_b * f_panLb_[f];
            r += acc_a * f_panRa_[f] + acc_b * f_panRb_[f];
        }

        /* ── mix ────────────────────────────────────────────────────── */
        const float body = cav * cav_lvl * 1.7f;
        const float tak  = bare * contact;

        const float g  = kOutTrim * a_level_;
        float yl = (l + body + tak) * g;
        float yr = (r + body + tak) * g;

        /* DC blocker — cheap insurance; the cavity SVF can drift. */
        const float nyl = yl - dc_xl_ + 0.9995f * dc_yl_;
        dc_xl_ = yl; dc_yl_ = nyl;
        const float nyr = yr - dc_xr_ + 0.9995f * dc_yr_;
        dc_xr_ = yr; dc_yr_ = nyr;

        out_l[s] = SoftClip(nyl);
        out_r[s] = SoftClip(nyr);
    }
}

} // namespace handpan_dsp
