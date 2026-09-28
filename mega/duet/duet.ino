// Duet - two Alchemy Labs answering each other, through this Mega
//
// The Mega side of the alchemy firmware `duet` (src/duet/duet.cpp). It is
// ../panel_i2c - the same panel drawing, the same polling, the same
// arbitration (the panel shows whichever lab you touched last) - plus:
//
//   THE RELAY. Every module is an I2C slave, so the labs cannot hear each
//   other. A lab reports each strike it plays as an EVENT frame
//   [PLAY, field, vel]; this sketch writes [HEAR, field, vel] straight to
//   every other live lab, which answers on its own schedule (its Duet page)
//   and reports each answer as [ANSWER, field, vel]. ANSWER is drawn but
//   never relayed, so two labs both set to answer cannot feed back.
//
//   STRIKE LIGHTS. A column of nine lights in each side margin - lab 0x42
//   (lab #1) on the right, 0x43 (lab #2) on the left, the way they sit in
//   the rack either side of the screen - one per tone field, the ding at the
//   bottom and the rim notes climbing to the top. Amber when that lab plays
//   a field, magenta when it answers, fading over about half a second.
//
//   FAST SWITCH. Both labs run the same firmware, so the title, jacks and
//   panel furniture are the same for either. Switching labs repaints only
//   the text cells that differ, the knobs whose positions differ (all six
//   only if the page colour changed), and a triangle beside the title that
//   points at the lab on screen. A lab with a different name or jacks still
//   gets panel_i2c's full repaint.
//
//   I2C IS POLLED BETWEEN DRAW STEPS (PumpAll), not only between frames, so
//   a strike is relayed within a few ms even while the panel is repainting.
//
// Bench test with no hands on the modules, on USB serial: "a3" makes lab
// 0x42 hear field 3, "b7" makes lab 0x43 hear field 7. Serial also carries
// one line per relayed strike and a summary line a second.
//
// This was first written as its own two-pan landscape layout. On this shield
// that version drew every fill and circle but no text at all, in any size or
// rotation, for reasons not found (2026-09-28) - so the drawing here is
// panel_i2c's, unchanged, because that is known to render.
//
// LINK
//   Mega SCL (pin 21, or the SCL pin by AREF)  ->  module header B3 (pin 8)
//   Mega SDA (pin 20, or the SDA pin by AREF)  ->  module header B5 (pin 7)
//   Mega GND                                   ->  module header pin 5
//   On the module those are PB6/PB7 = I2C4, NOT the board's own I2C1 on
//   PB8/PB9. No level shifter: the bus idles at 5 V on the Mega's pull-ups
//   (Wire.begin() enables the AVR's internal ones; Mega R3 layouts add 10k),
//   which the AVR needs - its TWI high threshold is 0.7 x 5 V = 3.5 V, above
//   what a 3.3 V pull-up could give - and PB6/PB7 are 5 V tolerant.
//
//     source 0 : Serial  (USB - canned test frames from a host)
//     source 1 : I2C 0x42   (panel_i2c::kDefaultAddress)
//     source 2 : I2C 0x43
//     source 3 : I2C 0x44
//   The address identifies the module.  An absent one NACKs its address in
//   ~100 us and is re-probed every kAbsentPollMs.
//
// TRANSPORT (panel_i2c.cpp has the other half)
//   READ 32 bytes:  [n] [n stream bytes] [pad]   n <= 31, the stream below.
//     A frame may straddle two reads; the byte parser does not care.
//   WRITE 1 byte:   0x01 ANNOUNCE - "send me everything".  Sent whenever a
//     module answers but has not yet delivered a COMMIT, so a Mega that
//     boots after the module still gets its labels.
//   Because the master pulls, nothing is ever lost while the TFT is busy:
//   bytes wait in the module's ring until the next poll.  The module holds
//   SCL (clock stretching) while its audio interrupt runs, so a read can take
//   a few ms instead of ~3 - hence the wire timeout below, not a short one.
//
// FRAMING
//   AA 55 <type> <len> <payload...> <crc8>       crc8 over type, len, payload
//     0x11 TEXT   slot, text[12]                            17 B
//     0x13 COMMIT kind, page, n_pages, r, g, b              10 B
//     0x12 STATE  page, btn_flags, activity, val[6]         13 B
//   TEXT slots: 0-5 knobs, 6-8 buttons, 9 page name, 10 module name,
//   11-20 jacks.  COMMIT kind 0 = labels changed (a page change), 1 = module
//   or firmware changed (full repaint).
//
//   Frames stay under 32 bytes, as on the UART link.  Over I2C that is no
//   longer about RX overruns, but the module side and ../panel_link share
//   the format and the USB test path still needs it.
//
// HARDWARE  Elegoo Mega 2560 + HVGA 480x320 3.5" shield (ILI9488).
//   See handpan_panel.ino for the two MCUFRIEND_kbv library edits this needs.
//   The shield occupies pins 22-53 only, so SDA/SCL (20/21) are free.

#include <Adafruit_GFX.h>
#include <MCUFRIEND_kbv.h>
#include <Wire.h>
#include <string.h>

MCUFRIEND_kbv tft;

/* ── Link ────────────────────────────────────────────────────────────── */

#define NUM_SOURCES  4
#define MAX_PAYLOAD  32

#define MSG_TEXT   0x11
#define MSG_STATE  0x12
#define MSG_COMMIT 0x13
#define MSG_EVENT  0x14   /* duet: kind, field, vel                           */

#define EVT_PLAY   0x01
#define EVT_ANSWER 0x02

const uint32_t kBaud = 115200ul;

/* ── I2C ─────────────────────────────────────────────────────────────── */

/* Source i (1..3) is the module at kI2cAddr[i].  Slot 0 is USB serial. */
const uint8_t  kI2cAddr[NUM_SOURCES] = {0, 0x42, 0x43, 0x44};

const uint8_t  CMD_ANNOUNCE   = 0x01;
const uint8_t  CMD_HEAR       = 0x20;   /* duet: the other lab played f, v  */
const uint8_t  kReadLen       = 32;     /* = Wire's BUFFER_LENGTH on AVR   */
const uint32_t kI2cClock      = 100000ul; /* standard mode: long wires, 10k */
const uint32_t kPollMs        = 10ul;   /* present module, ring drained    */
const uint32_t kAbsentPollMs  = 250ul;  /* nobody answered last time       */
const uint32_t kAnnounceAskMs = 500ul;  /* re-ask while not ready          */
/* Generous: the module stretches SCL while its audio callback runs. */
const uint32_t kWireTimeoutUs = 25000ul;

struct I2cSource {
  bool     present;
  uint32_t next_poll;
  uint32_t next_ask;
  uint16_t reads, nacks, bytes, asks;   /* DEBUG_I2C stats, per 2 s */
};
static I2cSource i2c[NUM_SOURCES];

/* Stay on a module this long after its last activity before allowing a
 * switch.  Not politeness: a label repaint is milliseconds and a full one is
 * hundreds, so unrestrained alternation would sit mid-redraw forever. */
const uint32_t kHoldoffMs = 1500ul;

/* Set to 0 once the link is trusted; costs a print per frame. */
#define DEBUG_ACK 0

/* Bus diagnostics on USB serial: idle line levels and an address scan at
 * boot, a line whenever a module appears or vanishes, and a stats line every
 * 2 s. Separates "nobody answers" (wiring, pins, pull-ups) from "answers but
 * sends nothing" (firmware). Set to 0 once the link is trusted. */
#define DEBUG_I2C 0

/* ── Text slots ──────────────────────────────────────────────────────── */

#define LBL        12
#define NUM_KNOBS   6
#define NUM_BTNS    3
#define NUM_JACKS  10
#define NUM_SLOTS  21

#define SLOT_KNOB(i) (i)
#define SLOT_BTN(i)  (6 + (i))
#define SLOT_PAGE    9
#define SLOT_NAME    10
#define SLOT_JACK(i) (11 + (i))

struct Module {
  bool     seen;
  bool     ready;                       /* a COMMIT has been seen */
  bool     dirty;                       /* a TEXT changed since the last COMMIT */
  char     text[NUM_SLOTS][LBL + 1];
  uint8_t  page, n_pages, rgb[3];
  uint8_t  val[NUM_KNOBS];
  uint8_t  btn_flags;
  uint32_t last_activity;
  uint32_t last_frame;      /* any valid frame, activity or not */
};
static Module mod[NUM_SOURCES];

/* ── Duet: strike lights and the relay ───────────────────────────────── */

#define NUM_FIELDS 9

/* One tone field's light. `level` counts down to 0; kind and velocity are
 * taken at the hit, so an answer landing on a played field takes it over. */
struct Glow { uint8_t level, kind, vel, drawn; };
static Glow glow[NUM_SOURCES][NUM_FIELDS];
const uint8_t  kGlowSteps  = 6;
const uint32_t kGlowStepMs = 90ul;             /* ~0.55 s to dark */
static bool    strikes_force = true;           /* repaint every light */
static int8_t  strikes_active = -2;            /* lab whose label is lit */

struct Counts { uint32_t played, heard, answered; };
static Counts counts[NUM_SOURCES];

/* Relays found while parsing a read go out once the read is done. */
struct Relay { uint8_t from, to, field, vel; };
static Relay   relay_q[8];
static uint8_t relay_n;

struct Parser {
  uint8_t state, type, len, idx, crc;
  uint8_t buf[MAX_PAYLOAD];
};
static Parser parser[NUM_SOURCES];

static int8_t   active     = -1;
static int8_t   drawn_mod  = -1;
static uint8_t  drawn_page = 0xFF;
static uint8_t  drawn_val[NUM_KNOBS];
static uint8_t  drawn_flags;
static bool     selftest   = true;
static bool     need_full  = false;
static bool     need_lbls  = false;
static bool     drawn_stale = false;

/* Silence this long on the active module means the wire, not the firmware. */
const uint32_t kLinkTimeoutMs = 3000ul;

/* ── Palette ─────────────────────────────────────────────────────────── */

#define RGB(r, g, b) ((uint16_t)(((r) & 0xF8) << 8 | ((g) & 0xFC) << 3 | (b) >> 3))

const uint16_t C_BG      = RGB(  0,   0,   0);
const uint16_t C_PANEL   = RGB( 12,  12,  16);
const uint16_t C_TEXT    = RGB(255, 255, 255);
const uint16_t C_DIM     = RGB(140, 138, 132);
const uint16_t C_KNOB    = RGB( 52,  52,  60);
const uint16_t C_KNOBRIM = RGB(215, 215, 225);
const uint16_t C_POINTER = RGB(255, 255, 255);
const uint16_t C_LED_OFF = RGB( 60,  58,  52);
const uint16_t C_IND_OFF = RGB( 34,  34,  40);
const uint16_t C_JACK    = RGB(215, 215, 225);
const uint16_t C_JACKIN  = RGB(  0,   0,   0);
const uint16_t C_IDLE    = RGB( 70,  70,  78);

static uint16_t C_PAGE = RGB(255, 168, 48);   /* from the active COMMIT */

const uint8_t GLOW_IDLE[3]   = { 40,  40,  48};
const uint8_t GLOW_PLAY[3]   = {255, 168,  48};
const uint8_t GLOW_ANSWER[3] = {255,  70, 200};

/* ── Panel geometry, mm (the "handpan" set of spagyros_paper.json) ───── */

const float PANEL_W = 61.0, PANEL_H = 128.5;
const float TITLE_FROM_TOP = 4.5;
const float RULE_TOP = 7.5, RULE_BOT = 7.0, BRAND_FROM_BOT = 3.0;
const float ROW_FROM_TOP[3] = {21.5, 49.0, 77.5};
const float SIDE_X_FROM_EDGE = 13.0;
const float KNOB_D = 21.6, RING_OD = 20.6, CAP_D = 15.0;
const float CENTRE_BTN_D = 8.5, IND_OFFSET = 6.5, IND_W = 4.5, IND_H = 2.5;
const float USB_W = 11.0, USB_H = 4.25, USB_BOTTOM = 35.25;
const uint8_t JACK_COLS = 5;
const float JACK_X_PITCH = 11.5, JACK_Y_PITCH = 13.0, JACK_X_SHIFT = 0.4;
const float JACK_D = 7.0, JACK_Y_LOW = 15.75;
const float LBL_JACK_GAP = 1.2, LBL_SIDE_GAP = 1.2, LBL_CENTRE_GAP = 1.2;
const float CENTRE_CUT_H = 16.5;

static float gScale;
static int   gOx, gOy;

static inline int MM (float v)  { return (int)(v * gScale + 0.5f); }
static inline int PX (float mm) { return gOx + (int)(mm * gScale + 0.5f); }
static inline int PYT(float mm) { return gOy + (int)(mm * gScale + 0.5f); }
static inline int PYB(float mm) { return gOy + (int)((PANEL_H - mm) * gScale + 0.5f); }

static void PumpAll(void);   /* fwd - the draw path calls it between parts */

/* ── Text ────────────────────────────────────────────────────────────── */

static void textAt(int cx, int cy, const char *s, uint8_t size, uint16_t color) {
  int16_t x1, y1; uint16_t w, h;
  tft.setTextSize(size);
  tft.setTextColor(color);
  tft.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  tft.setCursor(cx - w / 2 - x1, cy - h / 2 - y1);
  tft.print(s);
}

/* Erase a fixed cell, then centre new text in it.  Cells are what make a page
 * change cheap: only these repaint, never the panel furniture. */
static void textCell(int cx, int cy, int halfw, int halfh,
                     const char *s, uint8_t size, uint16_t color) {
  tft.fillRect(cx - halfw, cy - halfh, halfw * 2, halfh * 2, C_PANEL);
  if (s && s[0]) textAt(cx, cy, s, size, color);
}

static void fillRing(int cx, int cy, int rOuter, int rInner, uint16_t color) {
  for (int r = rInner; r <= rOuter; r++) tft.drawCircle(cx, cy, r, color);
}

/* ── Geometry helpers ────────────────────────────────────────────────── */

static float knobXmm(uint8_t i) { return (i & 1) ? PANEL_W - SIDE_X_FROM_EDGE
                                                 : SIDE_X_FROM_EDGE; }
static float knobYmm(uint8_t i) { return ROW_FROM_TOP[i >> 1]; }
static float jackXmm(uint8_t c) { return PANEL_W / 2 + JACK_X_SHIFT
                                       + (c - (JACK_COLS - 1) / 2.0f) * JACK_X_PITCH; }

/* ── Parts ───────────────────────────────────────────────────────────── */

static void drawKnobFace(uint8_t i, uint8_t v) {
  const int cx = PX(knobXmm(i)), cy = PYT(knobYmm(i));
  const int rRing = MM(RING_OD / 2), rCap = MM(CAP_D / 2);
  const float value = v / 255.0f;

  for (uint8_t s = 0; s < 17; s++) {
    float t = (float)s / 16.0f;
    float a = (-150.0f + 300.0f * t) * DEG_TO_RAD;
    tft.fillCircle(cx + (int)(sin(a) * rRing), cy - (int)(cos(a) * rRing), 2,
                   t <= value + 0.001f ? C_PAGE : C_LED_OFF);
  }
  tft.fillCircle(cx, cy, rCap, C_KNOB);
  tft.drawCircle(cx, cy, rCap, C_KNOBRIM);
  tft.drawCircle(cx, cy, rCap - 1, C_KNOBRIM);

  float a = (-150.0f + 300.0f * value) * DEG_TO_RAD;
  int px = cx + (int)(sin(a) * (rCap - 2)), py = cy - (int)(cos(a) * (rCap - 2));
  tft.drawLine(cx, cy, px, py, C_POINTER);
  tft.drawLine(cx + 1, cy, px + 1, py, C_POINTER);
}

static void drawKnobLabel(uint8_t i, const char *s) {
  textCell(PX(knobXmm(i)),
           PYT(knobYmm(i)) + MM(KNOB_D / 2 + LBL_SIDE_GAP + 1.4f),
           40, 4, s, 1, C_TEXT);
}

static void drawButtonFace(uint8_t i, bool lit) {
  const int cx = PX(PANEL_W / 2), cy = PYT(ROW_FROM_TOP[i]);
  const int r = MM(CENTRE_BTN_D / 2);
  const int iw = MM(IND_W), ih = MM(IND_H), io = MM(IND_OFFSET);
  tft.fillCircle(cx, cy, r, C_KNOB);
  tft.drawCircle(cx, cy, r, C_KNOBRIM);
  tft.fillRect(cx - iw / 2, cy - io - ih / 2, iw, ih, lit ? C_PAGE : C_IND_OFF);
  tft.fillRect(cx - iw / 2, cy + io - ih / 2, iw, ih, C_IND_OFF);
}

static void drawButtonLabel(uint8_t i, const char *s) {
  textCell(PX(PANEL_W / 2),
           PYT(ROW_FROM_TOP[i]) - MM(CENTRE_CUT_H / 2 + LBL_CENTRE_GAP + 1.4f),
           28, 4, s, 1, C_TEXT);
}

static void drawJack(float xmm, float ymm_from_bottom) {
  const int cx = PX(xmm), cy = PYB(ymm_from_bottom), r = MM(JACK_D / 2);
  fillRing(cx, cy, r, r - 2, C_JACK);
  tft.fillCircle(cx, cy, r - 3, C_JACKIN);
  tft.drawCircle(cx, cy, r - 3, C_KNOBRIM);
}

static void drawModuleDots(void) {
  const int right = gOx + MM(PANEL_W), top = gOy + 6;
  for (uint8_t i = 0; i < NUM_SOURCES; i++) {
    int x = right - 9 - i * 8;
    uint16_t c = (i == (uint8_t)active) ? C_PAGE : (mod[i].seen ? C_IDLE : C_PANEL);
    tft.fillRect(x, top, 5, 5, c);
    if (!mod[i].seen && i != (uint8_t)active) tft.drawRect(x, top, 5, 5, C_IDLE);
  }
}

/* ── Duet strike lights, in the side margins ─────────────────────────── */

/* Column for lab `src`: 0x42 (lab #1) right of the panel, 0x43 (lab #2)
 * left of it - where they sit in the rack. */
static int strikeX(uint8_t src) {
  return src == 1 ? tft.width() - gOx / 2 : gOx / 2;
}
static int strikeY(uint8_t f) {                /* ding at the bottom */
  const int top = gOy + 44, bot = gOy + MM(PANEL_H) - 30;
  return bot - (int)f * (bot - top) / (NUM_FIELDS - 1);
}

static void drawStrike(uint8_t src, uint8_t f) {
  Glow &g = glow[src][f];
  const uint8_t *hot = (g.kind == EVT_ANSWER) ? GLOW_ANSWER : GLOW_PLAY;
  /* A soft strike still has to be seen: velocity only takes the top 60 %. */
  const uint16_t amt = (uint16_t)g.level * (102u + (uint16_t)g.vel * 153u / 255u) / kGlowSteps;
  uint8_t c[3];
  for (uint8_t k = 0; k < 3; k++)
    c[k] = (uint8_t)(GLOW_IDLE[k] + ((int16_t)hot[k] - GLOW_IDLE[k]) * (int16_t)amt / 255);
  const int r = f ? 8 : 11;
  tft.fillCircle(strikeX(src), strikeY(f), r, RGB(c[0], c[1], c[2]));
  tft.drawCircle(strikeX(src), strikeY(f), r, g.level ? RGB(hot[0], hot[1], hot[2]) : C_IDLE);
  g.drawn = g.level;
}

static void drawStrikeLabel(uint8_t src) {
  const int x = strikeX(src), y = gOy + 18;
  tft.fillRect(x - 22, y - 5, 44, 11, C_BG);
  textAt(x, y, src == 1 ? "LAB 1" : "LAB 2", 1,
         src == (uint8_t)active ? C_PAGE : (mod[src].seen ? C_DIM : C_IDLE));
}

/* A triangle either side of the title; the one on the lab's side is lit,
 * pointing out at it. 0x42 (lab #1) is right of the screen, 0x43 left. */
static void drawPointer(void) {
  const int y = PYT(TITLE_FROM_TOP), dx = 46, h = 7;
  const int xl = PX(PANEL_W / 2) - dx, xr = PX(PANEL_W / 2) + dx;
  const uint16_t cl = (active == 2) ? C_PAGE : C_PANEL;
  const uint16_t cr = (active == 1) ? C_PAGE : C_PANEL;
  tft.fillTriangle(xl + h, y - h, xl + h, y + h, xl - h, y, cl);
  tft.fillTriangle(xr - h, y - h, xr - h, y + h, xr + h, y, cr);
}

/* Fade and repaint only lights that changed; everything after a full
 * repaint, whose fillScreen wiped the margins. */
static void drawStrikes(void) {
  static uint32_t next_fade = 0;
  const bool fade = (int32_t)(millis() - next_fade) >= 0;
  if (fade) next_fade = millis() + kGlowStepMs;

  const bool force = strikes_force;
  strikes_force = false;
  if (force || strikes_active != active) drawPointer();
  for (uint8_t src = 1; src <= 2; src++) {
    if (force || strikes_active != active) drawStrikeLabel(src);
    for (uint8_t f = 0; f < NUM_FIELDS; f++) {
      Glow &g = glow[src][f];
      if (fade && g.level && g.drawn != 0xFF) g.level--;
      if (force || g.level != g.drawn) drawStrike(src, f);
    }
  }
  strikes_active = active;
}

/* ── Redraw tiers ────────────────────────────────────────────────────── */

/* What each text slot and the page colour look like on the glass now, so a
 * repaint can skip everything that would come out the same. */
static char     drawn_txt[NUM_SLOTS][LBL + 1];
static uint16_t drawn_page_c = 0;

static bool textChanged(uint8_t slot, const char *s, bool force) {
  if (!force && strcmp(drawn_txt[slot], s) == 0) return false;
  strncpy(drawn_txt[slot], s, LBL);
  drawn_txt[slot][LBL] = '\0';
  return true;
}

/* Tier 3: page changed, or (duet) the other lab took the screen.  Only the
 * nine text cells and the page cues move - every page uses the same six pots
 * and three buttons, and jacks are declared per firmware rather than per
 * page.  Cells whose text is unchanged are left alone unless forced. */
static void drawLabels(bool force) {
  const Module &m = mod[active];
  for (uint8_t i = 0; i < NUM_KNOBS; i++)
    if (textChanged(SLOT_KNOB(i), m.text[SLOT_KNOB(i)], force)) { drawKnobLabel(i, m.text[SLOT_KNOB(i)]); PumpAll(); }
  for (uint8_t i = 0; i < NUM_BTNS; i++)
    if (textChanged(SLOT_BTN(i), m.text[SLOT_BTN(i)], force)) { drawButtonLabel(i, m.text[SLOT_BTN(i)]); PumpAll(); }
  const bool recolour = force || C_PAGE != drawn_page_c;
  if (recolour) {
    tft.drawFastHLine(PX(3), PYT(RULE_TOP), MM(PANEL_W - 6), C_PAGE);
    tft.drawFastHLine(PX(3), PYB(RULE_BOT), MM(PANEL_W - 6), C_PAGE);
  }
  if (textChanged(SLOT_PAGE, m.text[SLOT_PAGE], recolour))
    textCell(PX(17), PYB(BRAND_FROM_BOT), 46, 5, m.text[SLOT_PAGE], 1, C_PAGE);
  drawn_page_c = C_PAGE;
  drawModuleDots();
  PumpAll();
}

/* The switch is cheap only when the incoming lab would draw the same title
 * and jacks as the one on the glass. */
static bool sameFurniture(const Module &m) {
  if (strcmp(drawn_txt[SLOT_NAME], m.text[SLOT_NAME]) != 0) return false;
  for (uint8_t i = 0; i < NUM_JACKS; i++)
    if (strcmp(drawn_txt[SLOT_JACK(i)], m.text[SLOT_JACK(i)]) != 0) return false;
  return true;
}

/* Tier 2: knob positions and button lamps.  Only what moved. */
static void drawValues(bool force) {
  const Module &m = mod[active];
  for (uint8_t i = 0; i < NUM_KNOBS; i++) {
    if (force || m.val[i] != drawn_val[i]) {
      drawKnobFace(i, m.val[i]);
      drawn_val[i] = m.val[i];
      PumpAll();
    }
  }
  if (force || m.btn_flags != drawn_flags) {
    for (uint8_t i = 0; i < NUM_BTNS; i++) drawButtonFace(i, (m.btn_flags >> i) & 1);
    drawn_flags = m.btn_flags;
    PumpAll();
  }
}

/* Tier 1: everything.  Module changed, or first paint. */
static void drawAll(void) {
  const Module &m = mod[active];

  tft.fillScreen(C_BG);                       PumpAll();
  tft.fillRect(gOx, gOy, MM(PANEL_W), MM(PANEL_H), C_PANEL);
  tft.drawRect(gOx, gOy, MM(PANEL_W), MM(PANEL_H), C_IDLE);
  PumpAll();

  textAt(PX(PANEL_W / 2), PYT(TITLE_FROM_TOP),
         m.text[SLOT_NAME][0] ? m.text[SLOT_NAME] : "no link", 2, C_TEXT);
  textAt(PX(PANEL_W - 16), PYB(BRAND_FROM_BOT), "Hermetic Modular", 1, C_DIM);
  tft.drawRect(PX(PANEL_W / 2 - USB_W / 2), PYB(USB_BOTTOM + USB_H),
               MM(USB_W), MM(USB_H), C_IDLE);
  PumpAll();

  const float jy[2] = {JACK_Y_LOW, JACK_Y_LOW + JACK_Y_PITCH};
  const float lblUpY  = (jy[0] + jy[1]) / 2;
  const float lblLowY = jy[0] - JACK_D / 2 - LBL_JACK_GAP - 1.2f;
  for (uint8_t c = 0; c < JACK_COLS; c++) {
    drawJack(jackXmm(c), jy[0]);
    drawJack(jackXmm(c), jy[1]);
    textAt(PX(jackXmm(c)), PYB(lblUpY),  m.text[SLOT_JACK(c)],     1, C_TEXT);
    textAt(PX(jackXmm(c)), PYB(lblLowY), m.text[SLOT_JACK(c + 5)], 1, C_TEXT);
    PumpAll();
  }
  strncpy(drawn_txt[SLOT_NAME], m.text[SLOT_NAME], LBL);
  for (uint8_t i = 0; i < NUM_JACKS; i++) strncpy(drawn_txt[SLOT_JACK(i)], m.text[SLOT_JACK(i)], LBL);
  drawLabels(true);
  drawValues(true);
  strikes_force = true;
}

/* ── Frame handling ──────────────────────────────────────────────────── */

static uint8_t crc8(uint8_t crc, uint8_t b) {
  crc ^= b;
  for (uint8_t i = 0; i < 8; i++)
    crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
  return crc;
}

/* Returns true if the field actually changed. */
static bool copyField(char *dst, const uint8_t *src, uint8_t n) {
  char tmp[LBL + 1];
  if (n > LBL) n = LBL;
  memcpy(tmp, src, n);
  tmp[n] = '\0';
  for (int8_t i = (int8_t)n - 1; i >= 0 && (tmp[i] == ' ' || tmp[i] == '\0'); i--)
    tmp[i] = '\0';
  if (strcmp(tmp, dst) == 0) return false;
  strcpy(dst, tmp);
  return true;
}

static void light(uint8_t src, uint8_t kind, uint8_t field, uint8_t vel) {
  if (src >= NUM_SOURCES || field >= NUM_FIELDS) return;
  Glow &g = glow[src][field];
  g.level = kGlowSteps; g.kind = kind; g.vel = vel; g.drawn = 0xFF;
}

static void onEvent(uint8_t src, uint8_t kind, uint8_t field, uint8_t vel) {
  light(src, kind, field, vel);
  if (kind == EVT_ANSWER) { counts[src].answered++; return; }
  if (kind != EVT_PLAY) return;
  counts[src].played++;
  for (uint8_t j = 1; j < NUM_SOURCES; j++) {
    if (j == src || !i2c[j].present) continue;
    if (relay_n < sizeof relay_q / sizeof relay_q[0]) relay_q[relay_n++] = {src, j, field, vel};
  }
}

/* A TEXT frame landing mid-redraw can tear one label.  Harmless: the COMMIT
 * that follows the text always repaints, so the tear lasts a single pass. */
static void applyFrame(uint8_t src, uint8_t type, const uint8_t *p, uint8_t len) {
  Module &m = mod[src];
  m.seen = true;
  m.last_frame = millis();
  selftest = false;
#if DEBUG_ACK
  Serial.print(F("F src=")); Serial.print(src);
  Serial.print(F(" type=0x")); Serial.print(type, HEX);
  Serial.print(F(" len=")); Serial.println(len);
#endif

  if (type == MSG_TEXT && len >= 1) {   /* len 1 = clear this slot */
    uint8_t slot = p[0];
    if (slot < NUM_SLOTS && copyField(m.text[slot], p + 1, len - 1)) m.dirty = true;
  }
  else if (type == MSG_COMMIT && len >= 6) {
    m.page    = p[1];
    m.n_pages = p[2];
    m.rgb[0]  = p[3]; m.rgb[1] = p[4]; m.rgb[2] = p[5];
    /* A full commit repaints only if it brings something new: first contact,
     * or a module that rebooted into different firmware.  An ANNOUNCE this
     * sketch asked for twice (the ask is on a timer) changes nothing and must
     * not blank the panel. */
    const bool fresh = !m.ready || m.dirty;
    m.ready = true;
    m.dirty = false;
    if (src == (uint8_t)active) {
      if (!p[0])              need_lbls = true;   /* page change */
      else if (fresh)         need_full = true;
    }
    else if (p[0] && fresh) { if (src == (uint8_t)drawn_mod) drawn_mod = -1; }
  }
  else if (type == MSG_STATE && len >= 3 + NUM_KNOBS) {
    m.page      = p[0];
    m.btn_flags = p[1];
    if (p[2]) m.last_activity = millis();
    for (uint8_t i = 0; i < NUM_KNOBS; i++) m.val[i] = p[3 + i];
  }
  else if (type == MSG_EVENT && len >= 3) {
    onEvent(src, p[0], p[1], p[2]);
  }
}

static void feed(uint8_t src, uint8_t b) {
  Parser &ps = parser[src];
  switch (ps.state) {
    case 0: if (b == 0xAA) ps.state = 1; break;
    case 1: ps.state = (b == 0x55) ? 2 : (b == 0xAA ? 1 : 0); break;
    case 2: ps.type = b; ps.crc = crc8(0, b); ps.state = 3; break;
    case 3:
      ps.len = b; ps.crc = crc8(ps.crc, b); ps.idx = 0;
      ps.state = (ps.len > MAX_PAYLOAD) ? 0 : (ps.len ? 4 : 5);
      break;
    case 4:
      ps.buf[ps.idx++] = b; ps.crc = crc8(ps.crc, b);
      if (ps.idx >= ps.len) ps.state = 5;
      break;
    case 5:
      if (b == ps.crc) applyFrame(src, ps.type, ps.buf, ps.len);
      ps.state = 0;
      break;
  }
}

static void PollI2C(void);   /* fwd */
static void serialByte(uint8_t b);

/* Called between draw steps. USB bytes, and - for duet - the I2C labs too,
 * so a strike played during a repaint is relayed in milliseconds rather
 * than after it. A poll is only a read when one is due, ~3 ms each. */
static void PumpAll(void) {
  while (Serial.available()) serialByte((uint8_t)Serial.read());
  PollI2C();
}

/* ── I2C polling ─────────────────────────────────────────────────────── */

/* Nine clocks with SDA released, then a STOP: frees a slave that was
 * mid-byte, holding SDA low, when this Mega last reset. */
static void busRecover(void) {
  pinMode(SDA, INPUT_PULLUP);
  pinMode(SCL, OUTPUT);
  for (uint8_t i = 0; i < 9 && digitalRead(SDA) == LOW; i++) {
    digitalWrite(SCL, LOW);  delayMicroseconds(5);
    digitalWrite(SCL, HIGH); delayMicroseconds(5);
  }
  pinMode(SDA, OUTPUT);
  digitalWrite(SDA, LOW);  delayMicroseconds(5);
  digitalWrite(SCL, HIGH); delayMicroseconds(5);
  digitalWrite(SDA, HIGH); delayMicroseconds(5);
  pinMode(SDA, INPUT); pinMode(SCL, INPUT);
}

static bool sendHear(uint8_t to, uint8_t field, uint8_t vel) {
  Wire.beginTransmission(kI2cAddr[to]);
  Wire.write(CMD_HEAR);
  Wire.write(field);
  Wire.write(vel);
  const uint8_t e = Wire.endTransmission();
  if (Wire.getWireTimeoutFlag()) Wire.clearWireTimeoutFlag();
  if (e == 0) counts[to].heard++;
  return e == 0;
}

static void flushRelays(void) {
  for (uint8_t r = 0; r < relay_n; r++) {
    const Relay &q = relay_q[r];
    const bool ok = sendHear(q.to, q.field, q.vel);
    Serial.print(F("0x")); Serial.print(kI2cAddr[q.from], HEX);
    Serial.print(F(" PLAY f")); Serial.print(q.field);
    Serial.print(F(" v")); Serial.print(q.vel);
    Serial.print(F(" -> 0x")); Serial.print(kI2cAddr[q.to], HEX);
    Serial.println(ok ? F(" ok") : F(" NACK"));
  }
  relay_n = 0;
}

/* "a3": lab 0x42 hears field 3.  "b7": lab 0x43 hears field 7.  Drawn as a
 * played strike on the other lab, which is what it stands in for.  Every
 * byte also goes to the frame parser, as in panel_i2c; neither cares about
 * the other's bytes. */
static void serialByte(uint8_t b) {
  static char pending = 0;
  feed(0, b);
  if (b == 'a' || b == 'b') { pending = (char)b; return; }
  if (pending && b >= '0' && b <= '8') {
    const uint8_t to = (pending == 'a') ? 1 : 2, field = (uint8_t)(b - '0');
    light(to == 1 ? 2 : 1, EVT_PLAY, field, 200);
    const bool ok = sendHear(to, field, 200);
    Serial.print(F("test: 0x")); Serial.print(kI2cAddr[to], HEX);
    Serial.print(F(" hears f")); Serial.print(field);
    Serial.println(ok ? F(" ok") : F(" NACK"));
  }
  pending = 0;
}

static void askAnnounce(uint8_t addr) {
  Wire.beginTransmission(addr);
  Wire.write(CMD_ANNOUNCE);
  Wire.endTransmission();
}

static void pollSource(uint8_t src) {
  I2cSource &s = i2c[src];
  const uint32_t now = millis();
  if ((int32_t)(now - s.next_poll) < 0) return;

  const uint8_t got = Wire.requestFrom(kI2cAddr[src], kReadLen);
  if (Wire.getWireTimeoutFlag()) {
    Wire.clearWireTimeoutFlag();
#if DEBUG_I2C
    Serial.print(F("I2C timeout at 0x")); Serial.println(kI2cAddr[src], HEX);
#endif
  }

  if (got != kReadLen) {                 /* NACK: nobody at this address */
    while (Wire.available()) Wire.read();
#if DEBUG_I2C
    s.nacks++;
    if (s.present) { Serial.print(F("0x")); Serial.print(kI2cAddr[src], HEX); Serial.println(F(" GONE")); }
#endif
    s.present   = false;
    s.next_poll = now + kAbsentPollMs;
    return;
  }

  uint8_t n = (uint8_t)Wire.read();
#if DEBUG_I2C
  s.reads++;
  if (n > kReadLen - 1) { Serial.print(F("bad count byte 0x")); Serial.println(n, HEX); }
  else s.bytes += n;
  if (!s.present) { Serial.print(F("0x")); Serial.print(kI2cAddr[src], HEX); Serial.println(F(" ANSWERS")); }
#endif
  if (n > kReadLen - 1) n = 0;           /* 0xFF = a floating bus, not data */
  for (uint8_t i = 0; i < kReadLen - 1; i++) {
    const uint8_t b = (uint8_t)Wire.read();
    if (i < n) feed(src, b);
  }
  flushRelays();

  if (!s.present) {                      /* just appeared: ask straight away */
    s.present  = true;
    s.next_ask = now;
  }
  if (!mod[src].ready && (int32_t)(now - s.next_ask) >= 0) {
#if DEBUG_I2C
    s.asks++;
#endif
    askAnnounce(kI2cAddr[src]);
    s.next_ask = now + kAnnounceAskMs;
  }

  /* A full read means more is waiting - come straight back for it. */
  s.next_poll = (n == kReadLen - 1) ? now : now + kPollMs;
}

static void PollI2C(void) {
  static bool busy = false;
  if (busy) return;
  busy = true;
  for (uint8_t i = 1; i < NUM_SOURCES; i++) pollSource(i);
  busy = false;
#if DEBUG_I2C
  static uint32_t next_stats = 2000ul;
  if ((int32_t)(millis() - next_stats) >= 0) {
    next_stats = millis() + 2000ul;
    for (uint8_t i = 1; i < NUM_SOURCES; i++) {
      I2cSource &s = i2c[i];
      if (!s.present && !s.reads) { s.nacks = 0; continue; }
      Serial.print(F("0x")); Serial.print(kI2cAddr[i], HEX);
      Serial.print(F(" reads=")); Serial.print(s.reads);
      Serial.print(F(" bytes=")); Serial.print(s.bytes);
      Serial.print(F(" nacks=")); Serial.print(s.nacks);
      Serial.print(F(" asks="));  Serial.print(s.asks);
      Serial.print(F(" ready=")); Serial.println(mod[i].ready);
      s.reads = s.nacks = s.bytes = s.asks = 0;
    }
  }
#endif
}

#if DEBUG_I2C
/* Before Wire.begin: the lines with only the external pull-ups on them.
 * 1/1 = pulled up, idle.  0 on either = held low or no pull-up at all. */
static void reportLines(void) {
  pinMode(SDA, INPUT); pinMode(SCL, INPUT);
  delay(2);
  Serial.print(F("idle lines, no internal pull-ups: SDA=")); Serial.print(digitalRead(SDA));
  Serial.print(F(" SCL=")); Serial.println(digitalRead(SCL));
}

/* Listen without driving anything, Mega pull-ups off.  An I2C slave is
 * silent until addressed, so ANY edge here means something else is driving
 * the lines - e.g. a UART firmware whose TX is on B3 (SCL), which bursts
 * every 3 s.  Also catches a line that is actively held low. */
static void sniffLines(uint16_t ms) {
  pinMode(SDA, INPUT); pinMode(SCL, INPUT);
  uint32_t sclEdges = 0, sdaEdges = 0, sclLowUs = 0, sdaLowUs = 0;
  uint8_t  lastScl = digitalRead(SCL), lastSda = digitalRead(SDA);
  const uint32_t t0 = millis();
  uint32_t tPrev = micros();
  while (millis() - t0 < ms) {
    const uint8_t c = digitalRead(SCL), d = digitalRead(SDA);
    const uint32_t t = micros(), dt = t - tPrev; tPrev = t;
    if (!c) sclLowUs += dt;
    if (!d) sdaLowUs += dt;
    if (c != lastScl) { sclEdges++; lastScl = c; }
    if (d != lastSda) { sdaEdges++; lastSda = d; }
  }
  Serial.print(F("sniff ")); Serial.print(ms); Serial.print(F(" ms: SCL edges="));
  Serial.print(sclEdges); Serial.print(F(" low=")); Serial.print(sclLowUs / 1000);
  Serial.print(F("ms  SDA edges=")); Serial.print(sdaEdges); Serial.print(F(" low="));
  Serial.print(sdaLowUs / 1000); Serial.println(F("ms"));
}

/* endTransmission: 0 ACK, 2 address NACK, 3 data NACK, 4 other (bus
 * error / lost arbitration), 5 timeout.  A healthy empty bus is all 2s. */
static void scanBus(void) {
  uint8_t codes[6] = {0};
  Serial.print(F("scan:"));
  for (uint8_t a = 0x08; a <= 0x77; a++) {
    Wire.beginTransmission(a);
    const uint8_t e = Wire.endTransmission();
    if (e < 6) codes[e]++;
    if (e == 0) { Serial.print(F(" 0x")); Serial.print(a, HEX); }
    if (Wire.getWireTimeoutFlag()) Wire.clearWireTimeoutFlag();
  }
  if (!codes[0]) Serial.print(F(" nothing answered"));
  Serial.print(F("  [ack=")); Serial.print(codes[0]);
  Serial.print(F(" nack=")); Serial.print(codes[2]);
  Serial.print(F(" other=")); Serial.print(codes[4]);
  Serial.print(F(" timeout=")); Serial.print(codes[5]); Serial.println(F("]"));
}
#endif

/* ── Arbitration ─────────────────────────────────────────────────────── */

/* Whoever was touched most recently wins, but the module on screen keeps the
 * display until it has been quiet for kHoldoffMs.  Reaching for a knob is the
 * selection gesture; no button anywhere selects a module. */
static void arbitrate(void) {
  int8_t best = -1; uint32_t newest = 0;
  for (uint8_t i = 0; i < NUM_SOURCES; i++) {
    if (!mod[i].ready) continue;
    if (best < 0 || (int32_t)(mod[i].last_activity - newest) > 0) {
      best = (int8_t)i; newest = mod[i].last_activity;
    }
  }
  if (best < 0 || best == active) return;
  if (active < 0) { active = best; return; }
  if (millis() - mod[active].last_activity >= kHoldoffMs) active = best;
}

/* ── Self test ───────────────────────────────────────────────────────── */

/* Runs until a real frame arrives, so the display is verifiable with nothing
 * attached.  Mirrors the handpan pages in src/duet/duet.cpp. */
static const char *kStJack[NUM_JACKS] = {
  "STRIKE","NOTE","POS","RING","OUT L","EXC","ACCENT","DAMP","SYMP","OUT R" };
static const char *kStPage[3] = {"Play","Voicing","Build"};
static const uint8_t kStRgb[3][3] = {
  {0xff,0xa8,0x30},{0x90,0xe0,0xff},{0x60,0xff,0xc0} };
static const char *kStKnob[3][NUM_KNOBS] = {
  {"Scale","Root","Position","Mallet","Decay","Sympathy"},
  {"Temper","Shimmer","Metal","Tilt","Body","Contact"},
  {"Gu Tune","Fine","Spread","Dynamics","Exciter In","Level"} };
static const char *kStBtn[3][NUM_BTNS] = {
  {"PAGE","BUILD","HAND"},{"PAGE","BUILD","HAND"},{"","HELD","LOAD"} };

static void selftestTick(void) {
  static uint32_t next_page = 0;
  static uint8_t  pg = 0;
  Module &m = mod[0];

  if (!m.ready) {
    m.seen = m.ready = true;
    /* Titled SELF TEST, not "Handpan": a demo that renders identically to a
     * live module is worse than no demo - it reads as a working link. */
    strncpy(m.text[SLOT_NAME], "SELF TEST", LBL);
    for (uint8_t i = 0; i < NUM_JACKS; i++)
      strncpy(m.text[SLOT_JACK(i)], kStJack[i], LBL);
    m.n_pages = 3;
    m.last_activity = millis();
    next_page = millis();
  }
  if ((int32_t)(millis() - next_page) >= 0) {
    next_page = millis() + 4000ul;
    m.page = pg;
    strncpy(m.text[SLOT_PAGE], kStPage[pg], LBL);
    memcpy(m.rgb, kStRgb[pg], 3);
    for (uint8_t i = 0; i < NUM_KNOBS; i++) strncpy(m.text[SLOT_KNOB(i)], kStKnob[pg][i], LBL);
    for (uint8_t i = 0; i < NUM_BTNS;  i++) strncpy(m.text[SLOT_BTN(i)],  kStBtn[pg][i],  LBL);
    m.btn_flags = 1u << pg;
    pg = (pg + 1) % 3;
  }
  const float t = millis() / 1000.0f;
  for (uint8_t i = 0; i < NUM_KNOBS; i++)
    m.val[i] = (uint8_t)(127.5f + 127.0f * sin(t * (0.3f + 0.11f * i) + i));
}

/* ── Once a second, on USB serial ────────────────────────────────────── */

static void secondTick(void) {
  Serial.print(F("t=")); Serial.print(millis() / 1000ul);
  Serial.print(F(" active=0x")); Serial.print(active > 0 ? kI2cAddr[active] : 0, HEX);
  for (uint8_t i = 1; i <= 2; i++) {
    const Module &m = mod[i];
    Serial.print(F(" | 0x")); Serial.print(kI2cAddr[i], HEX);
    Serial.print(i2c[i].present ? F(" up ") : F(" down "));
    Serial.print(m.text[SLOT_NAME]); Serial.print(' '); Serial.print(m.text[SLOT_PAGE]);
    Serial.print(F(" knobs="));
    for (uint8_t k = 0; k < NUM_KNOBS; k++) { Serial.print(m.val[k]); if (k < NUM_KNOBS - 1) Serial.print(','); }
    Serial.print(F(" played=")); Serial.print(counts[i].played);
    Serial.print(F(" heard="));  Serial.print(counts[i].heard);
    Serial.print(F(" answered=")); Serial.print(counts[i].answered);
  }
  Serial.println();
}

/* ── Main ────────────────────────────────────────────────────────────── */

void setup() {
  Serial.begin(kBaud);

#if DEBUG_I2C
  Serial.println(F("panel_i2c boot"));
  reportLines();
  sniffLines(4000);
#endif
  busRecover();
  Wire.begin();                   /* master; also enables internal pull-ups */
  Wire.setClock(kI2cClock);
  Wire.setWireTimeout(kWireTimeoutUs, true);   /* reset TWI on a hang */
#if DEBUG_I2C
  scanBus();
#endif

  tft.begin(0x9488);              /* RD is not routed; see handpan_panel.ino */
  tft.setRotation(0);
  tft.setTextWrap(false);

  const int margin = 6;
  gScale = min((tft.height() - 2.0f * margin) / PANEL_H,
               (tft.width()  - 2.0f * margin) / PANEL_W);
  gOx = (tft.width()  - MM(PANEL_W)) / 2;
  gOy = (tft.height() - MM(PANEL_H)) / 2;

  tft.fillScreen(C_BG);
  textAt(tft.width() / 2, tft.height() / 2, "waiting for link", 1, C_DIM);
  Serial.println(F("duet ready"));
}

void loop() {
  PumpAll();
  if (selftest) selftestTick();

  static uint32_t next_sec = 1000ul;
  if ((int32_t)(millis() - next_sec) >= 0) { next_sec += 1000ul; secondTick(); }

  arbitrate();
  if (active < 0) return;

  Module &m = mod[active];
  if (!m.ready) return;

  C_PAGE = RGB(m.rgb[0], m.rgb[1], m.rgb[2]);

  if (active != drawn_mod && drawn_mod >= 0 && !need_full && sameFurniture(m)) {
    /* duet: the other lab took the screen - repaint only what differs */
    const bool recolour = C_PAGE != drawn_page_c;
    drawn_mod = active; drawn_page = m.page;
    need_lbls = false;
    drawLabels(false);
    drawValues(recolour);                    /* rings carry the page colour */
  }
  else if (active != drawn_mod || need_full) {     /* tier 1 */
    drawn_mod = active; drawn_page = m.page;
    need_full = need_lbls = false;
    drawAll();
  }
  else if (m.page != drawn_page || need_lbls) {   /* tier 3 - the B1 path */
    const bool recolour = C_PAGE != drawn_page_c;
    drawn_page = m.page; need_lbls = false;
    drawLabels(false);
    drawValues(recolour);                    /* rings carry the page colour */
  }
  else {                                     /* tier 2 */
    drawValues(false);
  }
  drawStrikes();

  /* Link watchdog. Without it a pulled wire just freezes the panel on its
   * last good frame, which looks exactly like a module sitting still. */
  if (!selftest) {
    const bool stale = (millis() - m.last_frame) > kLinkTimeoutMs;
    if (stale != drawn_stale) {
      drawn_stale = stale;
      textCell(PX(17), PYB(BRAND_FROM_BOT), 46, 5,
               stale ? "NO LINK" : m.text[SLOT_PAGE], 1,
               stale ? RGB(255, 60, 40) : C_PAGE);
      strncpy(drawn_txt[SLOT_PAGE], stale ? "NO LINK" : m.text[SLOT_PAGE], LBL);
    }
  }
}
