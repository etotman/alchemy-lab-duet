/**
 * mirrors/duet.cpp — Render src/duet's HostLink descriptor.
 *
 * A copy of src/duet's control surfaces, compiled natively to render the
 * descriptor the module will report.
 *
 * Nothing here touches hardware: the Alchemy SDK's descriptor layer is
 * board-free and compiles natively against lib/alchemy-sdk/stubs.  Run it
 * with tools/descriptor_probe/run.sh, which also validates the output.
 *
 * WHY: a module whose Presets manages nothing renders a descriptor saying
 * "size":0,"components":[] — no controls, no state.  It boots, makes sound
 * and enumerates on USB perfectly, but the web programmer hangs at
 * "reconnect and verify firmware".  See the README.
 *
 * MAINTENANCE: the block between the MIRROR markers is a deliberate copy of
 * src/duet's control surfaces.  It is not compiled from src/, so it can
 * drift.  Update it whenever you add or rename a knob, page, or jack — and
 * note that run.sh's static checks read src/duet directly, so the check
 * that catches the bug above stays correct even if this copy goes stale.
 *
 * Four pages here, not one: Play, Voicing and Duet cycle on B1, Build is a
 * held layer on B2.  The Pager therefore has to be built with four pages or
 * the descriptor advertises storage for fewer and the last page's knobs
 * silently lose their preset slots.
 */

#include <cstdio>
#include <cstring>

#include "alchemy/host_link/describe.h"
#include "alchemy/host_link/descriptor.h"
#include "alchemy/hw/alchemy_lab.h"
#include "alchemy/surface/jack.h"
#include "alchemy/surface/manual.h"
#include "alchemy/surface/page.h"
#include "alchemy/surface/pager.h"
#include "alchemy/surface/presets.h"
#include "alchemy/surface/virtual_knob.h"

using namespace alchemy;
using namespace alchemy::hostlink;

static daisy::QSPIHandle g_qspi;   /* stub — never touched on the host */

/* ─────────────────── MIRROR: firmware surfaces begin ─────────────────── */

static constexpr float kRootMinHz     = 65.41f;
static constexpr float kRootMaxHz     = 261.63f;
static constexpr float kDecayMinSec   = 0.25f;
static constexpr float kDecayMaxSec   = 16.f;
static constexpr float kTemperMax     = 0.4f;
static constexpr float kFineMaxCents  = 50.f;

static constexpr uint8_t kNumScalesMirror = 8;

enum : uint8_t {
    kPagePlay = 0, kPageVoicing = 1, kPageDuet = 2, kPageBuild = 3, kNumPages = 4
};

static const char* const kAnswerLabels[4] = { "Off", "Canon", "Mirror", "Scatter" };
static const char* const kIntervalLabels[9] = {
    "-4", "-3", "-2", "-1", "0", "+1", "+2", "+3", "+4"
};
static const char* const kRepeatLabels[4] = { "1", "2", "3", "4" };

static const char* const kScaleLabels[kNumScalesMirror] = {
    "Kurd", "Celtic", "Hijaz", "Pygmy", "Dorian", "Major", "Harmonic", "Insen"
};

static const DescriptorBuilder::ModuleInfo kInfo = {
    "duet", "Duet", "0.1.0", "i2c", "host-probe", "v2"
};

static AlchemyLab hw;                       /* stub-backed on the host */
static Pager      pager(kNumPages, kNumPots);
static Presets    presets(g_qspi);

/* ── Play ── */
static VirtualKnob k_scale = VirtualKnob(kPotTopLeft, "Scale")
    .Selector(kNumScalesMirror).Labels(kScaleLabels).Ident("pan.scale");
static VirtualKnob k_root = VirtualKnob(kPotTopRight, "Root")
    .Exp(kRootMinHz, kRootMaxHz).Ident("pan.root").Unit("Hz");
static VirtualKnob k_pos = VirtualKnob(kPotMiddleLeft, "Position")
    .Linear(0.f, 1.f).Ident("strike.position").Cv(2);
static VirtualKnob k_mallet = VirtualKnob(kPotMiddleRight, "Mallet")
    .Linear(0.f, 1.f).Ident("strike.mallet");
static VirtualKnob k_decay = VirtualKnob(kPotBottomLeft, "Decay")
    .Exp(kDecayMinSec, kDecayMaxSec).Ident("shell.decay").Unit("s");
static VirtualKnob k_symp = VirtualKnob(kPotBottomRight, "Sympathy")
    .Linear(0.f, 1.f).Ident("shell.sympathy").Cv(5);

/* ── Voicing ── */
static VirtualKnob k_temper = VirtualKnob(kPotTopLeft, "Temper")
    .Linear(-kTemperMax, kTemperMax).Ident("voice.temper");
static VirtualKnob k_shimmer = VirtualKnob(kPotTopRight, "Shimmer")
    .Linear(0.f, 1.f).Ident("voice.shimmer");
static VirtualKnob k_metal = VirtualKnob(kPotMiddleLeft, "Metal")
    .Linear(0.f, 1.f).Ident("voice.metal");
static VirtualKnob k_tilt = VirtualKnob(kPotMiddleRight, "Tilt")
    .Linear(0.f, 1.f).Ident("voice.tilt");
static VirtualKnob k_cavlvl = VirtualKnob(kPotBottomLeft, "Body")
    .Linear(0.f, 1.f).Ident("voice.body");
static VirtualKnob k_contact = VirtualKnob(kPotBottomRight, "Contact")
    .Linear(0.f, 1.f).Ident("voice.contact");

/* ── Duet ── */
static VirtualKnob k_answer = VirtualKnob(kPotTopLeft, "Answer")
    .Selector(4).Labels(kAnswerLabels).Ident("duet.answer");
static VirtualKnob k_delay = VirtualKnob(kPotTopRight, "Delay")
    .Exp(60.f, 2000.f).Ident("duet.delay").Unit("ms");
static VirtualKnob k_interval = VirtualKnob(kPotMiddleLeft, "Interval")
    .Selector(9).Labels(kIntervalLabels).Ident("duet.interval");
static VirtualKnob k_repeats = VirtualKnob(kPotMiddleRight, "Repeats")
    .Selector(4).Labels(kRepeatLabels).Ident("duet.repeats");
static VirtualKnob k_chance = VirtualKnob(kPotBottomLeft, "Chance")
    .Linear(0.f, 1.f).Ident("duet.chance");
static VirtualKnob k_ansvel = VirtualKnob(kPotBottomRight, "Answer Vel")
    .Linear(0.1f, 1.f).Ident("duet.velocity");

/* ── Build ── */
static VirtualKnob k_cavtune = VirtualKnob(kPotTopLeft, "Gu Tune")
    .Linear(0.5f, 1.8f).Ident("build.gu");
static VirtualKnob k_fine = VirtualKnob(kPotTopRight, "Fine")
    .Linear(-kFineMaxCents, kFineMaxCents).Ident("build.fine").Unit("cents");
static VirtualKnob k_spread = VirtualKnob(kPotMiddleLeft, "Spread")
    .Linear(0.f, 1.f).Ident("build.spread");
static VirtualKnob k_dyn = VirtualKnob(kPotMiddleRight, "Dynamics")
    .Linear(0.f, 1.f).Ident("build.dynamics");
static VirtualKnob k_ext = VirtualKnob(kPotBottomLeft, "Exciter In")
    .Linear(0.f, 1.f).Ident("build.ext");
static VirtualKnob k_level = VirtualKnob(kPotBottomRight, "Level")
    .Linear(0.f, 1.f).Ident("build.level");

static Page page_play  = Page(kPagePlay).Name("Play").Color("#ffa830");
static Page page_voice = Page(kPageVoicing).Name("Voicing").Color("#90e0ff");
static Page page_duet  = Page(kPageDuet).Name("Duet").Color("#ff70d0");
static Page page_build = Page(kPageBuild).Name("Build").Color("#60ffc0");

static Jack j_strike("STRIKE", "Strike",      JackSig::Trig);
static Jack j_exc   ("EXC",    "Exciter In",  JackSig::AudioIn);
static Jack j_note  ("NOTE",   "Note",        JackSig::Voct);
static Jack j_accent("ACCENT", "Accent",      JackSig::CvBi);
static Jack j_pos   ("POS",    "Position CV", JackSig::CvBi);
static Jack j_damp  ("DAMP",   "Damp CV",     JackSig::CvUni);
static Jack j_ring  ("RING",   "Ring Energy", JackSig::CvUni);
static Jack j_symp  ("SYMP",   "Sympathy CV", JackSig::CvBi);
static Jack j_out_l ("OUT_L",  "Out L",       JackSig::AudioOut);
static Jack j_out_r ("OUT_R",  "Out R",       JackSig::AudioOut);

static Manual manual =
    Manual()
        .Tagline("Two handpans that answer each other")
        .Preamble("**Handpan** is a modelled hang drum: nine tone fields, a "
                  "steel shell and an air cavity, all ringing at once. Each "
                  "field's first three modes are locked to f, 2f and 3f, and "
                  "a strike on one is carried through the shell into all the "
                  "others. Two Labs on the TFT panel's bus answer each "
                  "other through it.")
        .Section("duet", "Duet — the answer",
                 "A strike played here is relayed to the other pan, whose "
                 "Duet page answers it: Canon, Mirror or Scatter, after "
                 "Delay, Repeats times, with Chance and Answer Vel.")
        .Section("strike", "Strike and velocity",
                 "STRIKE takes a trigger, and the height of that trigger is "
                 "how hard the pan is hit. ACCENT offsets it.")
        .Section("note", "Note",
                 "NOTE is 1 V/oct, quantised to the nine tone fields. 0 V is "
                 "the ding. Past the top field or below the ding the request "
                 "is folded by octaves back into the pan's compass, so every "
                 "voltage picks a note.")
        .Section("hand", "The hand",
                 "B3 taps to strike and holds to mute. Position is where the "
                 "hand lands; Mallet is what lands.")
        .Section("shell", "Decay and Sympathy",
                 "Decay is how long the ding rings. Sympathy is how much of "
                 "a strike the untouched fields answer with.")
        .Section("temper", "Temper",
                 "The tuned triple sits at n^(1+temper), so dead centre is "
                 "a real handpan's 1:2:3. Left compresses toward bell "
                 "metal, right stretches toward gongs. The fundamental "
                 "never moves.")
        .Section("voicing", "Voicing",
                 "Shimmer, Metal, Tilt, Body and Contact are the rest of "
                 "the tuner's bench — how the shell is built.")
        .Section("build", "Build layer, and EXC",
                 "Held on B2: Gu Tune, Fine, Spread, Dynamics, Exciter In "
                 "and Level. Anything patched to EXC is poured into all "
                 "nine fields.");

/** Everything main() does to the surfaces before the first loop.Tick(). */
static void Compose()
{
    presets.Manage(pager);

    page_play.Knobs(k_scale, k_root, k_pos, k_mallet, k_decay, k_symp);
    page_voice.Knobs(k_temper, k_shimmer, k_metal, k_tilt, k_cavlvl, k_contact);
    page_duet.Knobs(k_answer, k_delay, k_interval, k_repeats, k_chance, k_ansvel);
    page_build.Knobs(k_cavtune, k_fine, k_spread, k_dyn, k_ext, k_level);

    page_play.Help("The instrument. B1 cycles Voicing and Duet; hold B2 for Build.");
    page_voice.Help("How the shell is built, rather than how it is played.");
    page_duet.Help("How this pan answers a strike played on the other one.");
    page_build.Help("Held on B2 — tuning reference, stereo, and gains.");

    k_scale.Help("Which handpan tuning the nine fields are cut to.");
    k_root.Help("Ding fundamental, C2 to C4.").SeeAlso(j_note);
    k_pos.Help("Where the hand lands: dimple centre to shoulder.")
        .SeeAlso(j_pos);
    k_mallet.Help("What lands: the width of the strike impulse.");
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

    k_answer.Help("Off, Canon, Mirror or Scatter.");
    k_delay.Help("Time to the first answer, and between repeats.");
    k_interval.Help("Tone fields to move the answer by, -4 to +4.");
    k_repeats.Help("How many answers per strike heard.");
    k_chance.Help("How often a heard strike is answered at all.");
    k_ansvel.Help("Answer velocity as a share of the velocity heard.");

    k_cavtune.Help("Helmholtz frequency as a ratio of the ding.");
    k_fine.Help("Detune the whole instrument, to sit with other players.");
    k_spread.Help("Stereo width of the tone-field layout around the shell.");
    k_dyn.Help("How much velocity changes timbre and pitch, not just level.")
        .SeeAlso(j_strike);
    k_ext.Help("Gain of the external exciter input.").SeeAlso(j_exc);
    k_level.Help("Output level.");

    j_strike.Help("Strike trigger; its peak height sets the velocity.")
        .SeeAlso(k_dyn);
    j_exc.Help("Any audio, poured into all nine tone fields.").SeeAlso(k_ext);
    j_note.Help("1 V/oct, quantised to the nine tone fields. 0 V is the "
                "ding; past the top note it folds back down an octave.")
        .SeeAlso(k_root).SeeAlso(k_scale);
    j_accent.Help("Adds or subtracts strike velocity, +/-5 V.");
    j_pos.Help("Strike position, summed with the Position knob.")
        .SeeAlso(k_pos);
    j_damp.Help("Palm mute, 0-5 V. Does what holding B3 does.");
    j_ring.Help("Output: 0-5 V envelope of the instrument's ringing energy.")
        .SeeAlso(k_symp);
    j_symp.Help("Sympathy, summed with the Sympathy knob.").SeeAlso(k_symp);
    j_out_l.Help("Left output.");
    j_out_r.Help("Right output.");
}

static const Page* kPageRefs[] = { &page_play, &page_voice, &page_duet, &page_build };
static const Jack* kJackRefs[] = {
    &j_strike, &j_exc, &j_note, &j_accent, &j_pos,
    &j_damp, &j_ring, &j_symp, &j_out_l, &j_out_r
};

/* ──────────────────── MIRROR: firmware surfaces end ──────────────────── */

int main()
{
    Compose();

    const PageSet pages{kPageRefs, sizeof kPageRefs / sizeof kPageRefs[0]};
    const uint8_t num_jacks = sizeof kJackRefs / sizeof kJackRefs[0];

    static char buf[24 * 1024];   /* same capacity the SDK gives the target */
    const uint32_t len = RenderDescriptor(
        buf, sizeof buf, kInfo, presets, &pages,
        nullptr, 0,               /* describe overrides */
        nullptr, 0,               /* buttons            */
        nullptr, 0,               /* root fragments     */
        kJackRefs, num_jacks, &manual,
        nullptr, 0);              /* factory defaults   */

    if (len == 0u)
    {
        std::fprintf(stderr,
                     "probe: RenderDescriptor returned 0 — the buffer could "
                     "not hold even a minimal error descriptor.\n");
        return 2;
    }

    std::fwrite(buf, 1, len, stdout);
    std::fputc('\n', stdout);
    return 0;
}
