/**
 * handpan_dsp.h — A physically-modelled handpan (hang drum).
 *
 * Nine tone fields: a centre "ding" plus eight rim notes, laid out on the
 * shell the way a real pan is, alternating left and right as the scale
 * climbs. Every field is a bank of modal resonators that is *always*
 * running — striking one field injects energy into it and, through the
 * shell and the air cavity, into all the others. There is no voice
 * allocation and no voice stealing: the instrument rings as one body,
 * which is exactly what makes a handpan sound like a handpan rather than
 * like a tuned bell.
 *
 * What the model reproduces, in rough order of how much each one matters
 * to whether it sounds real:
 *
 *   1. 1:2:3 mode tuning.  A handpan tuner hammers each tone field until
 *      its first three modes land on f, 2f and 3f. That is the defining
 *      acoustic signature of the instrument; nothing else sounds like it.
 *   2. Sympathetic halo.  A strike excites every other field through the
 *      shell, and each one rings at its own tuned frequencies. Without
 *      this the model sounds like a steel drum sample.
 *   3. Contact transient.  The "tak" of skin on steel — a short filtered
 *      noise burst — sits on top of the ring. Pure modal synthesis with a
 *      clean impulse always sounds synthetic; this is why.
 *   4. Split degenerate modes.  The dimple's asymmetry splits what would
 *      be one mode into a close pair, and the pair beats. That slow
 *      breathing shimmer is a large part of "alive".
 *   5. Frequency-dependent decay.  The fundamental rings for many seconds;
 *      the inharmonic shell modes are gone in under one.
 *   6. Helmholtz cavity.  The shell and its gu port form an air resonator
 *      near the ding's fundamental. It supplies the warm bloom under the
 *      attack, and it couples every field to every other field.
 *   7. Tension nonlinearity.  Struck hard, a field starts sharp and
 *      settles over ~150 ms.
 *
 * The DSP knows nothing about VirtualKnob, ControlLoop, or any other SDK
 * surface — and nothing about libDaisy either. That is deliberate: this
 * translation unit also compiles natively, so the model can be rendered to
 * a .wav and judged by ear without a flash cycle. handpan.cpp owns the
 * board and the audio callback; this file owns the sound.
 *
 * Threading: every Set*() is a control-thread call that writes plain words
 * and (at most) raises a dirty flag. All model state is touched only by the
 * audio thread.
 *
 * Strike() posts into a lock-free ring that Process() drains at the top of a
 * block. That ring is **single-producer**: it has one write cursor and no
 * claim step, so two threads calling Strike() can be handed the same slot
 * and lose a note. Pick one thread and strike only from there — handpan.cpp
 * strikes from the audio callback, and turns the button's strike into a
 * request counter that the same callback consumes.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace handpan_dsp {

/* ── Shape of the instrument ─────────────────────────────────────────── */

/** Tone fields: index 0 is the ding, 1..8 are the rim notes going up. */
constexpr uint8_t kNumFields = 9;

/** Modes per field: three tuned harmonics as beating pairs (indices 0..5)
 *  plus six untuned shell modes (6..11). See kInharmonic[] in the .cpp. */
constexpr uint8_t kModesPerField = 12;

constexpr uint16_t kTotalModes = kNumFields * kModesPerField; /* 108 */

/** Built-in handpan tunings. Each is nine semitone offsets from the root,
 *  the first being the ding. Names follow the handpan-maker convention;
 *  the interval content is spelled out in kScales[] so it is checkable. */
constexpr uint8_t kNumScales = 8;

/** Human-readable scale name, for the LED rings and the manual. */
const char* ScaleName(uint8_t scale);

/* ── Lifecycle ───────────────────────────────────────────────────────── */

/** Cache the sample rate, build the mode table, silence everything. */
void Init(float sample_rate);

/* ── Tuning ──────────────────────────────────────────────────────────── */

/** Select a tuning, 0..kNumScales-1. Retunes on the next control tick. */
void SetScale(uint8_t scale);

/** Fundamental of the ding, in Hz. The rim notes follow from the scale. */
void SetRootHz(float hz);

/* ── Performance controls ────────────────────────────────────────────── */

/** Where the hand lands. 0 = the dimple's centre (round, fundamental-heavy),
 *  1 = out on the shoulder of the field (thin, bright, ringy). */
void SetPosition(float p);

/** What lands. 0 = soft pad of the thumb (a long, dull force pulse),
 *  1 = hard fingertip or nail (a short pulse, and much more contact
 *  noise). This is modelled where it physically belongs — in the width of
 *  the strike impulse — so it shapes the attack spectrum rather than
 *  EQ-ing the result. */
void SetMallet(float m);

/** Ring length: T60 of the ding's fundamental, in seconds. Every other
 *  mode scales off this by frequency (see SetTilt). */
void SetDecay(float seconds);

/** Halo: how much of a strike reaches the fields that were *not* struck,
 *  and how strongly the air cavity couples them together. 0 is a bank of
 *  isolated resonators; 1 is a whole instrument breathing. */
void SetSympathy(float s);

/** Palm on the shell — the player's mute. 0 is open, 1 chokes the ring
 *  down to a short thud. Continuous, so it works as a swell too. */
void SetHandDamp(float d);

/* ── Voicing ("the tuner's page") ────────────────────────────────────── */

/** Depth of the split between each mode and its beating partner. 0 is a
 *  dead-still perfectly symmetric field; the default beats about every two
 *  seconds; 1 beats three or four times a second, and deeper. */
void SetShimmer(float s);

/**
 * Temper — the one thing a handpan maker is not allowed to change.
 *
 * The tuned triple sits at n^(1 + temper), so **zero is a real handpan**:
 * 1, 2, 3 exactly, the ratio a tuner spends hours hammering toward. Away
 * from zero the partials stretch or compress and the instrument stops
 * being a handpan and becomes another piece of tuned metal — compressed
 * lands near a bell's hum-and-tierce cluster (1 : 1.5 : 1.9), stretched
 * goes out toward gongs and plates (1 : 2.6 : 4.7).
 *
 * The fundamental is never moved, so the note stays where the scale put
 * it however far the rest is bent. Range +/-0.4.
 */
void SetTemper(float t);

/** Level of the six untuned shell modes — how much audible steel sits on
 *  top of the tuned 1:2:3 core. A dB taper: 0 is off, the default leaves
 *  them 20-30 dB under the note, and the top brings them up by 24 dB and
 *  lets them ring nearly three times as long. */
void SetMetal(float m);

/** Brightness of the ring: tilts both the level and the decay of the
 *  octave and twelfth against the fundamental. At 0 the octave is as loud
 *  as the fundamental and outlasts it (bright, glassy); high values give
 *  the dark, fast-closing top end of a thick, well-annealed shell, where
 *  only the fundamental is left after a fraction of a second. */
void SetTilt(float t);

/** Helmholtz frequency as a ratio of the ding's fundamental (~0.5..2). */
void SetCavityTune(float ratio);

/** How much of the air cavity is in the mix. */
void SetCavityLevel(float l);

/** Tension nonlinearity: how sharp a full-velocity strike starts, in
 *  cents, before settling over ~150 ms. Real pans do 10-35 cents. */
void SetBloom(float cents);

/* ── Set-and-forget (module settings, not knobs) ─────────────────────── */

/** Stereo width of the tone-field layout, 0 = mono, 1 = fully spread. */
void SetSpread(float s);

/** How much strike velocity is allowed to change timbre as well as level.
 *  0 makes every strike identical in colour; 1 is a real instrument. */
void SetDynamics(float d);

/** Level of the bare skin-on-steel "tak" that bypasses the resonators. A
 *  dB taper: masked under the note up to the default, an audible click
 *  near the top. */
void SetContact(float c);

/** Output level, 0..1 (unity at 1). */
void SetLevel(float l);

/** Gain applied to the external exciter input before it reaches the
 *  fields, 0..1. */
void SetExtLevel(float l);

/* ── Playing ─────────────────────────────────────────────────────────── */

/** Strike tone field @p field (0..kNumFields-1) at @p velocity (0..1).
 *  Safe to call from a poll hook; the audio thread picks it up within a
 *  block. Extra strikes on an already-ringing field add energy to it, as
 *  they do on the real thing — nothing is cut off. */
void Strike(uint8_t field, float velocity);

/** Which tone field a 1 V/oct input is asking for. Volts are measured
 *  from the ding, and the result is the nearest field in the current
 *  tuning, so a sequencer's pitch CV plays the pan like a keyboard. */
uint8_t FieldForVolts(float volts);

/** Fundamental of a tone field in Hz, for display and for the manual. */
float FieldHz(uint8_t field);

/* ── Metering ────────────────────────────────────────────────────────── */

/** How hard field @p field is ringing right now, 0..1, for its LED ring. */
float FieldEnergy(uint8_t field);

/** Total ringing energy of the instrument, 0..1, for the CV output. */
float RingEnergy();

/* ── Audio ───────────────────────────────────────────────────────────── */

/**
 * Render @p n frames. @p ext may be null; when it is not, that signal is
 * fed into every tone field as an external exciter, which turns the module
 * into a resonator you can hit with any audio at all.
 */
void Process(const float* ext, float* out_l, float* out_r, size_t n);

} // namespace handpan_dsp
