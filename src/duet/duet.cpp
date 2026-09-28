/**
 * duet.cpp — two handpans that answer each other through the Mega.
 *
 * A handpan firmware plus one page. Run it on two (or three) Alchemy Labs
 * on the panel I2C bus, each at its own address, with mega/duet on the
 * Mega. Play one pan and the other answers.
 *
 * How the answer travels. The modules are all I2C slaves and cannot reach
 * each other, so the Mega relays:
 *
 *   lab A: a strike you played (STRIKE jack or B3)
 *          -> EVENT [PLAY, field, vel]            panel_i2c::SendEvent
 *   Mega:  reads it on its next poll, draws it, writes to every other lab
 *          -> [HEAR, field, vel]                  panel_i2c::ReceiveMessage
 *   lab B: its Duet page decides whether and how to answer, schedules the
 *          strikes itself, and reports each one as EVENT [ANSWER, ...] so
 *          the screen can draw it. ANSWER is never relayed, so two labs
 *          both set to answer cannot feed back into each other.
 *
 * The answer's timing is kept on the answering module, against its own
 * millisecond clock, not on the Mega, whose loop stalls for a TFT redraw.
 * The relay itself adds 5-20 ms, which sits inside the shortest Delay.
 *
 * A field is sent, not a pitch. Each pan plays the answer in its own scale
 * and root, so two labs on different tunings translate each other's phrase
 * rather than copy it.
 *
 * The sound is not copied - duet_dsp.cpp compiles
 * src/handpan/handpan_dsp.cpp in place, so the engine stays one file.
 *
 * A physically-modelled hang drum. Nine tone fields — a centre ding and
 * eight rim notes — each a bank of modal resonators tuned the way a real
 * pan is hammered: three modes locked to f, 2f and 3f. Every field is
 * always ringing, so a strike on one is heard through all the others and
 * there is no such thing as running out of voices.
 *
 * Playing it:
 *
 *   STRIKE (J1)   a trigger. Its *peak height* is the velocity, so a
 *                 velocity-scaled trigger plays dynamics the way a hand
 *                 does. J1 is the AC-coupled codec input, which is
 *                 exactly right for edges and useless for DC.
 *   NOTE (J3)     1 V/oct, quantised to the nine tone fields of the
 *                 current tuning. 0 V is the ding.
 *   B3            tap to strike the selected field; keep your hand on it
 *                 and the shell mutes, the way a palm on a real pan does.
 *   EXC (J2)      any audio at all, poured into every field. The module
 *                 becomes a resonator you can hit with a drum machine.
 *
 * Four pages: **Play** is the instrument, **Voicing** is the tuner's
 * bench, **Duet** is how this pan answers the other one, and **Build** —
 * held on B2 — is the set-and-forget end. B1 cycles Play, Voicing, Duet.
 *
 * The DSP core in handpan_dsp.* has no libDaisy dependency on purpose: it
 * also compiles natively, so the model can be rendered to a .wav and
 * judged by ear without a flash cycle. This file owns the board; that one
 * owns the sound.
 *
 * Mirror any control change into tools/descriptor_probe/mirrors/duet.cpp,
 * then run tools/descriptor_probe/run.sh duet.
 *
 * The Pager is load-bearing and must stay managed and attached — without
 * it the module reports no controls and the web programmer hangs at
 * "reconnect and verify firmware" after a successful flash. See the README.
 */

#include "daisy_seed.h"
#include "util/CpuLoadMeter.h"
#include "alchemy/hw/alchemy_lab.h"
#include "alchemy/host_link/host.h"
#include "alchemy/surface/control_loop.h"
#include "alchemy/surface/jack.h"
#include "alchemy/surface/manual.h"
#include "alchemy/surface/page.h"
#include "alchemy/surface/pager.h"
#include "alchemy/surface/presets.h"
#include "alchemy/surface/virtual_knob.h"

#include <cmath>

#include "common/panel_i2c.h"
#include "handpan/handpan_dsp.h"
#include "handpan/handpan_palette.h"

using namespace alchemy;
namespace hp = handpan_dsp;

/* ── Ranges ──────────────────────────────────────────────────────────── */

/* Ding fundamental. Real pans are built from about B2 up to A3; two
 * octaves either side of that covers every tuning anyone sells and leaves
 * room to detune the whole instrument into a bass drone. */
static constexpr float kRootMinHz = 65.41f;   /* C2 */
static constexpr float kRootMaxHz = 261.63f;  /* C4 */

/* Ring length of the ding, to -60 dB. The short end is a hand-damped
 * "tok"; the long end outlasts most phrases. Exponential, because all the
 * useful detail is at the short end. */
static constexpr float kDecayMinSec = 0.25f;
static constexpr float kDecayMaxSec = 16.f;

static constexpr float kTemperMax     = 0.4f;
static constexpr float kFineMaxCents  = 50.f;

/* Strike bloom is not its own knob. It is a velocity response — how far a
 * hard strike bends the pitch before it settles — and Dynamics is already
 * the control for how much velocity is allowed to change the sound beyond
 * its level. One knob for one idea. The scale is picked so the Dynamics
 * default lands on 18 cents, which is mid-range for a real pan. */
static constexpr float kBloomPerDyn   = 22.5f;

/* ── Strike input (J1) ───────────────────────────────────────────────── */

/* J1 is a codec input: +/-5 V at the panel is +/-1.0 in sample units. */
static constexpr float kJackFullScaleV = 5.f;
static constexpr float kTrigThreshV    = 1.0f;
static constexpr float kTrigThresh     = kTrigThreshV / kJackFullScaleV;

/* Hold the peak for a millisecond after the edge before deciding how hard
 * the strike was. That is the whole velocity mechanism: a trigger's height
 * is how hard the hand hit. One millisecond of latency is inaudible, and
 * the AC coupling has not begun to droop over that span. */
static constexpr float kPeakWindowMs  = 1.0f;
static constexpr float kRefractoryMs  = 6.0f;

/* A 4 V trigger drives the trigger's own share of the velocity to full. */
static constexpr float kFullVelVolts  = 4.0f;
static constexpr float kMinVel        = 0.05f;

/* Velocity a full-scale trigger produces ON ITS OWN, and deliberately not
 * 1.0.
 *
 * ACCENT *adds* to this, so if the trigger alone reached the ceiling there
 * would be nothing left to add into. That is exactly what used to happen:
 * the trigger's share was scaled straight to 1.0 and clamped there, so any
 * trigger above 3.2 V pinned velocity at maximum and the accent jack was
 * mathematically inert — you could sweep it over its whole range and hear
 * nothing. A fixed-height gate is the common case, not the exception, so
 * the base has to sit below the ceiling for accent to mean anything.
 *
 * 0.65 also puts an unaccented strike back among the velocities the model
 * was voiced against (the reference phrase runs 0.42 to 0.95); a hardwired
 * 1.0 was hitting the pan harder than anything it was tuned on. */
static constexpr float kVelFromTrig   = 0.65f;

/* Volts of ACCENT per unit of velocity. Chosen so a 0-5 V source spans
 * exactly the headroom above kVelFromTrig — full scale lands on 1.0 rather
 * than running into the clamp partway up. A bipolar source gets the same
 * authority downward. */
static constexpr float kAccentPerVolt = 0.07f;

/* B3 struck by hand, with no trigger to take a height from. */
static constexpr float kManualVel     = 0.75f;

/* Tap to strike, hold to mute. Below the onset it is purely a strike, so
 * a normal tap never damps; past it the palm settles over the ramp. */
static constexpr float kMuteOnsetMs   = 120.f;
static constexpr float kMuteRampMs    = 180.f;

/* Below this the external input is treated as unpatched, which lets the
 * engine skip silent tone fields instead of driving all nine. */
static constexpr float kExtSquelch    = 1e-4f;

/* ── CV jack assignments ─────────────────────────────────────────────── */
/*
 * hw.cv[i] is J3+i. J7 is claimed as an *output*, which is only possible
 * because every one of J3..J8 is a switchable jack on this board — the
 * DG411 either points the ADC at the panel or the DAC at it.
 */
static constexpr uint8_t kCvNote   = 0;  /* J3  1 V/oct, which tone field  */
static constexpr uint8_t kCvAccent = 1;  /* J4  +/- velocity offset        */
static constexpr uint8_t kCvPos    = 2;  /* J5  strike position            */
static constexpr uint8_t kCvDamp   = 3;  /* J6  palm mute                  */
static constexpr uint8_t kCvRingOut= 4;  /* J7  OUTPUT: ringing energy     */
static constexpr uint8_t kCvSymp   = 5;  /* J8  sympathy                   */

static_assert(kNumCvInputs > kCvSymp, "handpan needs all six CV jacks");

/* Note CV is clamped only to the panel's own span. It used to be pinched to
 * -2..+4 V on the reasoning that the tone fields span an octave and a half
 * so nothing outside could mean anything — which was exactly backwards. The
 * engine folds an out-of-compass request by octaves back onto the pan, so
 * every volt now picks a note; the clamp is only here to keep a floating
 * input from reaching the fold loop with a wild value. */
static constexpr float kNoteVoltsMin = -10.f;
static constexpr float kNoteVoltsMax =  10.f;

/* ── Pages ───────────────────────────────────────────────────────────── */

enum : uint8_t {
    kPagePlay = 0, kPageVoicing = 1, kPageDuet = 2, kPageBuild = 3, kNumPages = 4
};

static const char* const kScaleLabels[hp::kNumScales] = {
    "Kurd", "Celtic", "Hijaz", "Pygmy", "Dorian", "Major", "Harmonic", "Insen"
};

/* ── Play ────────────────────────────────────────────────────────────── */

static VirtualKnob k_scale = VirtualKnob(kPotTopLeft, "Scale")
    .Selector(hp::kNumScales).Labels(kScaleLabels)
    .Ident("pan.scale")
    .Ring(SelectorRing(kScaleOn, kScaleOff, hp::kNumScales));

static VirtualKnob k_root = VirtualKnob(kPotTopRight, "Root")
    .Exp(kRootMinHz, kRootMaxHz).Ident("pan.root").Unit("Hz")
    .Ring(Level(kRootColor).Passive(kRootPassive));

static VirtualKnob k_pos = VirtualKnob(kPotMiddleLeft, "Position")
    .Linear(0.f, 1.f).Ident("strike.position")
    .Cv(kCvPos)
    .Ring(Level(kPosColor).Passive(kPosPassive));

static VirtualKnob k_mallet = VirtualKnob(kPotMiddleRight, "Mallet")
    .Linear(0.f, 1.f).Ident("strike.mallet")
    .Ring(Level(kMalletColor).Passive(kMalletPassive));

static VirtualKnob k_decay = VirtualKnob(kPotBottomLeft, "Decay")
    .Exp(kDecayMinSec, kDecayMaxSec).Ident("shell.decay").Unit("s")
    .Ring(Level(kDecayColor).Passive(kDecayPassive));

static VirtualKnob k_symp = VirtualKnob(kPotBottomRight, "Sympathy")
    .Linear(0.f, 1.f).Ident("shell.sympathy")
    .Cv(kCvSymp)
    .Ring(Level(kSympColor).Passive(kSympPassive));

/* ── Voicing ─────────────────────────────────────────────────────────── */

/* Temper takes the top-left slot because it is the one control here that
 * changes what kind of instrument this is rather than what it sounds like.
 * Bipolar: dead centre is a real handpan's 1:2:3. */
static VirtualKnob k_temper = VirtualKnob(kPotTopLeft, "Temper")
    .Linear(-kTemperMax, kTemperMax).Ident("voice.temper")
    .Ring(Bipolar(kTemperUp, kTemperDown, kTemperCenter));

static VirtualKnob k_shimmer = VirtualKnob(kPotTopRight, "Shimmer")
    .Linear(0.f, 1.f).Ident("voice.shimmer")
    .Ring(Level(kShimmerColor).Passive(kShimmerPass));

static VirtualKnob k_metal = VirtualKnob(kPotMiddleLeft, "Metal")
    .Linear(0.f, 1.f).Ident("voice.metal")
    .Ring(Level(kMetalColor).Passive(kMetalPass));

static VirtualKnob k_tilt = VirtualKnob(kPotMiddleRight, "Tilt")
    .Linear(0.f, 1.f).Ident("voice.tilt")
    .Ring(Level(kTiltColor).Passive(kTiltPass));

static VirtualKnob k_cavlvl = VirtualKnob(kPotBottomLeft, "Body")
    .Linear(0.f, 1.f).Ident("voice.body")
    .Ring(Level(kCavColor).Passive(kCavPass));

static VirtualKnob k_contact = VirtualKnob(kPotBottomRight, "Contact")
    .Linear(0.f, 1.f).Ident("voice.contact")
    .Ring(Level(kContactColor).Passive(kContactPass));

/* ── Duet ────────────────────────────────────────────────────────────── */
/*
 * How this pan answers a strike it HEARS - one the Mega relayed from the
 * other lab. Nothing here changes how this pan responds to its own STRIKE
 * jack or B3; those are reported upward and answered over there.
 */

enum : uint8_t { kAnsOff = 0, kAnsCanon, kAnsMirror, kAnsScatter, kNumAnswers };

static const char* const kAnswerLabels[kNumAnswers] = {
    "Off", "Canon", "Mirror", "Scatter"
};

/* Interval is in tone fields, not semitones: the fields are the pan's scale,
 * so a step of two is "a third" in whatever tuning this pan is cut to. */
static constexpr int8_t kIntervalMax = 4;
static constexpr uint8_t kNumIntervals = 2 * kIntervalMax + 1;
static const char* const kIntervalLabels[kNumIntervals] = {
    "-4", "-3", "-2", "-1", "0", "+1", "+2", "+3", "+4"
};

static constexpr uint8_t kMaxRepeats = 4;
static const char* const kRepeatLabels[kMaxRepeats] = { "1", "2", "3", "4" };

/* The short end is a flam behind the call; the long end a phrase later. */
static constexpr float kDelayMinMs = 60.f;
static constexpr float kDelayMaxMs = 2000.f;

static constexpr Rgb kDuetOn    = {0xFF, 0x50, 0xC0};
static constexpr Rgb kDuetOff   = {0x16, 0x04, 0x10};
static constexpr Rgb kDuetColor = {0xFF, 0x70, 0xD0};
static constexpr Rgb kDuetPass  = {0x16, 0x06, 0x12};

static VirtualKnob k_answer = VirtualKnob(kPotTopLeft, "Answer")
    .Selector(kNumAnswers).Labels(kAnswerLabels)
    .Ident("duet.answer")
    .Ring(SelectorRing(kDuetOn, kDuetOff, kNumAnswers));

static VirtualKnob k_delay = VirtualKnob(kPotTopRight, "Delay")
    .Exp(kDelayMinMs, kDelayMaxMs).Ident("duet.delay").Unit("ms")
    .Ring(Level(kDuetColor).Passive(kDuetPass));

static VirtualKnob k_interval = VirtualKnob(kPotMiddleLeft, "Interval")
    .Selector(kNumIntervals).Labels(kIntervalLabels)
    .Ident("duet.interval")
    .Ring(SelectorRing(kDuetOn, kDuetOff, kNumIntervals));

static VirtualKnob k_repeats = VirtualKnob(kPotMiddleRight, "Repeats")
    .Selector(kMaxRepeats).Labels(kRepeatLabels)
    .Ident("duet.repeats")
    .Ring(SelectorRing(kDuetOn, kDuetOff, kMaxRepeats));

static VirtualKnob k_chance = VirtualKnob(kPotBottomLeft, "Chance")
    .Linear(0.f, 1.f).Ident("duet.chance")
    .Ring(Level(kDuetColor).Passive(kDuetPass));

static VirtualKnob k_ansvel = VirtualKnob(kPotBottomRight, "Answer Vel")
    .Linear(0.1f, 1.f).Ident("duet.velocity")
    .Ring(Level(kDuetColor).Passive(kDuetPass));

/* ── Build (held on B2) ──────────────────────────────────────────────── */

static VirtualKnob k_cavtune = VirtualKnob(kPotTopLeft, "Gu Tune")
    .Linear(0.5f, 1.8f).Ident("build.gu")
    .Ring(Level(kBuildColor).Passive(kBuildPass));

static VirtualKnob k_fine = VirtualKnob(kPotTopRight, "Fine")
    .Linear(-kFineMaxCents, kFineMaxCents).Ident("build.fine").Unit("cents")
    .Ring(Bipolar(kFineUp, kFineDown, kFineCenter));

static VirtualKnob k_spread = VirtualKnob(kPotMiddleLeft, "Spread")
    .Linear(0.f, 1.f).Ident("build.spread")
    .Ring(Level(kBuildColor).Passive(kBuildPass));

static VirtualKnob k_dyn = VirtualKnob(kPotMiddleRight, "Dynamics")
    .Linear(0.f, 1.f).Ident("build.dynamics")
    .Ring(Level(kBuildColor).Passive(kBuildPass));

static VirtualKnob k_ext = VirtualKnob(kPotBottomLeft, "Exciter In")
    .Linear(0.f, 1.f).Ident("build.ext")
    .Ring(Level(kBuildColor).Passive(kBuildPass));

static VirtualKnob k_level = VirtualKnob(kPotBottomRight, "Level")
    .Linear(0.f, 1.f).Ident("build.level")
    .Ring(Level(kBuildColor).Passive(kBuildPass));

static Page page_play = Page(kPagePlay).Name("Play").Color("#ffa830")
    .Knobs(k_scale, k_root, k_pos, k_mallet, k_decay, k_symp);

static Page page_voice = Page(kPageVoicing).Name("Voicing").Color("#90e0ff")
    .Knobs(k_temper, k_shimmer, k_metal, k_tilt, k_cavlvl, k_contact);

static Page page_duet = Page(kPageDuet).Name("Duet").Color("#ff70d0")
    .Knobs(k_answer, k_delay, k_interval, k_repeats, k_chance, k_ansvel);

static Page page_build = Page(kPageBuild).Name("Build").Color("#60ffc0")
    .Knobs(k_cavtune, k_fine, k_spread, k_dyn, k_ext, k_level);

static Page* const kPages[] = { &page_play, &page_voice, &page_duet, &page_build };

/* ── Jacks ───────────────────────────────────────────────────────────── */

static Jack j_strike("STRIKE", "Strike",       JackSig::Trig);
static Jack j_exc   ("EXC",    "Exciter In",   JackSig::AudioIn);
static Jack j_note  ("NOTE",   "Note",         JackSig::Voct);
static Jack j_accent("ACCENT", "Accent",       JackSig::CvBi);
static Jack j_pos   ("POS",    "Position CV",  JackSig::CvBi);
static Jack j_damp  ("DAMP",   "Damp CV",      JackSig::CvUni);
static Jack j_ring  ("RING",   "Ring Energy",  JackSig::CvUni);
static Jack j_symp  ("SYMP",   "Sympathy CV",  JackSig::CvBi);
static Jack j_out_l ("OUT_L",  "Out L",        JackSig::AudioOut);
static Jack j_out_r ("OUT_R",  "Out R",        JackSig::AudioOut);

/* ── Panel display ───────────────────────────────────────────────────── */

/* What B1/B2/B3 do on each page, in page-index order. The knob legends come
 * off the VirtualKnobs themselves, but the buttons are read directly from
 * hw.buttons rather than declared as VirtualButtons, so their meaning lives
 * here. B3 is the palm mute everywhere except Build, where OnRender turns it
 * into the audio-load meter. */
static const char* const kBtnLabels[kNumPages * 3] = {
    "PAGE", "BUILD", "HAND",   /* Play    */
    "PAGE", "BUILD", "HAND",   /* Voicing */
    "PAGE", "BUILD", "HAND",   /* Duet    */
    "",     "HELD",  "LOAD",   /* Build   */
};

static const Jack* const kJackOrder[] = {
    &j_strike, &j_note, &j_pos,    &j_ring, &j_out_l,
    &j_exc,    &j_accent, &j_damp, &j_symp, &j_out_r,
};

/* ── SDK surfaces ────────────────────────────────────────────────────── */

static AlchemyLab  hw;
static ControlLoop loop(hw);
static Pager       pager(kNumPages, kNumPots);
static Presets     presets(hw.seed.qspi);

/* Its own id, not handpan's: the extra page changes the preset schema, so a
 * handpan preset would not load here anyway. To debug the link, pass
 * panel_i2c::Diag() here instead of "i2c" and HELLO carries a live I2C4
 * register dump (tools/hostlink_reboot.py hello <port>). */
static hostlink::Host host(presets, "duet", "Duet", "0.1.0", "i2c");

/* ── Shared control state ────────────────────────────────────────────── */

/* Which tone field the note CV is asking for. Written on the 1 ms poll,
 * read by the audio callback when a strike arrives — one byte, so it
 * cannot tear. */
static volatile uint8_t cur_field_ = 0;

/* Velocity offset from the accent jack, same deal. */
static volatile float   accent_ = 0.f;

/* B3's strike, as a request counter rather than a direct hp::Strike().
 *
 * The engine's strike queue is single-producer, and the trigger jack is
 * already striking from the audio callback — so a B3 press handled on the
 * control poll would be a second producer on a queue with one write cursor,
 * and a button pressed on the same millisecond as a trigger could hand both
 * the same slot and swallow a note. The control side only ever increments;
 * the audio callback acknowledges and does the striking, so there stays
 * exactly one writer. */
static volatile uint32_t manual_req_ = 0u;
static uint32_t          manual_ack_ = 0u;

/* Audio load. A hundred-odd high-Q resonators is the most expensive thing
 * this board has been asked to run, and the honest way to know whether it
 * fits is to measure it on the hardware rather than to reason about a flop
 * count.
 *
 * Shown on B3, but only while the Build layer is held — Build is the bench
 * page, and a diagnostic belongs there rather than permanently occupying a
 * performance button's LED. The rest of the time B3 shows what B3 does.
 *
 * Not B1 or B2, because the control loop paints those itself with the
 * Pager's navigation indicator; knowing which page you are on beats knowing
 * the load. */
static daisy::CpuLoadMeter cpu_;

/* Peak-hold over the meter's smoothed average: rises at once, falls back
 * slowly.
 *
 * It is updated on every render tick, not only while it is being shown.
 * The meter lives on a *held* layer, so a reading that only accumulated
 * while B2 was down could never catch the load of actually playing — you
 * cannot hold the layer and play with both hands. Running it all the time
 * and holding the peak means the gesture is: play hard, then hold B2 and
 * read what that playing cost.
 *
 * Deliberately NOT CpuLoadMeter::GetMaxCpuLoad(), which is the maximum
 * since the last Reset() — and nothing ever calls Reset(). The worst single
 * block since power-on (cold cache, the first retune, the boot preset load)
 * latches into it permanently and the reading can then only ever climb, so
 * the LED settles on one colour within a second of boot and never moves
 * again. A meter that cannot fall is not a meter. */
static float load_meter_ = 0.f;

/* Fall per render tick. At the 16 ms frame that is a ~1.3 s time constant,
 * so a peak stays readable for the few seconds it takes to stop playing
 * and reach for B2. */
static constexpr float kLoadFall = 0.012f;

/* Palm-mute amount, shared from the poll so the render hook can colour B3
 * with it. One float, written on one thread. */
static volatile float damp_now_ = 0.f;

/* Strike detector state. Audio thread only. */
static bool    trig_armed_ = false;
static bool    trig_hi_    = false;
static float   trig_peak_  = 0.f;
static int32_t trig_count_ = 0;
static int32_t trig_refr_  = 0;
static int32_t peak_win_   = 48;
static int32_t refr_len_   = 288;

/* ── Duet: messages and the answer scheduler ─────────────────────────── */

/* Wire format, shared with mega/duet/duet.ino. Up (EVENT payload) and down
 * (master write) are both three bytes: kind, field, velocity 0..255. */
static constexpr uint8_t kEvtPlay   = 0x01;  /* up: a strike played here    */
static constexpr uint8_t kEvtAnswer = 0x02;  /* up: this pan answering      */
static constexpr uint8_t kCmdHear   = 0x20;  /* down: the other pan played  */

/**
 * One-writer, one-reader FIFO of strikes. Two of them, because strikes cross
 * threads both ways: a STRIKE-jack hit is found on the audio thread and has
 * to be reported from the control thread, and an answer is scheduled on the
 * control thread and has to be struck on the audio thread (the engine's
 * strike queue has one producer, and it is the audio callback).
 */
struct StrikeFifo
{
    static constexpr uint8_t kSize = 16;   /* power of two */
    struct Hit { uint8_t field; float vel; };
    Hit              q[kSize];
    volatile uint8_t head = 0, tail = 0;

    bool Push(uint8_t field, float vel)
    {
        const uint8_t h = head, next = (uint8_t)((h + 1u) & (kSize - 1u));
        if (next == tail) return false;
        q[h] = {field, vel};
        __DMB();
        head = next;
        return true;
    }
    bool Pop(Hit& out)
    {
        const uint8_t t = tail;
        if (t == head) return false;
        __DMB();
        out  = q[t];
        tail = (uint8_t)((t + 1u) & (kSize - 1u));
        return true;
    }
};

static StrikeFifo played_;    /* audio -> control: jack strikes to report */
static StrikeFifo answers_;   /* control -> audio: answers to strike      */

/* Answers waiting for their moment. Control thread only. Sized for four
 * repeats of a fast phrase landing inside the longest delay; if it fills,
 * new answers are dropped rather than old ones cut short. */
struct Pending { uint32_t due; uint8_t field; float vel; bool used; };
static constexpr uint8_t kMaxPending = 32;
static Pending pending_[kMaxPending];

/* Each repeat is quieter than the one before, like an echo. */
static constexpr float kRepeatFall = 0.72f;

static uint32_t rng_ = 0x2545F491u;
static uint32_t Rand()
{
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return rng_;
}
static float Rand01() { return (float)(Rand() >> 8) * (1.f / 16777216.f); }

/* Walk off either end of the pan and you bounce back: ding, rim 1..8, then
 * back down. Wrapping instead would jump from the top note to the ding. */
static uint8_t Reflect(int f)
{
    const int top    = hp::kNumFields - 1;
    const int period = 2 * top;
    f %= period;
    if (f < 0) f += period;
    return (uint8_t)(f <= top ? f : period - f);
}

static uint8_t AnswerField(uint8_t mode, uint8_t heard, int interval, uint8_t rep)
{
    const int top = hp::kNumFields - 1;
    switch (mode)
    {
        /* Each repeat climbs another interval, so Repeats > 1 turns one
         * note into a short arpeggio off it. Interval 0 is a plain echo. */
        case kAnsCanon:  return Reflect(heard + interval * (rep + 1));
        /* Low answers high: the ding answers with the top note. */
        case kAnsMirror: return Reflect(top - heard + interval * rep);
        default:         return (uint8_t)(Rand() % hp::kNumFields);
    }
}

static void Schedule(uint32_t due, uint8_t field, float vel)
{
    for (uint8_t i = 0; i < kMaxPending; i++)
    {
        if (!pending_[i].used)
        {
            pending_[i] = {due, field, vel, true};
            return;
        }
    }
}

static void SendStrikeEvent(uint8_t kind, uint8_t field, float vel)
{
    const float   v   = vel < 0.f ? 0.f : (vel > 1.f ? 1.f : vel);
    const uint8_t b[3] = {kind, field, (uint8_t)(v * 255.f + 0.5f)};
    panel_i2c::SendEvent(b, sizeof b);
}

/* The other pan played (field, vel). Decide, once, whether and how to
 * answer, and put every repeat on the schedule. */
static void Hear(uint32_t now, uint8_t field, float vel)
{
    const uint8_t mode = static_cast<uint8_t>(k_answer.Value());
    if (mode == kAnsOff || mode >= kNumAnswers) return;
    if (field >= hp::kNumFields) return;
    if (Rand01() >= k_chance.Value()) return;

    const float   delay    = k_delay.Value();
    const int     interval = static_cast<int>(k_interval.Value()) - kIntervalMax;
    const uint8_t reps     = static_cast<uint8_t>(k_repeats.Value()) + 1u;

    float v = vel * k_ansvel.Value();
    for (uint8_t r = 0; r < reps && r < kMaxRepeats; r++)
    {
        const uint32_t due = now + static_cast<uint32_t>(delay * (float)(r + 1));
        Schedule(due, AnswerField(mode, field, interval, r), v);
        v *= kRepeatFall;
    }
}

/* 1 ms: hear, fire what is due, report what was played. */
static void DuetPoll(uint32_t now)
{
    uint8_t msg[panel_i2c::kMaxMsg];
    uint8_t len = 0;
    while (panel_i2c::ReceiveMessage(msg, len))
    {
        if (len >= 3 && msg[0] == kCmdHear)
            Hear(now, msg[1], (float)msg[2] * (1.f / 255.f));
    }

    for (uint8_t i = 0; i < kMaxPending; i++)
    {
        Pending& p = pending_[i];
        if (!p.used || (int32_t)(now - p.due) < 0) continue;
        p.used = false;
        if (answers_.Push(p.field, p.vel))
            SendStrikeEvent(kEvtAnswer, p.field, p.vel);
    }

    StrikeFifo::Hit h;
    while (played_.Pop(h)) SendStrikeEvent(kEvtPlay, h.field, h.vel);
}

/* ── Ring overlays ───────────────────────────────────────────────────── */

/**
 * On the Root knob: a pip marking which of the nine tone fields the note
 * input has selected, brightening as that field rings. It is the one
 * piece of state a player cannot otherwise see — the pan has nine notes
 * and the panel has six knobs.
 */
static void DrawSelectedField(LedPanel& panel, uint8_t pot,
                              const ArcGeometry& geo, float /*norm*/,
                              uint32_t /*t_ms*/, void* /*ctx*/)
{
    const uint8_t f = cur_field_;
    if (f >= hp::kNumFields) return;

    const float span = static_cast<float>(geo.arc_leds - 1) * geo.step_hours;
    const float hour = geo.start_hour
                     + span * (static_cast<float>(f)
                               / static_cast<float>(hp::kNumFields - 1));

    const float e = hp::FieldEnergy(f);
    panel.SetRingByHour(pot, hour, LedPanel::Scale(kRingGlow, 0.25f + 0.75f * e));
}

/**
 * On the Sympathy knob: the whole instrument's ringing energy as an arc.
 * Sympathy is the control that decides how much of the pan answers a
 * strike, so its ring showing how much *is* answering reads correctly.
 */
static void DrawRingEnergy(LedPanel& panel, uint8_t pot,
                           const ArcGeometry& geo, float /*norm*/,
                           uint32_t /*t_ms*/, void* /*ctx*/)
{
    const float e = hp::RingEnergy();
    if (e < 0.02f) return;

    const uint8_t n = static_cast<uint8_t>(e * static_cast<float>(geo.arc_leds));
    for (uint8_t i = 0; i < n && i < geo.arc_leds; i++)
    {
        panel.SetRingByHour(pot,
                            geo.start_hour + static_cast<float>(i) * geo.step_hours,
                            LedPanel::Scale(kRingGlow, 0.10f + 0.35f * e));
    }
}

/* ── Audio ───────────────────────────────────────────────────────────── */

/**
 * Strike detection lives here rather than on the control poll because the
 * trigger's *height* is the velocity, and height needs samples. Reading
 * J1 at audio rate gets the edge inside half a millisecond and gets the
 * peak for free; polling at 1 ms would get neither reliably.
 */
static void Audio(daisy::AudioHandle::InputBuffer  in,
                  daisy::AudioHandle::OutputBuffer out,
                  size_t                           n)
{
    cpu_.OnBlockStart();

    for (size_t i = 0; i < n; i++)
    {
        const float x = in[0][i];

        if (trig_refr_ > 0) trig_refr_--;

        const bool hi = (x > kTrigThresh);

        if (trig_armed_)
        {
            if (x > trig_peak_) trig_peak_ = x;
            if (--trig_count_ <= 0)
            {
                /* Clamp the trigger's own contribution BEFORE the accent
                 * is added, so a hot trigger saturates its share without
                 * swallowing the accent's. */
                float t = (trig_peak_ * kJackFullScaleV) / kFullVelVolts;
                if (t > 1.f) t = 1.f;

                float v = t * kVelFromTrig + accent_;
                if (v < kMinVel) v = kMinVel;
                if (v > 1.f)     v = 1.f;

                hp::Strike(cur_field_, v);
                played_.Push(cur_field_, v);   /* for the other pan to hear */
                trig_armed_ = false;
                trig_refr_  = refr_len_;
            }
        }
        else if (hi && !trig_hi_ && trig_refr_ == 0)
        {
            trig_armed_ = true;
            trig_peak_  = x;
            trig_count_ = peak_win_;
        }

        trig_hi_ = hi;
    }

    /* B3, struck from here rather than from the poll so the engine's
     * single-producer strike queue keeps exactly one producer. */
    const uint32_t req = manual_req_;
    if (req != manual_ack_)
    {
        manual_ack_ = req;
        hp::Strike(cur_field_, kManualVel);
    }

    /* Answers to the other pan, scheduled on the control thread. */
    StrikeFifo::Hit a;
    while (answers_.Pop(a)) hp::Strike(a.field, a.vel);

    /* J2 is the external exciter. Handing the engine a null pointer when
     * nothing is patched is not just tidiness — it is what lets it skip
     * tone fields that have gone quiet, instead of driving all nine
     * forever. */
    const float* ext = nullptr;
    for (size_t i = 0; i < n; i++)
    {
        if (in[1][i] > kExtSquelch || in[1][i] < -kExtSquelch) { ext = in[1]; break; }
    }

    hp::Process(ext, out[0], out[1], n);

    cpu_.OnBlockEnd();
}

/* ── Control ─────────────────────────────────────────────────────────── */

/*
 * Note CV, accent and the palm mute run on the 1 ms poll rather than the
 * 16 ms frame. A sequencer moves pitch and trigger together, so reading
 * the note at frame rate would let a strike land on the previous note's
 * field for up to a frame — a wrong note, on every fast passage.
 */
static void OnPoll(uint32_t t_ms)
{
    float volts = hw.cv[kCvNote].Volts();
    if (volts < kNoteVoltsMin) volts = kNoteVoltsMin;
    if (volts > kNoteVoltsMax) volts = kNoteVoltsMax;
    cur_field_ = hp::FieldForVolts(volts);

    /* Unpatched sits at 0 V, so the trigger's own height stands alone. */
    accent_ = hw.cv[kCvAccent].Volts() * kAccentPerVolt;

    /* Palm mute: the damp jack, or B3 held. Tap it and you get a strike
     * with no mute at all; leave your hand there and the shell closes
     * down, which is what a hand on a real pan does. */
    float damp = hw.cv[kCvDamp].Volts() * (1.f / kJackFullScaleV);
    if (damp < 0.f) damp = 0.f;
    if (damp > 1.f) damp = 1.f;

    IButton& b3 = hw.buttons[kButtonB3];
    if (b3.RisingEdge())
    {
        manual_req_++;
        /* Reported from here rather than from the audio callback that
         * strikes it: the field is the same one, read a moment earlier. */
        SendStrikeEvent(kEvtPlay, cur_field_, kManualVel);
    }
    if (b3.Pressed())
    {
        const float held = b3.TimeHeldMs() - kMuteOnsetMs;
        if (held > 0.f)
        {
            float d = held / kMuteRampMs;
            if (d > 1.f) d = 1.f;
            if (d > damp) damp = d;
        }
    }
    hp::SetHandDamp(damp);
    damp_now_ = damp;

    /* Ring energy out on J7 — a 0..5 V envelope of the whole instrument,
     * for opening a filter or feeding a reverb send in time with the
     * decay. Poll rate, because a 16 ms envelope would step audibly. */
    hw.cv[kCvRingOut].SetVolts(hp::RingEnergy() * 5.f);

    DuetPoll(t_ms);
}

/**
 * B3's LED.
 *
 * This runs on the render hook rather than the poll because the control loop
 * paints the navigation indicators during render, and anything written from
 * a poll would be overwritten before it reached the strip. OnRender fires
 * after them.
 */
static void OnRender(uint32_t /*t_ms*/)
{
    /* Track the load whether or not anyone is looking. */
    float l = cpu_.GetAvgCpuLoad();
    /* NAN until the first audio block has been timed, and a NaN loses every
     * comparison below — which would read as a confident green. */
    if (!(l >= 0.f)) l = 0.f;
    load_meter_ = (l > load_meter_)
                    ? l
                    : load_meter_ + (l - load_meter_) * kLoadFall;

    /* Hold B2 and B3 becomes the audio-load meter: green under half load,
     * amber to three quarters, red past it.
     *
     * All three are painted at full brightness. An earlier green of
     * 0x006018 was a third the brightness of everything else on this
     * button, so against the bright blue of a ringing pan it read as the
     * light going *out* rather than changing colour — which is exactly how
     * it was reported. A state indicator has to differ in hue, not in how
     * dim it is. */
    if (pager.ActivePage() == kPageBuild)
    {
        hw.leds.SetButtonPair(kButtonB3,
                              (load_meter_ > 0.75f) ? Rgb{0xFF, 0x18, 0x00}
                            : (load_meter_ > 0.50f) ? Rgb{0xFF, 0xA0, 0x00}
                                                    : Rgb{0x30, 0xFF, 0x40});
        return;
    }

    /* Otherwise it shows what the button does. Amber while the palm is on
     * the shell, and the rest of the time it breathes with the instrument —
     * dim at rest, bright while the pan is ringing. A performance button's
     * light should answer the playing, not report a statistic. */
    const float d = damp_now_;
    if (d > 0.02f)
    {
        hw.leds.SetButtonPair(kButtonB3,
                              LedPanel::Scale(Rgb{0xFF, 0x60, 0x00},
                                              0.25f + 0.75f * d));
    }
    else
    {
        hw.leds.SetButtonPair(kButtonB3,
                              LedPanel::Scale(Rgb{0x60, 0xC0, 0xFF},
                                              0.06f + 0.94f * hp::RingEnergy()));
    }
}

/* Everything a knob owns. Retuning is amortised inside the engine — one
 * tone field per audio block — so pushing the whole parameter set every
 * frame costs nothing but a handful of stores. */
static void OnFrame()
{
    hp::SetScale(static_cast<uint8_t>(k_scale.Value()));
    hp::SetRootHz(k_root.Value() * exp2f(k_fine.Value() * (1.f / 1200.f)));
    hp::SetPosition(k_pos.Value());
    hp::SetMallet(k_mallet.Value());
    hp::SetDecay(k_decay.Value());
    hp::SetSympathy(k_symp.Value());

    hp::SetTemper(k_temper.Value());
    hp::SetShimmer(k_shimmer.Value());
    hp::SetMetal(k_metal.Value());
    hp::SetTilt(k_tilt.Value());
    hp::SetCavityLevel(k_cavlvl.Value());
    hp::SetContact(k_contact.Value());

    hp::SetCavityTune(k_cavtune.Value());
    hp::SetSpread(k_spread.Value());
    hp::SetDynamics(k_dyn.Value());
    hp::SetBloom(k_dyn.Value() * kBloomPerDyn);
    hp::SetExtLevel(k_ext.Value());
    hp::SetLevel(k_level.Value());

    panel_i2c::Tick();
}

/* ── Manual ──────────────────────────────────────────────────────────── */

static Manual manual =
    Manual()
        .Tagline("Two handpans that answer each other")
        .Preamble(
            "**Handpan** is a modelled hang drum: nine tone fields, a steel "
            "shell and an air cavity, all ringing at once. Each field is "
            "tuned the way a pan maker hammers one — its first three modes "
            "locked to **f, 2f and 3f** — and a strike on any field is "
            "carried through the shell into every other field, which is "
            "the halo you hear around a real instrument.\n\n"
            "Nothing is ever cut off to make room: all nine fields are "
            "permanently resonating, so there is no voice count and no "
            "note stealing. Patch a trigger to **STRIKE** and a 1 V/oct "
            "sequence to **NOTE**, or just tap **B3**.\n\n"
            "**Duet** runs on two Alchemy Labs sharing the TFT panel's I2C "
            "bus. Whatever you play on one is relayed by the panel to the "
            "other, which answers it according to its **Duet** page.")
        .Section("duet", "Duet — the answer",
                 "Every strike you play here, from STRIKE or B3, is sent to "
                 "the panel, which passes it to the other pan. That pan's "
                 "Duet page decides what comes back. Its answers are not "
                 "passed on again, so two pans both set to answer trade "
                 "phrases instead of feeding back.\n\n"
                 "**Answer** picks the rule. **Canon** repeats the note "
                 "moved by **Interval** tone fields; with Repeats above one, "
                 "each repeat climbs another interval, so one strike becomes "
                 "a short arpeggio. Interval 0 is a plain echo. **Mirror** "
                 "answers low with high: the ding with the top note, the top "
                 "note with the ding. **Scatter** answers with any field at "
                 "random. **Off** listens and does nothing.\n\n"
                 "**Delay** is the gap to the first answer, and between "
                 "repeats, from 60 ms to 2 s. **Chance** is how often it "
                 "answers at all. **Answer Vel** scales the velocity it "
                 "heard, and each repeat is softer than the one before.\n\n"
                 "The answer names a tone field, not a pitch, so a pan cut "
                 "to a different scale or root plays the same shape in its "
                 "own tuning.")
        .Section("strike", "Strike and velocity",
                 "STRIKE takes a trigger, and the *height* of that trigger "
                 "is how hard the pan is hit — a 4 V edge is a full-force "
                 "strike, 1 V is a fingertip. That is not a convenience: "
                 "velocity changes the timbre, not just the level, because "
                 "a harder contact is a shorter one and drives the shell's "
                 "untuned modes far more. Feed it a velocity-scaled trigger "
                 "and the instrument breathes; feed it a flat gate and "
                 "every strike is identical.\n\n"
                 "A plain fixed-height gate gives every strike the same "
                 "velocity, which is where **ACCENT** comes in: it adds to "
                 "what the trigger asked for, and a full-scale 0-5 V source "
                 "spans exactly the headroom left above it. Put it on a "
                 "loop length that does not divide the note pattern and the "
                 "dynamics drift against the melody.")
        .Section("note", "Note",
                 "NOTE is 1 V/oct, quantised to the nine tone fields of the "
                 "current tuning — 0 V is the ding, and the rim notes climb "
                 "from there. Anything between fields snaps to the nearest, "
                 "so an unquantised sequence still lands on the "
                 "instrument.\n\n"
                 "A pan has nine notes and cannot grow more, so a request "
                 "above the top field or below the ding is **folded by "
                 "octaves back into the instrument's compass** rather than "
                 "stopping at the end. A melody that runs past the top of "
                 "the pan keeps playing, an octave lower — which is what a "
                 "player does with a tune too wide for the instrument in "
                 "front of them.\n\n"
                 "Inside the pan's own range nothing is folded: the note "
                 "you get is the one actually nearest. Note that the gap "
                 "between the ding and the first rim note is a fifth in "
                 "most of these tunings, so about half a volt above 0 V all "
                 "plays the ding. That is the instrument, not the "
                 "quantiser.")
        .Section("hand", "The hand: B3, Position and Mallet",
                 "**B3 taps to strike and holds to mute.** A tap under "
                 "120 ms is purely a strike; keep your hand down and the "
                 "shell damps over the next fifth of a second, exactly as a "
                 "palm left on a real pan does. DAMP does the same from "
                 "CV.\n\n"
                 "B3's light breathes with the instrument — dim at rest, "
                 "bright while the pan is ringing — and turns amber while "
                 "the palm is down. Hold **B2** and it becomes the audio "
                 "load meter instead: green under half, amber to three "
                 "quarters, red past it.\n\n"
                 "**Position** is where the hand lands: hard left is the "
                 "centre of the dimple — round, fundamental-heavy, the "
                 "sound of a thumb — and hard right is out on the shoulder "
                 "of the tone field, thin and bright and ringing.\n\n"
                 "**Mallet** is what lands. It sets the width of the strike "
                 "impulse, from a 1.3 ms pad of the thumb to a 0.14 ms "
                 "fingertip, so it shapes the attack the way a real contact "
                 "does rather than EQ-ing the result afterwards.")
        .Section("shell", "Decay and Sympathy",
                 "**Decay** is how long the ding's fundamental rings, from "
                 "a quarter-second tok to sixteen seconds. Every other mode "
                 "scales off it by frequency — the shell modes are always "
                 "gone first, the fundamental always last.\n\n"
                 "**Sympathy** is the halo: how much of a strike reaches the "
                 "eight fields nobody touched, and how hard the air cavity "
                 "couples them. At zero this is a bank of isolated "
                 "resonators and sounds like one. Past about three quarters "
                 "every strike starts to play a chord, which is lovely and "
                 "is not a handpan.")
        .Section("temper", "Temper — the one ratio a tuner may not touch",
                 "Every tone field's first three modes are hammered to sit "
                 "at **f, 2f and 3f**. That ratio is not a preference, it "
                 "is what makes the instrument a handpan; a pan whose "
                 "octave is four cents out is a pan that goes back on the "
                 "bench.\n\n"
                 "Temper bends it anyway. The partials land at "
                 "*n^(1+temper)*, so **dead centre is a real handpan** and "
                 "the further you go the less the thing is one:\n\n"
                 "- **Hard left** compresses to about 1 : 1.5 : 1.9, which "
                 "is near a church bell's hum-and-tierce cluster — dark, "
                 "hollow, a struck minor third that is not in any scale.\n"
                 "- **Left of centre** is steel tongue drum: still tuned, "
                 "but thicker and less bright than a pan.\n"
                 "- **Right of centre** stretches like a struck plate, "
                 "the way a piano's top octave is stretched.\n"
                 "- **Hard right** is 1 : 2.6 : 4.7 — a gong. Pitch stops "
                 "meaning much and the scale becomes a set of textures.\n\n"
                 "The fundamental never moves, so the note stays exactly "
                 "where the scale put it however far the rest is bent. Move "
                 "it a hair off centre for an instrument that is *almost* "
                 "right, which is a very old trick.")
        .Section("voicing", "Voicing — the rest of the bench",
                 "**Shimmer** splits every mode into a beating pair, the "
                 "way an asymmetric dimple does. It is what makes the "
                 "instrument breathe instead of sit still.\n\n"
                 "**Metal** is the level of the six untuned shell modes — "
                 "how much audible steel sits above the tuned core. "
                 "**Tilt** is how fast the top end closes down: low is "
                 "bright and gong-like, high is a thick, dark, "
                 "fast-closing shell.\n\n"
                 "**Body** is the Helmholtz cavity — the air inside the "
                 "shell speaking through the gu port, the warm bloom "
                 "underneath the attack. **Contact** is the bare "
                 "skin-on-steel tak that bypasses the resonators "
                 "entirely.\n\n"
                 "Nothing here is post-processing. Every one of these "
                 "rebuilds the mode table, so what you are turning is how "
                 "the shell was made, not an EQ on the way out.")
        .Section("build", "Build layer, and EXC",
                 "Hold **B2** for the set-and-forget end: **Gu Tune** "
                 "moves the cavity relative to the ding, **Fine** detunes "
                 "the whole instrument +/-50 cents to sit with other "
                 "players, **Spread** is the stereo width of the tone-field "
                 "layout, and **Exciter In** and **Level** are the two "
                 "gains.\n\n"
                 "**Dynamics** is how much a strike's velocity is allowed "
                 "to change the sound beyond its level — the timbre shift "
                 "toward the shell modes, and how far a hard strike blooms "
                 "sharp before settling over about 150 ms. At zero every "
                 "strike is the same colour and only the level moves, which "
                 "is what a sampler does. Turn it up and the instrument "
                 "answers the hand.\n\n"
                 "Anything patched to **EXC** is poured into all nine tone "
                 "fields. Send it a drum machine, a noise burst, a spoken "
                 "word: it comes back as that signal played on a handpan. "
                 "With nothing patched the input is squelched entirely, "
                 "which lets the engine skip fields that have gone quiet. "
                 "(The manual has room for eight sections; Duet took EXC's.)");

static void DescribeManual()
{
    page_play.Help("The instrument. B1 cycles Voicing and Duet; hold B2 for Build.");
    page_voice.Help("How the shell is built, rather than how it is played.");
    page_duet.Help("How this pan answers a strike played on the other one.");
    page_build.Help("Held on B2 — tuning reference, stereo, and gains.");

    k_scale.Help("Which handpan tuning the nine fields are cut to.");
    k_root.Help("Ding fundamental, C2 to C4. The rim notes follow the scale.")
        .SeeAlso(j_note);
    k_pos.Help("Where the hand lands: dimple centre to the shoulder of the "
               "tone field.").SeeAlso(j_pos);
    k_mallet.Help("What lands: the width of the strike impulse, soft thumb "
                  "to hard fingertip.");
    k_decay.Help("How long the ding rings, to -60 dB.").SeeAlso(j_damp);
    k_symp.Help("How much of a strike the untouched fields answer with.")
        .SeeAlso(j_symp);

    k_temper.Help("Stretches the tuned triple off 1:2:3. Centre is a real "
                  "handpan; left goes toward bell metal, right toward gongs.");
    k_shimmer.Help("Beating between each mode and its split partner.");
    k_metal.Help("Level of the untuned shell modes above the tuned core.");
    k_tilt.Help("How fast the high modes die relative to the fundamental.");
    k_cavlvl.Help("The air cavity in the mix — the bloom under the attack.");
    k_contact.Help("Bare skin-on-steel tak, bypassing the resonators.");

    k_answer.Help("Off, Canon (moved by Interval), Mirror (low answers "
                  "high) or Scatter (any field).");
    k_delay.Help("Time to the first answer, and between repeats.");
    k_interval.Help("Tone fields to move the answer by, -4 to +4.");
    k_repeats.Help("How many answers per strike heard. Canon climbs "
                   "another interval with each.");
    k_chance.Help("How often a heard strike is answered at all.");
    k_ansvel.Help("Answer velocity as a share of the velocity heard.");

    k_cavtune.Help("Helmholtz frequency as a ratio of the ding.");
    k_fine.Help("Detune the whole instrument, to sit with other players.");
    k_spread.Help("Stereo width of the tone-field layout around the shell.");
    k_dyn.Help("How much velocity changes timbre and pitch, not just level. "
               "Also sets how far a hard strike blooms sharp before it "
               "settles.").SeeAlso(j_strike);
    k_ext.Help("Gain of the external exciter input.").SeeAlso(j_exc);
    k_level.Help("Output level.");

    j_strike.Help("Strike trigger. Its peak height sets the velocity, so a "
                  "velocity-scaled trigger plays dynamics.").SeeAlso(k_dyn);
    j_exc.Help("Any audio, poured into all nine tone fields.").SeeAlso(k_ext);
    j_note.Help("1 V/oct, quantised to the nine tone fields. 0 V is the "
                "ding; past the top note it folds back down an octave.")
        .SeeAlso(k_root).SeeAlso(k_scale);
    j_accent.Help("Adds or subtracts strike velocity, +/-5 V.");
    j_pos.Help("Strike position, summed with the Position knob.")
        .SeeAlso(k_pos);
    j_damp.Help("Palm mute, 0-5 V. Does what holding B3 does.");
    j_ring.Help("Output: 0-5 V envelope of the whole instrument's ringing "
                "energy.").SeeAlso(k_symp);
    j_symp.Help("Sympathy, summed with the Sympathy knob.").SeeAlso(k_symp);
    j_out_l.Help("Left output.");
    j_out_r.Help("Right output.");

    host.Jacks(j_strike, j_exc, j_note, j_accent, j_pos, j_damp, j_ring,
               j_symp, j_out_l, j_out_r)
        .Attach(manual);
}

/* ── Glue ────────────────────────────────────────────────────────────── */

int main()
{
    hw.Init();
    hp::Init(hw.SampleRate());

    const float sr = hw.SampleRate();
    /* The block size the hardware actually negotiated, not the constant it
     * was asked for — Seed may round it, and a wrong figure here scales the
     * whole load reading. */
    cpu_.Init(sr, static_cast<int>(hw.BlockSize()));

    peak_win_ = static_cast<int32_t>(kPeakWindowMs * 0.001f * sr);
    refr_len_ = static_cast<int32_t>(kRefractoryMs * 0.001f * sr);
    if (peak_win_ < 1) peak_win_ = 1;

    /* B1 cycles the three performance pages; Build is a held layer on B2 so
     * it can never be left showing by accident. Declared here rather than
     * at static-init time so it cannot depend on the order two globals in
     * this file happen to be constructed in. */
    pager.Cycle(hw.buttons[kButtonB1], kPagePlay, kPageVoicing, kPageDuet)
         .Shift(hw.buttons[kButtonB2], kPageBuild);

    /* J7 stops being an input and starts being an output. Every one of
     * J3..J8 can do this — the DG411 points either the ADC or the DAC at
     * the panel — so spending one of six CV inputs on an envelope out is
     * a choice, not a hardware limitation. */
    hw.cv[kCvRingOut].EnableCvOutput();

    k_root.Overdraw(DrawSelectedField);
    k_symp.Overdraw(DrawRingEnergy);

    DescribeManual();

    /* Preset payload — and what the descriptor advertises to the host. */
    presets.Manage(pager);
    presets.Init();
    presets.BootLoad();

    static panel_i2c::Config disp_cfg;
    disp_cfg.hw          = &hw;
    disp_cfg.pager       = &pager;
    disp_cfg.pages       = kPages;
    disp_cfg.num_pages   = kNumPages;
    disp_cfg.module_name = "Duet";
    disp_cfg.jacks       = kJackOrder;
    disp_cfg.num_jacks   = (uint8_t)(sizeof(kJackOrder) / sizeof(kJackOrder[0]));
    disp_cfg.btn_labels  = kBtnLabels;   /* handpan polls hw.buttons[] */
    disp_cfg.address     = panel_i2c::kDefaultAddress;   /* make PANEL_I2C_ADDR= */
    panel_i2c::Init(disp_cfg);

    /* Two labs booted together must not roll the same dice. */
    rng_ ^= (uint32_t)disp_cfg.address * 0x9E3779B9u ^ daisy::System::GetNow();
    if (rng_ == 0u) rng_ = 1u;

    OnFrame();
    hw.StartAudio(Audio);

    loop.Use(pager)
        .Use(host)
        .Use(page_play)
        .Use(page_voice)
        .Use(page_duet)
        .Use(page_build)
        .OnPoll(OnPoll)
        .OnFrame(OnFrame)
        .OnRender(OnRender)
        .OnPageChange(panel_i2c::PageChanged);

    for (;;) loop.Tick();
}
