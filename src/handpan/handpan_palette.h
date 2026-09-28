/**
 * handpan_palette.h — Ring colours.
 *
 * The instrument is a steel shell, so the palette is metal: warm bronze
 * for the things that decide the note, cool steel for the things that
 * decide the material, and a bright anneal-blue for the halo.
 *
 *   Play      [Scale bronze ] [Root  amber ]
 *             [Position gold] [Mallet copper]
 *             [Decay ember  ] [Sympathy halo]
 *
 * Every passive colour is a heavily dimmed trace of its own hue rather
 * than black, so an unfilled ring still reads as a ring in a dark rack.
 */

#pragma once

#include "alchemy/led/panel.h"

using Rgb = alchemy::LedPanel::Rgb;

/* ── Play ─────────────────────────────────────────────────────────────── */
constexpr Rgb kScaleOn      = {0xFF, 0xA8, 0x30};
constexpr Rgb kScaleOff     = {0x18, 0x0A, 0x02};
constexpr Rgb kRootColor    = {0xFF, 0xC8, 0x50};
constexpr Rgb kRootPassive  = {0x16, 0x0E, 0x03};
constexpr Rgb kPosColor     = {0xFF, 0xD8, 0x80};
constexpr Rgb kPosPassive   = {0x16, 0x10, 0x06};
constexpr Rgb kMalletColor  = {0xFF, 0x80, 0x40};
constexpr Rgb kMalletPassive= {0x16, 0x08, 0x03};
constexpr Rgb kDecayColor   = {0xFF, 0x60, 0x20};
constexpr Rgb kDecayPassive = {0x14, 0x05, 0x01};
constexpr Rgb kSympColor    = {0x50, 0xD0, 0xFF};
constexpr Rgb kSympPassive  = {0x02, 0x0C, 0x16};

/* ── Voicing ──────────────────────────────────────────────────────────── */
constexpr Rgb kShimmerColor = {0x90, 0xE0, 0xFF};
constexpr Rgb kShimmerPass  = {0x06, 0x0E, 0x16};
constexpr Rgb kMetalColor   = {0xC0, 0xD0, 0xE0};
constexpr Rgb kMetalPass    = {0x0C, 0x0E, 0x12};
constexpr Rgb kTiltColor    = {0x80, 0xA0, 0xC0};
constexpr Rgb kTiltPass     = {0x08, 0x0A, 0x10};
constexpr Rgb kCavColor     = {0xB0, 0x70, 0xFF};
constexpr Rgb kCavPass      = {0x0C, 0x04, 0x16};
/* Temper is bipolar and its centre is not a default but a *fact* — dead
 * centre is the 1:2:3 ratio that makes the thing a handpan at all — so the
 * centre colour is a bright marker you can find by feel, not a dim pivot.
 * Compressed goes warm toward bell metal, stretched cool toward gongs. */
constexpr Rgb kTemperUp     = {0x70, 0xC0, 0xFF};
constexpr Rgb kTemperDown   = {0xFF, 0x90, 0x30};
constexpr Rgb kTemperCenter = {0xE0, 0xFF, 0xE0};
constexpr Rgb kContactColor = {0xE0, 0xE0, 0xB0};
constexpr Rgb kContactPass  = {0x10, 0x10, 0x08};

/* ── Build (the held layer) ───────────────────────────────────────────── */
constexpr Rgb kBuildColor   = {0x60, 0xFF, 0xC0};
constexpr Rgb kBuildPass    = {0x03, 0x14, 0x0E};
constexpr Rgb kFineUp       = {0x60, 0xFF, 0xC0};
constexpr Rgb kFineDown     = {0xFF, 0x90, 0x60};
constexpr Rgb kFineCenter   = {0x18, 0x18, 0x18};

/** Overlay painted on every ring as its tone field rings — the instrument
 *  showing you where the energy is. Deliberately close to white so it
 *  reads on top of whatever hue the knob's own arc is wearing. */
constexpr Rgb kRingGlow     = {0xFF, 0xF0, 0xD0};
