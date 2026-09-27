// SynthGen: offline procedural music generator for the stock library.
//
// Every note is rendered independently into stereo bus buffers (drums, bass,
// music, fx) plus two send buses (delay, reverb). After all notes are placed,
// the send effects run, the kick-driven sidechain ducks bass and music, and a
// soft-knee master clipper + peak normalisation produce the final track.
#include "SynthGen.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kSr = float(kSampleRate);
constexpr float kInvSr = 1.0f / kSr;

// ------------------------------------------------------------- DSP helpers

struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 0x9E3779B9u) {}
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    float uni() { return float(next() >> 8) * (1.0f / 16777216.0f); }
    float bi() { return uni() * 2.0f - 1.0f; }
    int range(int n) { return int(next() % uint32_t(n)); }
    bool chance(float p) { return uni() < p; }
};

struct SineTable {
    static constexpr int kSize = 4096;
    float t[kSize + 1];
    SineTable() {
        for (int i = 0; i <= kSize; ++i) t[i] = float(std::sin(6.283185307179586 * i / kSize));
    }
};
const SineTable kSine;

// ph must be in [0, 1).
inline float fsin(float ph) {
    float x = ph * float(SineTable::kSize);
    int i = int(x);
    float f = x - float(i);
    return kSine.t[i] + (kSine.t[i + 1] - kSine.t[i]) * f;
}

inline float wrap01(float ph) { return ph - std::floor(ph); }

inline float ftanh(float x) {
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

inline float midiHz(float m) { return 440.0f * std::pow(2.0f, (m - 69.0f) / 12.0f); }

inline float decayCoef(float tau) { return std::exp(-kInvSr / tau); }

inline void panGains(float pan, float& gl, float& gr) {
    float a = (pan + 1.0f) * 0.25f * kPi;
    gl = std::cos(a) * 1.41421356f;
    gr = std::sin(a) * 1.41421356f;
}

// Topology-preserving state variable filter (Simper/Cytomic).
struct Svf {
    float k = 1, a1 = 0, a2 = 0, a3 = 0, ic1 = 0, ic2 = 0;
    void set(float fc, float q) {
        fc = std::clamp(fc, 20.0f, kSr * 0.45f);
        float g = std::tan(kPi * fc * kInvSr);
        k = 1.0f / q;
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    void tick(float v0, float& lp, float& bp, float& hp) {
        float v3 = v0 - ic2;
        float v1 = a1 * ic1 + a2 * v3;
        float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        lp = v2;
        bp = v1;
        hp = v0 - k * v1 - v2;
    }
    float lp(float x) { float l, b, h; tick(x, l, b, h); return l; }
    float bp(float x) { float l, b, h; tick(x, l, b, h); return b; }
    float hp(float x) { float l, b, h; tick(x, l, b, h); return h; }
};

inline float blep(float t, float dt) {
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

struct Osc {
    float ph = 0;
    void adv(float dt) {
        ph += dt;
        if (ph >= 1.0f) ph -= 1.0f;
    }
    float saw(float dt) {
        float v = 2.0f * ph - 1.0f - blep(ph, dt);
        adv(dt);
        return v;
    }
    float square(float dt) {
        float v = ph < 0.5f ? 1.0f : -1.0f;
        v += blep(ph, dt);
        float p2 = ph + 0.5f;
        if (p2 >= 1.0f) p2 -= 1.0f;
        v -= blep(p2, dt);
        adv(dt);
        return v;
    }
    float sine(float dt) {
        float v = fsin(ph);
        adv(dt);
        return v;
    }
};

// ------------------------------------------------------------ style config

struct Sends {
    float dly = 0, rev = 0;
};

struct KickP {
    float f0 = 160, f1 = 48, pdec = 0.035f, adec = 0.22f, drive = 1.5f, click = 0.25f, level = 0.9f;
};
struct BassP {
    float cutoff = 300, envAmt = 1500, envDec = 0.08f, q = 1.5f, sub = 0.5f;
    float detune = 0, drive = 1.5f, level = 0.35f, lfo = 0;
    bool square = false;
};
struct PadP {
    int voices = 3;
    float detuneCents = 12, cutoff = 2200, q = 0.8f, attack = 0.3f, release = 0.8f, level = 0.09f;
};
struct PluckP {
    float cutoff = 400, envAmt = 5000, envDec = 0.07f, ampDec = 0.22f, q = 1.5f, level = 0.1f, sqMix = 0.3f;
    Sends send{0.35f, 0.25f};
};

// Distorted, filter-modulated bass used for bass house, jump-up and neuro.
struct GrowlP {
    float lo = 150, hi = 2500, q = 3.0f, drive = 3.0f, detune = 1.008f;
    float fm = 0.0f, fmRatio = 2.0f, formant = 0.5f, sub = 0.8f, level = 0.3f;
    bool perNote = false;  // true: one filter sweep per note; false: tempo-synced LFO
};

enum class DrumPat { FourFloor, Breakbeat, BoomBap, TechHouse, TwoStep, DnbRoll, Jungle };
enum class BassPat { Offbeat, Octave, Rolling, Reese, Syncop, Sub, Acid, TechRoll, Disco, Growl, Garage, LiquidSub, Neuro,
                     Wobble, JungleSub };
enum class ChordInstr { None, Pad, Stabs, Rhodes, Organ };
enum class LeadInstr { Saw, Square, Rhodes };

struct StyleCfg {
    DrumPat drums = DrumPat::FourFloor;
    KickP kick;
    float hatDecay = 1, hatLevel = 1, clapLevel = 1, snareRev = 0.18f;
    float swing8 = 0, swing16 = 0;
    bool roll = true;
    float riserLevel = 1;

    BassPat bass = BassPat::Offbeat;
    BassP bassP;

    ChordInstr chordsDrop = ChordInstr::Pad, chordsBreak = ChordInstr::Pad;
    PadP pad;
    PluckP stab;
    int chordCenter = 7;  // semitones above root for chord voicing centre

    bool arp = false;
    float arpStep = 0.25f;
    std::vector<int> arpPattern = {0, 1, 2, 3};
    PluckP arpP;

    bool leadDrop1 = false, leadDrop2 = false, leadBreak = false;
    LeadInstr lead = LeadInstr::Saw;

    std::vector<int> prog = {0, 5, 2, 6};
    int chordBars = 2;
    bool sevenths = false;

    float scDepth = 0.6f, scRelease = 0.15f;
    float dlyBeats = 0.75f, dlyFb = 0.45f, dlyReturn = 0.6f, revReturn = 0.8f, revRoom = 0.84f;
    bool lofi = false;

    int build1Bars = 8, dropBars = 16, breakBars = 8, build2Bars = 8;

    // Extensions used by the newer styles; defaults keep the originals unchanged.
    std::vector<int> stabStepsA, stabStepsB;  // stab steps on even/odd bars (empty = legacy)
    float stabGate = 0.3f;
    bool padUnderDrop = false;  // layer pads under the drop chords
    bool perc = false;          // tech-house style rims and bongos
    float ghostDensity = 0.5f;  // DnB ghost-snare probability
    bool ride = false;          // DnB ride cymbal on the beat
    float snarePitch = 1.2f;
    GrowlP growl;
};

StyleCfg makeStyle(SongStyle s) {
    StyleCfg c;
    switch (s) {
    case SongStyle::SynthHouse:
        c.bass = BassPat::Octave;
        c.bassP = {500, 2500, 0.1f, 2.5f, 0.3f, 0, 1.5f, 0.3f, 0, false};
        c.pad.cutoff = 2600;
        c.leadDrop1 = c.leadDrop2 = c.leadBreak = true;
        c.lead = LeadInstr::Square;
        c.prog = {0, 5, 2, 6};
        c.chordBars = 1;
        c.scDepth = 0.55f;
        break;
    case SongStyle::Techno:
        c.kick = {170, 45, 0.03f, 0.28f, 2.5f, 0.3f, 0.95f};
        c.hatLevel = 1.1f;
        c.bass = BassPat::Rolling;
        c.bassP = {200, 900, 0.05f, 2.0f, 0.6f, 0, 2.0f, 0.36f, 0, false};
        c.chordsDrop = ChordInstr::Stabs;
        c.chordsBreak = ChordInstr::Pad;
        c.pad.cutoff = 1200;
        c.pad.attack = 1.0f;
        c.stab = {300, 1800, 0.08f, 0.3f, 3.0f, 0.11f, 0.5f, {0.6f, 0.4f}};
        c.prog = {0, 0, 0, 5};
        c.scDepth = 0.5f;
        c.dlyFb = 0.55f;
        c.chordCenter = 3;
        break;
    case SongStyle::DeepHouse:
        c.kick = {150, 50, 0.035f, 0.2f, 1.2f, 0.2f, 0.8f};
        c.swing16 = 0.12f;
        c.bass = BassPat::Syncop;
        c.bassP = {350, 600, 0.15f, 1.0f, 0.8f, 0, 1.3f, 0.4f, 0, false};
        c.chordsDrop = ChordInstr::Stabs;
        c.chordsBreak = ChordInstr::Pad;
        c.pad.cutoff = 1400;
        c.pad.attack = 0.5f;
        c.stab = {500, 3000, 0.12f, 0.25f, 1.2f, 0.09f, 0.5f, {0.45f, 0.3f}};
        c.prog = {0, 3, 5, 4};
        c.sevenths = true;
        c.scDepth = 0.45f;
        c.chordCenter = 3;
        break;
    case SongStyle::DrumAndBass:
        c.drums = DrumPat::Breakbeat;
        c.kick = {200, 55, 0.025f, 0.16f, 2.0f, 0.3f, 0.9f};
        c.bass = BassPat::Reese;
        c.bassP = {600, 300, 0.3f, 2.0f, 0.7f, 1.012f, 2.5f, 0.3f, 0.6f, false};
        c.pad.voices = 4;
        c.pad.cutoff = 1800;
        c.pad.attack = 0.6f;
        c.pad.release = 1.2f;
        c.arp = true;
        c.arpStep = 0.5f;
        c.arpPattern = {0, 2, 1, 3, 2, 1};
        c.arpP.ampDec = 0.3f;
        c.arpP.level = 0.09f;
        c.prog = {0, 5, 3, 6};
        c.scDepth = 0.35f;
        c.scRelease = 0.1f;
        c.dlyBeats = 1.5f;
        break;
    case SongStyle::Trance:
        c.kick = {180, 50, 0.03f, 0.2f, 2.0f, 0.25f, 0.9f};
        c.bass = BassPat::Rolling;
        c.bassP = {400, 2200, 0.06f, 2.0f, 0.4f, 0, 1.6f, 0.3f, 0, false};
        c.pad = {7, 22, 5000, 0.7f, 0.05f, 0.5f, 0.06f};
        c.arp = true;
        c.arpPattern = {0, 2, 1, 2, 3, 2, 1, 2, 0, 2, 1, 2, 3, 1, 2, 1};
        c.arpP = {800, 6000, 0.09f, 0.18f, 1.5f, 0.1f, 0.3f, {0.35f, 0.25f}};
        c.leadBreak = c.leadDrop2 = true;
        c.prog = {0, 5, 2, 6};
        c.scDepth = 0.65f;
        break;
    case SongStyle::LoFi:
        c.drums = DrumPat::BoomBap;
        c.kick = {120, 50, 0.04f, 0.2f, 1.6f, 0.1f, 1.0f};
        c.swing8 = 0.3f;
        c.hatDecay = 1.3f;
        c.hatLevel = 1.1f;
        c.snareRev = 0.3f;
        c.roll = false;
        c.riserLevel = 0.4f;
        c.bass = BassPat::Sub;
        c.bassP = {250, 200, 0.2f, 0.8f, 1.0f, 0, 1.0f, 0.4f, 0, false};
        c.chordsDrop = c.chordsBreak = ChordInstr::Rhodes;
        c.leadDrop1 = c.leadDrop2 = true;
        c.lead = LeadInstr::Rhodes;
        c.prog = {3, 6, 2, 5};
        c.chordBars = 1;
        c.sevenths = true;
        c.scDepth = 0.25f;
        c.chordCenter = 2;
        c.lofi = true;
        c.build1Bars = 4;
        c.dropBars = 8;
        c.build2Bars = 4;
        break;
    case SongStyle::AcidTechno:
        c.kick = {170, 45, 0.03f, 0.26f, 2.5f, 0.3f, 0.95f};
        c.bass = BassPat::Acid;
        c.chordsDrop = ChordInstr::None;
        c.chordsBreak = ChordInstr::Pad;
        c.pad.cutoff = 1600;
        c.pad.attack = 0.8f;
        c.prog = {0, 0, 5, 5};
        c.scDepth = 0.5f;
        break;
    case SongStyle::ProgHouse:
        c.bass = BassPat::Offbeat;
        c.bassP = {380, 2000, 0.09f, 2.0f, 0.5f, 0, 1.5f, 0.34f, 0, false};
        c.pad.voices = 5;
        c.pad.cutoff = 3000;
        c.pad.detuneCents = 15;
        c.arp = true;
        c.arpPattern = {0, 1, 2, 1, 3, 1, 2, 1};
        c.arpP.envDec = 0.1f;
        c.arpP.ampDec = 0.3f;
        c.leadBreak = c.leadDrop2 = true;
        c.prog = {0, 2, 6, 5};
        c.scDepth = 0.6f;
        break;
    case SongStyle::TechHouse:
        c.drums = DrumPat::TechHouse;
        c.kick = {165, 46, 0.03f, 0.24f, 2.2f, 0.28f, 0.95f};
        c.swing16 = 0.1f;
        c.perc = true;
        c.hatLevel = 0.9f;
        c.bass = BassPat::TechRoll;
        c.bassP = {170, 800, 0.06f, 2.5f, 0.75f, 0, 2.4f, 0.4f, 0, false};
        c.chordsDrop = ChordInstr::Stabs;
        c.chordsBreak = ChordInstr::Pad;
        c.stabStepsA = {6};
        c.stabStepsB = {3, 14};
        c.stab = {350, 2200, 0.07f, 0.28f, 2.5f, 0.1f, 0.4f, {0.55f, 0.35f}};
        c.pad.cutoff = 1300;
        c.pad.attack = 0.9f;
        c.prog = {0, 0, 5, 3};
        c.chordCenter = 3;
        c.scDepth = 0.5f;
        c.dlyBeats = 0.75f;
        c.dlyFb = 0.5f;
        break;
    case SongStyle::DiscoHouse:
        c.kick = {155, 50, 0.035f, 0.22f, 1.6f, 0.22f, 0.9f};
        c.swing16 = 0.08f;
        c.clapLevel = 1.15f;
        c.bass = BassPat::Disco;
        c.bassP = {550, 2200, 0.07f, 2.0f, 0.4f, 0, 1.6f, 0.34f, 0, false};
        c.chordsDrop = ChordInstr::Stabs;
        c.chordsBreak = ChordInstr::Stabs;
        c.stabStepsA = {2, 6, 10, 14};
        c.stabStepsB = {2, 6, 9, 12, 14};
        c.stabGate = 0.2f;
        c.stab = {700, 3500, 0.09f, 0.2f, 1.4f, 0.09f, 0.25f, {0.3f, 0.3f}};
        c.leadDrop2 = c.leadBreak = true;
        c.lead = LeadInstr::Square;
        c.prog = {0, 3, 6, 5};
        c.chordBars = 1;
        c.sevenths = true;
        c.chordCenter = 5;
        c.scDepth = 0.4f;
        break;
    case SongStyle::BassHouse:
        c.kick = {175, 45, 0.028f, 0.26f, 2.6f, 0.32f, 1.0f};
        c.hatLevel = 1.05f;
        c.bass = BassPat::Growl;
        c.growl = {110, 2600, 4.5f, 3.5f, 1.01f, 0.6f, 1.0f, 0.7f, 0.9f, 0.3f, true};
        c.chordsDrop = ChordInstr::None;
        c.chordsBreak = ChordInstr::Pad;
        c.pad.cutoff = 1800;
        c.arp = true;
        c.arpStep = 0.5f;
        c.arpPattern = {0, 2, 1, 3};
        c.arpP.level = 0.07f;
        c.prog = {0, 0, 5, 6};
        c.scDepth = 0.6f;
        c.scRelease = 0.12f;
        break;
    case SongStyle::UkGarage:
        c.drums = DrumPat::TwoStep;
        c.kick = {150, 52, 0.03f, 0.2f, 1.5f, 0.25f, 0.95f};
        c.swing16 = 0.26f;
        c.perc = true;
        c.bass = BassPat::Garage;
        c.bassP = {170, 400, 0.12f, 1.0f, 1.0f, 0, 1.8f, 0.42f, 0, false};
        c.chordsDrop = ChordInstr::Organ;
        c.chordsBreak = ChordInstr::Pad;
        c.stabStepsA = {0, 6, 11};
        c.stabStepsB = {3, 8, 14};
        c.stabGate = 0.35f;
        c.pad.cutoff = 1600;
        c.pad.attack = 0.6f;
        c.prog = {0, 5, 3, 4};
        c.sevenths = true;
        c.chordCenter = 5;
        c.scDepth = 0.35f;
        break;
    case SongStyle::LiquidDnb:
        c.drums = DrumPat::DnbRoll;
        c.kick = {190, 55, 0.025f, 0.17f, 1.6f, 0.25f, 0.85f};
        c.ghostDensity = 0.55f;
        c.ride = true;
        c.snareRev = 0.3f;
        c.bass = BassPat::LiquidSub;
        c.bassP = {160, 250, 0.2f, 0.8f, 1.0f, 0, 1.3f, 0.45f, 0, false};
        c.chordsDrop = ChordInstr::Rhodes;
        c.chordsBreak = ChordInstr::Pad;
        c.padUnderDrop = true;
        c.pad = {6, 18, 2400, 0.8f, 0.8f, 1.5f, 0.075f};
        c.leadBreak = c.leadDrop2 = true;
        c.lead = LeadInstr::Rhodes;
        c.prog = {0, 5, 2, 4};
        c.sevenths = true;
        c.chordCenter = 3;
        c.scDepth = 0.25f;
        c.scRelease = 0.1f;
        c.dlyBeats = 1.5f;
        c.revRoom = 0.88f;
        c.dropBars = 32;
        c.breakBars = 16;
        break;
    case SongStyle::Neurofunk:
        c.drums = DrumPat::DnbRoll;
        c.kick = {210, 52, 0.022f, 0.15f, 2.6f, 0.35f, 0.95f};
        c.ghostDensity = 0.35f;
        c.snarePitch = 1.35f;
        c.bass = BassPat::Neuro;
        c.growl = {90, 3200, 5.0f, 4.0f, 1.015f, 1.1f, 1.5f, 0.9f, 0.85f, 0.28f, false};
        c.chordsDrop = ChordInstr::None;
        c.chordsBreak = ChordInstr::Pad;
        c.pad = {5, 25, 900, 1.2f, 1.5f, 1.5f, 0.08f};
        c.prog = {0, 0, 5, 6};
        c.chordCenter = 3;
        c.scDepth = 0.3f;
        c.scRelease = 0.08f;
        c.dlyBeats = 0.75f;
        c.dropBars = 32;
        c.breakBars = 16;
        break;
    case SongStyle::JumpUp:
        c.drums = DrumPat::DnbRoll;
        c.kick = {200, 55, 0.025f, 0.18f, 2.2f, 0.3f, 1.0f};
        c.ghostDensity = 0.15f;
        c.bass = BassPat::Wobble;
        c.growl = {120, 2200, 3.5f, 3.0f, 1.006f, 0.3f, 1.0f, 0.4f, 1.0f, 0.3f, false};
        c.chordsDrop = ChordInstr::None;
        c.chordsBreak = ChordInstr::Pad;
        c.pad.cutoff = 2000;
        c.leadBreak = true;
        c.lead = LeadInstr::Square;
        c.prog = {0, 0, 3, 4};
        c.scDepth = 0.35f;
        c.scRelease = 0.1f;
        c.dropBars = 32;
        c.breakBars = 16;
        break;
    case SongStyle::Jungle:
        c.drums = DrumPat::Jungle;
        c.kick = {160, 48, 0.035f, 0.22f, 1.8f, 0.2f, 0.95f};
        c.snarePitch = 1.3f;
        c.snareRev = 0.25f;
        c.bass = BassPat::JungleSub;
        c.bassP = {120, 150, 0.2f, 0.8f, 1.0f, 0, 2.2f, 0.48f, 0, false};
        c.chordsDrop = ChordInstr::Stabs;
        c.chordsBreak = ChordInstr::Pad;
        c.stabStepsA = {0};
        c.stabStepsB = {};
        c.stabGate = 0.5f;
        c.stab = {900, 5000, 0.12f, 0.35f, 2.0f, 0.12f, 0.6f, {0.4f, 0.45f}};
        c.pad = {6, 20, 2000, 0.8f, 1.0f, 1.4f, 0.08f};
        c.leadBreak = true;
        c.lead = LeadInstr::Rhodes;
        c.prog = {0, 5, 6, 4};
        c.chordBars = 2;
        c.chordCenter = 5;
        c.scDepth = 0.25f;
        c.dropBars = 32;
        c.breakBars = 16;
        break;
    }
    return c;
}

// ---------------------------------------------------------------- context

struct Bus {
    std::vector<float> l, r;
    void init(size_t n) {
        l.assign(n, 0.0f);
        r.assign(n, 0.0f);
    }
};

struct AcidStep {
    int note = 0;
    bool gate = false, accent = false, slide = false;
};

struct MNote {
    int step, len, deg;
};

struct Ctx {
    const SongSpec& spec;
    StyleCfg st;
    double fpb = 0;  // frames per beat
    size_t total = 0;
    Bus drums, bass, music, fx, dly, rev;
    std::vector<size_t> kicks;
    Rng rng;
    std::vector<AcidStep> acid[2];
    std::vector<MNote> motif;
    int jungleVar = 0;

    Ctx(const SongSpec& s) : spec(s), st(makeStyle(s.style)), rng(s.seed) {}

    size_t at(double beat) const { return beat <= 0.0 ? 0 : size_t(std::llround(beat * fpb)); }

    void emit(Bus& b, size_t i, float l, float r, Sends s) {
        b.l[i] += l;
        b.r[i] += r;
        if (s.dly > 0) {
            dly.l[i] += l * s.dly;
            dly.r[i] += r * s.dly;
        }
        if (s.rev > 0) {
            rev.l[i] += l * s.rev;
            rev.r[i] += r * s.rev;
        }
    }
};

// ------------------------------------------------------------ instruments

void kick(Ctx& c, double beat, float vel) {
    const KickP& k = c.st.kick;
    size_t s0 = c.at(beat);
    if (s0 >= c.total) return;
    c.kicks.push_back(s0);
    size_t n = std::min(size_t((0.012f + k.adec * 6.0f) * kSr), c.total - s0);
    Rng r(c.rng.next());
    float ph = 0, pe = 1, ae = 1, ce = 1;
    const float pc = decayCoef(k.pdec), ac = decayCoef(k.adec), cc = decayCoef(0.0015f);
    const size_t hold = size_t(0.012f * kSr);
    const float norm = 1.0f / ftanh(k.drive);
    for (size_t i = 0; i < n; ++i) {
        float f = k.f1 + (k.f0 - k.f1) * pe;
        pe *= pc;
        ph += f * kInvSr;
        if (ph >= 1.0f) ph -= 1.0f;
        float x = fsin(ph) * ae + r.bi() * ce * k.click;
        ce *= cc;
        if (i >= hold) ae *= ac;
        float y = ftanh(x * k.drive) * norm * vel * k.level;
        c.drums.l[s0 + i] += y;
        c.drums.r[s0 + i] += y;
    }
}

void hat(Ctx& c, double beat, float vel, bool open, float pan) {
    size_t s0 = c.at(beat);
    if (s0 >= c.total) return;
    const float dec = (open ? 0.22f : 0.035f) * c.st.hatDecay;
    size_t n = std::min(size_t(dec * 6.5f * kSr), c.total - s0);
    static constexpr float fr[6] = {205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f};
    float ph[6] = {0, 0, 0, 0, 0, 0}, dt[6];
    for (int j = 0; j < 6; ++j) dt[j] = fr[j] * 1.7f * kInvSr;
    Rng r(c.rng.next());
    Svf h1, h2;
    h1.set(7000, 0.7f);
    h2.set(8500, 0.9f);
    float e = 1.0f;
    const float ec = decayCoef(dec);
    float gl, gr;
    panGains(pan, gl, gr);
    const float amp = vel * 0.3f * c.st.hatLevel;
    const Sends s{0.0f, open ? 0.1f : 0.04f};
    for (size_t i = 0; i < n; ++i) {
        float m = 0;
        for (int j = 0; j < 6; ++j) {
            m += ph[j] < 0.5f ? 1.0f : -1.0f;
            ph[j] += dt[j];
            if (ph[j] >= 1.0f) ph[j] -= 1.0f;
        }
        float x = m * (0.6f / 6.0f) + r.bi() * 0.6f;
        float y = h2.hp(h1.hp(x)) * e * amp;
        e *= ec;
        c.emit(c.drums, s0 + i, y * gl, y * gr, s);
    }
}

void clap(Ctx& c, double beat, float vel) {
    size_t s0 = c.at(beat);
    if (s0 >= c.total) return;
    size_t n = std::min(size_t(0.6f * kSr), c.total - s0);
    Rng r(c.rng.next());
    Svf bl, br;
    bl.set(1150, 1.6f);
    br.set(1250, 1.6f);
    const float amp = vel * 0.9f * c.st.clapLevel;
    float te = 0.7f;
    const float tc = decayCoef(0.12f);
    for (size_t i = 0; i < n; ++i) {
        float t = float(i) * kInvSr;
        float env;
        if (t < 0.03f) {
            env = 0;
            for (int k = 0; k < 4; ++k) {
                float tk = float(k) * 0.0095f;
                if (t >= tk) env += std::exp(-(t - tk) / 0.0035f) * (k == 3 ? 1.0f : 0.8f);
            }
        } else {
            env = te;
            te *= tc;
        }
        float ns = r.bi();
        float nl = ns * 0.7f + r.bi() * 0.3f, nr = ns * 0.7f + r.bi() * 0.3f;
        c.emit(c.drums, s0 + i, bl.bp(nl) * env * amp, br.bp(nr) * env * amp, {0.05f, 0.25f});
    }
}

void snare(Ctx& c, double beat, float vel, float pitch) {
    size_t s0 = c.at(beat);
    if (s0 >= c.total) return;
    size_t n = std::min(size_t(0.4f * kSr), c.total - s0);
    Rng r(c.rng.next());
    Osc o1, o2;
    Svf hl, hr;
    hl.set(1500, 0.7f);
    hr.set(1600, 0.7f);
    float te = 1, ne = 1;
    const float tc = decayCoef(0.055f), nc = decayCoef(0.14f);
    const float d1 = 185.0f * pitch * kInvSr, d2 = 330.0f * pitch * kInvSr;
    const float amp = vel * 0.6f;
    for (size_t i = 0; i < n; ++i) {
        float tone = (o1.sine(d1) + 0.6f * o2.sine(d2)) * 0.5f * te;
        float ns = r.bi();
        float nl = hl.hp(ns * 0.8f + r.bi() * 0.2f), nr = hr.hp(ns * 0.8f + r.bi() * 0.2f);
        te *= tc;
        float yl = (tone + 0.8f * nl * ne) * amp, yr = (tone + 0.8f * nr * ne) * amp;
        ne *= nc;
        c.emit(c.drums, s0 + i, yl, yr, {0.03f, c.st.snareRev});
    }
}

void crash(Ctx& c, double beat, float vel) {
    size_t s0 = c.at(beat);
    if (s0 >= c.total) return;
    size_t n = std::min(size_t(4.0f * kSr), c.total - s0);
    Rng r(c.rng.next());
    Svf l1, l2, r1, r2;
    l1.set(5000, 0.7f);
    l2.set(5500, 0.7f);
    r1.set(5000, 0.7f);
    r2.set(5500, 0.7f);
    float e = 1;
    const float ec = decayCoef(0.9f);
    const float amp = vel * 0.28f;
    for (size_t i = 0; i < n; ++i) {
        float a = std::min(1.0f, float(i) / 40.0f) * e * amp;
        e *= ec;
        c.emit(c.fx, s0 + i, l2.hp(l1.hp(r.bi())) * a, r2.hp(r1.hp(r.bi())) * a, {0.0f, 0.3f});
    }
}

void riser(Ctx& c, double beat, double beats) {
    size_t s0 = c.at(beat);
    if (s0 >= c.total) return;
    size_t n = std::min(size_t(beats * c.fpb), c.total - s0);
    Rng r(c.rng.next());
    Svf fl, fr;
    Osc tone;
    const float lvl = c.st.riserLevel;
    for (size_t i = 0; i < n; ++i) {
        float p = float(i) / float(n);
        if ((i & 31) == 0) {
            float fc = 250.0f * std::pow(40.0f, p);
            fl.set(fc, 2.5f);
            fr.set(fc * 1.1f, 2.5f);
        }
        float amp = p * p * 0.35f * lvl;
        float t = tone.sine(200.0f * std::pow(8.0f, p) * kInvSr) * p * p * 0.05f * lvl;
        c.emit(c.fx, s0 + i, fl.bp(r.bi()) * amp + t, fr.bp(r.bi()) * amp + t, {0.0f, 0.35f});
    }
}

void impact(Ctx& c, double beat) {
    size_t s0 = c.at(beat);
    if (s0 >= c.total) return;
    size_t n = std::min(size_t(2.5f * kSr), c.total - s0);
    Rng r(c.rng.next());
    Osc o;
    Svf lp;
    lp.set(400, 0.7f);
    float pe = 1, ae = 1, ne = 1;
    const float pc = decayCoef(0.4f), ac = decayCoef(0.5f), nc = decayCoef(0.08f);
    for (size_t i = 0; i < n; ++i) {
        float f = 35.0f + 35.0f * pe;
        pe *= pc;
        float y = (o.sine(f * kInvSr) * ae + lp.lp(r.bi()) * ne * 0.6f) * 0.32f;
        ae *= ac;
        ne *= nc;
        c.emit(c.fx, s0 + i, y, y, {0.0f, 0.2f});
    }
}

void bassNote(Ctx& c, double beat, double lenBeats, int midi, float vel) {
    const BassP& p = c.st.bassP;
    size_t s0 = c.at(beat);
    if (s0 >= c.total) return;
    const size_t len = size_t(lenBeats * c.fpb), rel = size_t(0.02f * kSr), att = size_t(0.003f * kSr);
    size_t n = std::min(len + rel, c.total - s0);
    Rng r(c.rng.next());
    const float dt = midiHz(float(midi)) * kInvSr;
    Osc o1, o2, sub;
    o1.ph = r.uni();
    o2.ph = r.uni();
    Svf fl, fr;
    float env = 1;
    const float envc = decayCoef(p.envDec);
    const float lfo = p.lfo > 0 ? 1.0f + p.lfo * std::sin(2.0f * kPi * float(beat) / 8.0f) : 1.0f;
    const bool wide = p.detune > 0;
    const float amp = vel * p.level;
    for (size_t i = 0; i < n; ++i) {
        if ((i & 15) == 0) {
            float fc = (p.cutoff + p.envAmt * env * vel) * lfo;
            fl.set(fc, p.q);
            if (wide) fr.set(fc, p.q);
        }
        env *= envc;
        float a;
        if (i < att) a = float(i) / float(att);
        else if (i < len) a = 1.0f;
        else a = 1.0f - float(i - len) / float(rel);
        float sb = sub.sine(dt) * p.sub;
        float yl, yr;
        if (wide) {
            float s1 = o1.saw(dt * p.detune), s2 = o2.saw(dt / p.detune);
            yl = ftanh((fl.lp(s1 + 0.5f * s2) * 0.7f + sb) * p.drive);
            yr = ftanh((fr.lp(s2 + 0.5f * s1) * 0.7f + sb) * p.drive);
        } else {
            float s = p.square ? o1.square(dt) : o1.saw(dt);
            yl = yr = ftanh((fl.lp(s) * 0.7f + sb) * p.drive);
        }
        c.emit(c.bass, s0 + i, yl * a * amp, yr * a * amp, {0.0f, 0.0f});
    }
}

// Continuous monophonic 303-style line with accents and slides.
void acidLine(Ctx& c, int startBar, int bars, int rootMidi, float brightness) {
    const double stepFrames = c.fpb / 4.0;
    const size_t s0 = c.at(startBar * 4.0);
    const size_t s1 = std::min(c.at((startBar + bars) * 4.0), c.total);
    if (s0 >= s1) return;
    auto stepAt = [&](int step) -> const AcidStep& {
        int bar = step / 16;
        return c.acid[(bar % 4 == 3) ? 1 : 0][size_t(step % 16)];
    };
    Osc o;
    Svf f;
    float freq = midiHz(float(rootMidi)), target = freq, env = 0, amp = 0, ampTarget = 0, accent = 0;
    int curStep = -1;
    size_t gateOff = 0;
    const float glideC = 1.0f - decayCoef(0.018f), ampC = 1.0f - decayCoef(0.002f);
    const float envc = decayCoef(0.12f), envcAcc = decayCoef(0.07f);
    for (size_t i = s0; i < s1; ++i) {
        int step = int(double(i - s0) / stepFrames);
        if (step != curStep) {
            curStep = step;
            const AcidStep& st = stepAt(step);
            bool slideIn = false;
            if (step > 0) {
                const AcidStep& pv = stepAt(step - 1);
                slideIn = pv.slide && pv.gate && st.gate;
            }
            if (st.gate) {
                target = midiHz(float(rootMidi + st.note));
                if (!slideIn) {
                    freq = target;
                    env = 1.0f;
                    accent = st.accent ? 1.0f : 0.0f;
                }
                ampTarget = st.accent ? 1.25f : 1.0f;
                gateOff = st.slide ? s1 : i + size_t(stepFrames * 0.55);
            } else {
                gateOff = i;
            }
        }
        if (i >= gateOff) ampTarget = 0;
        freq += (target - freq) * glideC;
        amp += (ampTarget - amp) * ampC;
        if ((i & 7) == 0) {
            float barPos = float(double(i) / (c.fpb * 4.0));
            float sweep = 0.5f - 0.5f * std::cos(2.0f * kPi * barPos / 16.0f);
            float fc = (180.0f + 900.0f * sweep + (1200.0f + 2400.0f * accent) * env) * brightness;
            f.set(fc, 6.5f);
        }
        env *= accent > 0 ? envcAcc : envc;
        float y = ftanh(f.lp(o.saw(freq * kInvSr)) * 2.5f) * amp * 0.3f;
        c.emit(c.bass, i, y, y, {0.12f, 0.05f});
    }
}

void padNote(Ctx& c, double beat, double lenBeats, int midi, float cutMul) {
    const PadP& p = c.st.pad;
    size_t s0 = c.at(beat);
    if (s0 >= c.total) return;
    const size_t len = size_t(lenBeats * c.fpb), rel = size_t(p.release * kSr);
    const size_t att = std::max<size_t>(1, size_t(p.attack * kSr));
    size_t n = std::min(len + rel, c.total - s0);
    const int V = std::clamp(p.voices, 1, 8);
    Osc o[8];
    float dts[8], gl[8], gr[8];
    Rng r(c.rng.next());
    const float f = midiHz(float(midi));
    for (int v = 0; v < V; ++v) {
        float spread = V > 1 ? (2.0f * float(v) / float(V - 1) - 1.0f) : 0.0f;
        dts[v] = f * std::pow(2.0f, spread * p.detuneCents / 1200.0f) * kInvSr;
        o[v].ph = r.uni();
        panGains(spread * 0.8f, gl[v], gr[v]);
    }
    Svf fl, fr;
    const float fc = p.cutoff * cutMul;
    fl.set(fc, p.q);
    fr.set(fc, p.q);
    const float norm = p.level / std::sqrt(float(V));
    float aLen = 1.0f;
    for (size_t i = 0; i < n; ++i) {
        float a;
        if (i < len) {
            a = i < att ? float(i) / float(att) : 1.0f;
            aLen = a;
        } else {
            a = aLen * (1.0f - float(i - len) / float(rel));
        }
        float sl = 0, sr = 0;
        for (int v = 0; v < V; ++v) {
            float s = o[v].saw(dts[v]);
            sl += s * gl[v];
            sr += s * gr[v];
        }
        c.emit(c.music, s0 + i, fl.lp(sl) * a * norm, fr.lp(sr) * a * norm, {0.08f, 0.4f});
    }
}

void pluck(Ctx& c, double beat, double gateBeats, int midi, float vel, float pan, const PluckP& p, float cutMul) {
    size_t s0 = c.at(beat);
    if (s0 >= c.total) return;
    const size_t gate = size_t(gateBeats * c.fpb);
    size_t n = std::min({size_t(p.ampDec * 5.0f * kSr), gate + size_t(0.2f * kSr), c.total - s0});
    Rng r(c.rng.next());
    const float dt = midiHz(float(midi)) * kInvSr;
    Osc o1, o2;
    o1.ph = r.uni();
    o2.ph = r.uni();
    Svf f;
    float env = 1, amp = vel * p.level;
    const float envc = decayCoef(p.envDec), ampc = decayCoef(p.ampDec), relc = decayCoef(0.03f);
    float gl, gr;
    panGains(pan, gl, gr);
    for (size_t i = 0; i < n; ++i) {
        if ((i & 15) == 0) f.set((p.cutoff + p.envAmt * env) * cutMul, p.q);
        env *= envc;
        amp *= i < gate ? ampc : relc;
        float x = (1.0f - p.sqMix) * o1.saw(dt) + p.sqMix * o2.square(dt * 1.003f);
        float a = std::min(1.0f, float(i) / 30.0f);
        float y = f.lp(x) * amp * a;
        c.emit(c.music, s0 + i, y * gl, y * gr, p.send);
    }
}

// Two-operator FM electric piano.
void rhodes(Ctx& c, double beat, double lenBeats, int midi, float vel, float pan, Sends s) {
    size_t s0 = c.at(beat);
    if (s0 >= c.total) return;
    const size_t len = size_t(lenBeats * c.fpb);
    size_t n = std::min(len + size_t(0.5f * kSr), c.total - s0);
    const float f = midiHz(float(midi));
    const float dt = f * kInvSr, bdt = std::min(0.45f, f * 7.0f * kInvSr), tdt = 4.0f * kInvSr;
    float cph = 0, mph = 0, bph = 0, tph = 0;
    float ie = 1, ae = 1, be = 1;
    const float ic = decayCoef(0.5f), ac = decayCoef(1.6f), bc = decayCoef(0.15f), rc = decayCoef(0.08f);
    const float amp = vel * 0.2f;
    for (size_t i = 0; i < n; ++i) {
        float index = 0.25f + 1.8f * ie;
        ie *= ic;
        float mod = fsin(mph) * index * (1.0f / (2.0f * kPi));
        float y = fsin(wrap01(cph + mod)) * ae + 0.1f * fsin(bph) * be;
        ae *= i < len ? ac : rc;
        be *= bc;
        cph += dt; if (cph >= 1.0f) cph -= 1.0f;
        mph += dt; if (mph >= 1.0f) mph -= 1.0f;
        bph += bdt; if (bph >= 1.0f) bph -= 1.0f;
        tph += tdt; if (tph >= 1.0f) tph -= 1.0f;
        float gl, gr;
        panGains(std::clamp(pan + 0.25f * fsin(tph), -1.0f, 1.0f), gl, gr);
        float a = std::min(1.0f, float(i) / 60.0f) * amp;
        c.emit(c.music, s0 + i, y * a * gl, y * a * gr, s);
    }
}

// Tuned percussion: rims, bongos and congas (pitch-dropping sine plus a click).
void perc(Ctx& c, double beat, float vel, float hz, float dec, float pan, float noise) {
    size_t s0 = c.at(beat);
    if (s0 >= c.total) return;
    size_t n = std::min(size_t(dec * 6.0f * kSr), c.total - s0);
    Rng r(c.rng.next());
    Svf bp;
    bp.set(hz * 4.0f, 2.0f);
    float ph = 0, e = 1, ne = 1, pe = 1;
    const float ec = decayCoef(dec), nc = decayCoef(0.004f), pc = decayCoef(0.006f);
    float gl, gr;
    panGains(pan, gl, gr);
    const float amp = vel * 0.32f;
    for (size_t i = 0; i < n; ++i) {
        ph += hz * (1.0f + 0.6f * pe) * kInvSr;
        if (ph >= 1.0f) ph -= 1.0f;
        pe *= pc;
        float y = (fsin(ph) * e + bp.bp(r.bi()) * ne * noise) * amp;
        e *= ec;
        ne *= nc;
        c.emit(c.drums, s0 + i, y * gl, y * gr, {0.12f, 0.12f});
    }
}

// Additive "M1" style organ stab (drawbar sines with a percussive 3rd harmonic).
void organ(Ctx& c, double beat, double lenBeats, int midi, float vel, float pan) {
    size_t s0 = c.at(beat);
    if (s0 >= c.total) return;
    const size_t len = size_t(lenBeats * c.fpb), rel = size_t(0.07f * kSr);
    size_t n = std::min(len + rel, c.total - s0);
    static constexpr float harm[6] = {1, 2, 3, 4, 6, 8};
    static constexpr float amps[6] = {1.0f, 0.55f, 0.0f, 0.4f, 0.2f, 0.12f};
    const float f = midiHz(float(midi));
    float ph[6] = {0, 0, 0, 0, 0, 0}, dt[6];
    for (int h = 0; h < 6; ++h) dt[h] = std::min(0.45f, f * harm[h] * kInvSr);
    float pe = 1, e = 1, aLen = 0;
    const float pc = decayCoef(0.06f), ec = decayCoef(0.25f);
    float gl, gr;
    panGains(pan, gl, gr);
    const float amp = vel * 0.075f;
    for (size_t i = 0; i < n; ++i) {
        float x = 0;
        for (int h = 0; h < 6; ++h) {
            float a = h == 2 ? 0.7f * pe : amps[h];
            x += fsin(ph[h]) * a;
            ph[h] += dt[h];
            if (ph[h] >= 1.0f) ph[h] -= 1.0f;
        }
        pe *= pc;
        float a;
        if (i < len) {
            a = std::min(1.0f, float(i) / 60.0f) * (0.55f + 0.45f * e);
            aLen = a;
        } else {
            a = aLen * (1.0f - float(i - len) / float(rel));
        }
        e *= ec;
        float y = ftanh(x * 0.8f) * a * amp;
        c.emit(c.music, s0 + i, y * gl, y * gr, {0.2f, 0.25f});
    }
}

// Growl / wobble bass: detuned FM saws through a modulated low-pass plus a
// formant band-pass, then driven hard. The sine sub stays clean underneath.
void growlNote(Ctx& c, double beat, double lenBeats, int midi, float vel, float lfoBeats) {
    const GrowlP& g = c.st.growl;
    size_t s0 = c.at(beat);
    if (s0 >= c.total) return;
    const size_t len = size_t(lenBeats * c.fpb), rel = size_t(0.015f * kSr), att = size_t(0.002f * kSr);
    size_t n = std::min(len + rel, c.total - s0);
    Rng r(c.rng.next());
    const float dt = midiHz(float(midi)) * kInvSr;
    Osc a, b, sub;
    a.ph = r.uni();
    b.ph = r.uni();
    float mph = 0;
    Svf fl, fr, bl, br, pl, pr;
    pl.set(7000, 0.7f);
    pr.set(7000, 0.7f);
    float lfo = 0;
    const float ratio = g.hi / g.lo;
    for (size_t i = 0; i < n; ++i) {
        if ((i & 7) == 0) {
            if (g.perNote) {
                float p = std::min(1.0f, float(i) / float(std::max<size_t>(1, len)));
                lfo = std::sin(kPi * p);
            } else {
                double bpos = beat + double(i) / c.fpb;
                lfo = 0.5f - 0.5f * std::cos(2.0f * kPi * float(std::fmod(bpos / lfoBeats, 1.0)));
            }
            float fc = g.lo * std::pow(ratio, lfo);
            fl.set(fc, g.q);
            fr.set(fc * 1.04f, g.q);
            bl.set(fc * 2.2f, 4.0f);
            br.set(fc * 2.3f, 4.0f);
        }
        float mod = g.fm > 0 ? fsin(mph) * g.fm * (0.3f + 0.7f * lfo) : 0.0f;
        mph += dt * g.fmRatio;
        if (mph >= 1.0f) mph -= 1.0f;
        const float fmMul = std::max(0.2f, 1.0f + 0.5f * mod);
        float x1 = a.saw(dt * g.detune * fmMul);
        float x2 = b.saw(dt / g.detune * fmMul);
        float inL = x1 + 0.6f * x2, inR = x2 + 0.6f * x1;
        float yl = fl.lp(inL) + g.formant * bl.bp(inL);
        float yr = fr.lp(inR) + g.formant * br.bp(inR);
        yl = pl.lp(ftanh(yl * g.drive));
        yr = pr.lp(ftanh(yr * g.drive));
        float sb = sub.sine(dt) * g.sub;
        float e;
        if (i < att) e = float(i) / float(att);
        else if (i < len) e = 1.0f;
        else e = 1.0f - float(i - len) / float(rel);
        float amp = e * vel * g.level;
        c.emit(c.bass, s0 + i, (yl * 0.55f + sb) * amp, (yr * 0.55f + sb) * amp, {0.0f, 0.0f});
    }
}

void leadNote(Ctx& c, double beat, double lenBeats, int midi, float vel) {
    if (c.st.lead == LeadInstr::Rhodes) {
        rhodes(c, beat, lenBeats, midi, vel * 1.1f, 0.1f, {0.3f, 0.35f});
        return;
    }
    size_t s0 = c.at(beat);
    if (s0 >= c.total) return;
    const size_t len = size_t(lenBeats * c.fpb), rel = size_t(0.15f * kSr);
    size_t n = std::min(len + rel, c.total - s0);
    const float f = midiHz(float(midi));
    const bool sq = c.st.lead == LeadInstr::Square;
    Osc a, b;
    b.ph = 0.37f;
    Svf fl, fr;
    float env = 1, vph = 0, aLen = 0;
    const float envc = decayCoef(0.2f);
    const float amp = vel * 0.11f;
    const size_t att = size_t(0.005f * kSr), vibStart = size_t(0.18f * kSr);
    for (size_t i = 0; i < n; ++i) {
        if ((i & 15) == 0) {
            float fc = 1800.0f + 3500.0f * env;
            fl.set(fc, 1.0f);
            fr.set(fc * 1.05f, 1.0f);
        }
        float e;
        if (i < len) {
            e = i < att ? float(i) / float(att) : 0.7f + 0.3f * env;
            aLen = e;
        } else {
            e = aLen * (1.0f - float(i - len) / float(rel));
        }
        env *= envc;
        float depth = i > vibStart ? std::min(1.0f, float(i - vibStart) / (0.3f * kSr)) * 0.012f : 0.0f;
        float vib = 1.0f + depth * fsin(vph);
        vph += 5.5f * kInvSr;
        if (vph >= 1.0f) vph -= 1.0f;
        float dt = f * vib * kInvSr;
        float x1 = sq ? a.square(dt * 1.004f) : a.saw(dt * 1.004f);
        float x2 = sq ? b.square(dt / 1.004f) : b.saw(dt / 1.004f);
        float yl = fl.lp(x1 * 0.8f + x2 * 0.4f) * e * amp;
        float yr = fr.lp(x2 * 0.8f + x1 * 0.4f) * e * amp;
        c.emit(c.music, s0 + i, yl, yr, {0.3f, 0.3f});
    }
}

// ------------------------------------------------------------- harmony

int degMidi(int root, int deg) {
    static const int sc[7] = {0, 2, 3, 5, 7, 8, 10};
    int o = deg >= 0 ? deg / 7 : -((-deg + 6) / 7);
    int d = deg - 7 * o;
    return root + 12 * o + sc[d];
}

std::vector<int> voiceChord(int root, int deg, bool seventh, int center) {
    std::vector<int> v;
    const int cnt = seventh ? 4 : 3;
    for (int k = 0; k < cnt; ++k) {
        int m = degMidi(root, deg + 2 * k);
        while (m < center - 6) m += 12;
        while (m >= center + 6) m -= 12;
        v.push_back(m);
    }
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

int bassMidi(int root, int deg) {
    int base = root - 24;
    int m = degMidi(base, deg);
    if (m > base + 7) m -= 12;
    return m;
}

int chordDegAt(const Ctx& c, int bar) {
    const auto& p = c.st.prog;
    return p[size_t((bar / c.st.chordBars) % int(p.size()))];
}

// Nearest chord tone (in scale degrees) to `d` for the chord on degree `cd`.
int snapToChord(int d, int cd) {
    int best = d, bestDist = 1000;
    for (int oct = -2; oct <= 2; ++oct)
        for (int k = 0; k < 3; ++k) {
            int cand = cd + 2 * k + 7 * oct;
            int dist = std::abs(cand - d);
            if (dist < bestDist) {
                bestDist = dist;
                best = cand;
            }
        }
    return best;
}

void makePatterns(Ctx& c) {
    Rng& r = c.rng;
    // Acid: two 16-step patterns, the second a variation of the first.
    static const int pool[] = {0, 0, 0, 12, 0, 3, 7, 10, 12, 5, 0, -2};
    for (int p = 0; p < 2; ++p) {
        c.acid[p].assign(16, AcidStep{});
        for (int s = 0; s < 16; ++s) {
            AcidStep& st = c.acid[p][size_t(s)];
            if (p == 1 && s < 8) {
                st = c.acid[0][size_t(s)];
                continue;
            }
            st.gate = s == 0 || r.chance(0.78f);
            st.note = s == 0 ? 0 : pool[r.range(12)];
            st.accent = s == 0 || r.chance(0.3f);
            st.slide = r.chance(0.25f);
        }
    }
    // Lead motif over two bars (32 sixteenth steps).
    static const int rhythms[3][10][2] = {
        {{0, 3}, {3, 3}, {6, 2}, {8, 4}, {12, 2}, {14, 2}, {16, 3}, {19, 3}, {22, 2}, {24, 6}},
        {{0, 2}, {2, 2}, {4, 4}, {10, 2}, {12, 4}, {16, 2}, {18, 2}, {20, 4}, {26, 2}, {28, 4}},
        {{0, 6}, {6, 2}, {8, 6}, {14, 2}, {16, 6}, {22, 2}, {24, 6}, {30, 2}, {-1, 0}, {-1, 0}},
    };
    static const int moves[] = {-2, -1, -1, 0, 1, 1, 2};
    const int t = r.range(3);
    int d = 4;
    c.motif.clear();
    for (int k = 0; k < 10; ++k) {
        if (rhythms[t][k][0] < 0) break;
        d = std::clamp(d + moves[r.range(7)], 0, 9);
        c.motif.push_back({rhythms[t][k][0], rhythms[t][k][1], d});
    }
}

// ------------------------------------------------------------- arrangement

enum class Sec { Intro, Build, Drop, Break, Build2, Drop2, Outro };
struct Section {
    Sec kind;
    int start, bars;
};

std::vector<Section> makeSections(const StyleCfg& st) {
    std::vector<Section> v;
    int b = 0;
    auto add = [&](Sec k, int n) {
        v.push_back({k, b, n});
        b += n;
    };
    add(Sec::Intro, 8);
    add(Sec::Build, st.build1Bars);
    add(Sec::Drop, st.dropBars);
    add(Sec::Break, st.breakBars);
    add(Sec::Build2, st.build2Bars);
    add(Sec::Drop2, st.dropBars);
    add(Sec::Outro, 8);
    return v;
}

double beatOf(const StyleCfg& st, int bar, int step) {
    double b = bar * 4.0 + step * 0.25;
    if (st.swing16 > 0 && (step & 1)) b += st.swing16 * 0.25;
    if (st.swing8 > 0 && (step & 3) == 2) b += st.swing8 * 0.5;
    return b;
}

void scheduleDrums(Ctx& c, const Section& s) {
    const StyleCfg& st = c.st;
    const bool drop = s.kind == Sec::Drop || s.kind == Sec::Drop2;
    Rng& r = c.rng;

    // Transition effects.
    if (drop) {
        crash(c, s.start * 4.0, 1.0f);
        impact(c, s.start * 4.0);
    } else if (s.kind == Sec::Break) {
        crash(c, s.start * 4.0, 0.6f);
    } else if (s.kind == Sec::Outro) {
        crash(c, s.start * 4.0, 0.7f);
    }
    if (s.kind == Sec::Build) {
        int rb = std::max(0, s.bars - 4);
        riser(c, (s.start + rb) * 4.0, (s.bars - rb) * 4.0);
    }
    if (s.kind == Sec::Build2) {
        riser(c, s.start * 4.0, s.bars * 4.0);
        if (st.roll) {
            const int stageBars = std::max(1, s.bars / 4);
            const double totalBeats = s.bars * 4.0;
            static const int div[4] = {1, 2, 4, 8};
            for (int k = 0; k < 4; ++k) {
                for (int b = 0; b < stageBars * 4; ++b) {
                    for (int j = 0; j < div[k]; ++j) {
                        double beat = (s.start + k * stageBars) * 4.0 + b + double(j) / div[k];
                        float p = float((beat - s.start * 4.0) / totalBeats);
                        snare(c, beat, 0.25f + 0.7f * p, 1.0f + 0.4f * p);
                    }
                }
            }
        }
    }

    const bool kickOn = s.kind == Sec::Intro || s.kind == Sec::Build || drop || s.kind == Sec::Outro;
    const bool hatsOn = s.kind != Sec::Break && s.kind != Sec::Build2;
    const bool snareOn = s.kind == Sec::Build || drop;
    if (!kickOn && !hatsOn) return;

    for (int rb = 0; rb < s.bars; ++rb) {
        const int bar = s.start + rb;
        const bool second = rb >= s.bars / 2;
        const bool openOn = (s.kind == Sec::Intro && second) || drop || (s.kind == Sec::Outro && !second) ||
                            (s.kind == Sec::Build && second);
        const bool extraOn = drop && (s.kind == Sec::Drop2 || second);
        const bool fill = drop && (rb % 8 == 7);
        const bool odd = (bar & 1) != 0;
        for (int step = 0; step < 16; ++step) {
            const double b = beatOf(st, bar, step);
            switch (st.drums) {
            case DrumPat::FourFloor:
                if (kickOn && step % 4 == 0) kick(c, b, 1.0f);
                if (snareOn && (step == 4 || step == 12)) clap(c, b, 1.0f);
                if (fill && (step == 13 || step == 15)) clap(c, b, step == 13 ? 0.45f : 0.65f);
                if (hatsOn) {
                    if (step % 4 == 2) {
                        if (openOn) hat(c, b, 0.9f, true, 0.15f);
                        else hat(c, b, 0.8f, false, 0.1f);
                    } else if (extraOn || (s.kind == Sec::Intro && second)) {
                        float v = (step & 1) ? 0.35f + 0.15f * r.uni() : 0.2f;
                        hat(c, b, v, false, (step & 1) ? -0.3f : 0.3f);
                    }
                }
                break;
            case DrumPat::Breakbeat:
                if (kickOn && (step == 0 || step == 10)) kick(c, b, 1.0f);
                if (kickOn && drop && odd && step == 7) kick(c, b, 0.6f);
                if (snareOn && (step == 4 || step == 12)) snare(c, b, 1.0f, 1.2f);
                if (drop && ((!odd && step == 7) || (odd && step == 15))) snare(c, b, 0.28f, 1.2f);
                if (hatsOn) {
                    if (step % 2 == 0) hat(c, b, step % 4 == 0 ? 0.5f : 0.35f, false, 0.2f);
                    else if (extraOn) hat(c, b, 0.15f + 0.1f * r.uni(), false, -0.25f);
                    if (openOn && odd && step == 14) hat(c, b, 0.6f, true, -0.1f);
                }
                break;
            case DrumPat::BoomBap:
                if (kickOn && (step == 0 || step == 10 || (odd && step == 7))) kick(c, b, step == 0 ? 1.0f : 0.8f);
                if (snareOn && (step == 4 || step == 12)) snare(c, b, 0.8f, 0.9f);
                if (hatsOn && step % 2 == 0) {
                    if (openOn && odd && step == 14) hat(c, b, 0.6f, true, 0.1f);
                    else hat(c, b, step % 4 == 0 ? 0.45f : 0.3f, false, 0.15f);
                }
                break;
            case DrumPat::TechHouse:
                if (kickOn && step % 4 == 0) kick(c, b, 1.0f);
                if (snareOn && (step == 4 || step == 12)) clap(c, b, 1.0f);
                if (fill && step == 15) clap(c, b, 0.5f);
                if (hatsOn) {
                    if (step % 4 == 2) {
                        hat(c, b, openOn ? 0.85f : 0.75f, openOn, 0.15f);
                    } else if (extraOn || s.kind == Sec::Build || (s.kind == Sec::Intro && second)) {
                        float v = (step & 1) ? 0.3f + 0.15f * r.uni() : 0.18f;
                        hat(c, b, v, false, (step & 1) ? -0.35f : 0.35f);
                    }
                }
                if (st.perc && (drop || s.kind == Sec::Build)) {
                    if (step == 3 || step == 11) perc(c, b, 0.55f, 1650.0f, 0.012f, -0.4f, 0.8f);
                    if (odd && (step == 7 || step == 14)) perc(c, b, 0.5f, 330.0f, 0.08f, 0.45f, 0.15f);
                    if (!odd && step == 9) perc(c, b, 0.45f, 225.0f, 0.1f, 0.5f, 0.1f);
                }
                break;
            case DrumPat::TwoStep:
                if (kickOn && (step == 0 || step == 10)) kick(c, b, step == 0 ? 1.0f : 0.85f);
                if (snareOn && (step == 4 || step == 12)) {
                    clap(c, b, 0.75f);
                    snare(c, b, 0.5f, 1.1f);
                }
                if (fill && step == 15) snare(c, b, 0.3f, 1.2f);
                if (hatsOn) {
                    if (openOn && step == 14) hat(c, b, 0.6f, true, -0.1f);
                    else if (step % 2 == 0) hat(c, b, step % 4 == 2 ? 0.7f : 0.4f, false, 0.15f);
                    else if (extraOn || s.kind == Sec::Build) hat(c, b, 0.22f + 0.1f * r.uni(), false, -0.3f);
                }
                if (st.perc && drop) {
                    if ((odd && (step == 7 || step == 15)) || (!odd && step == 3)) perc(c, b, 0.5f, 1900.0f, 0.01f, 0.35f, 0.9f);
                    if (step == 9 && odd) perc(c, b, 0.35f, 280.0f, 0.07f, -0.45f, 0.1f);
                }
                break;
            case DrumPat::DnbRoll:
                if (kickOn && (step == 0 || step == 10)) kick(c, b, 1.0f);
                if (kickOn && drop && odd && step == 13 && r.chance(0.5f)) kick(c, b, 0.6f);
                if (snareOn && (step == 4 || step == 12)) snare(c, b, 1.0f, st.snarePitch);
                if (drop && (step == 7 || step == 9 || step == 14) && r.chance(st.ghostDensity))
                    snare(c, b, 0.16f + 0.12f * r.uni(), st.snarePitch);
                if (hatsOn) {
                    if (openOn && odd && step == 14) hat(c, b, 0.6f, true, -0.1f);
                    else if (step % 2 == 0) hat(c, b, step % 4 == 0 ? 0.45f : 0.32f, false, 0.2f);
                    else if (extraOn) hat(c, b, 0.14f + 0.1f * r.uni(), false, -0.25f);
                    if (st.ride && (drop || openOn) && step % 4 == 0) hat(c, b, 0.3f, true, -0.35f);
                }
                break;
            case DrumPat::Jungle: {
                // Chopped "amen"-style bars: 0 kick, 1 snare, 2 ghost snare.
                static const int pats[4][10][2] = {
                    {{0, 0}, {2, 0}, {4, 1}, {7, 2}, {9, 2}, {10, 0}, {11, 0}, {12, 1}, {15, 2}, {-1, 0}},
                    {{0, 0}, {2, 0}, {4, 1}, {7, 2}, {9, 2}, {10, 0}, {12, 1}, {14, 2}, {-1, 0}, {-1, 0}},
                    {{0, 0}, {4, 1}, {6, 0}, {7, 2}, {10, 0}, {12, 1}, {13, 2}, {14, 1}, {15, 2}, {-1, 0}},
                    {{0, 0}, {2, 0}, {4, 1}, {5, 2}, {8, 0}, {10, 1}, {11, 0}, {12, 1}, {14, 1}, {15, 2}},
                };
                if (step == 0) c.jungleVar = (rb % 4 == 3) ? 2 + r.range(2) : (odd ? 1 : 0);
                for (const auto& h : pats[c.jungleVar]) {
                    if (h[0] != step) continue;
                    if (h[1] == 0 && kickOn) kick(c, b, step == 0 ? 1.0f : 0.8f);
                    if (h[1] == 1 && snareOn) snare(c, b, 0.95f, st.snarePitch * (step == 14 ? 1.1f : 1.0f));
                    if (h[1] == 2 && snareOn) snare(c, b, 0.3f, st.snarePitch * 1.05f);
                }
                if (hatsOn) {
                    if (step % 2 == 0) hat(c, b, step % 4 == 0 ? 0.42f : 0.3f, false, 0.2f);
                    else if (extraOn) hat(c, b, 0.18f + 0.1f * r.uni(), false, -0.25f);
                    if (openOn && step == 6 && odd) hat(c, b, 0.5f, true, -0.2f);
                }
                break;
            }
            }
        }
    }
}

void scheduleMusic(Ctx& c, const Section& s) {
    if (s.kind == Sec::Intro || s.kind == Sec::Outro) return;
    const StyleCfg& st = c.st;
    const int root = c.spec.rootMidi;
    const bool drop = s.kind == Sec::Drop || s.kind == Sec::Drop2;
    const bool breakish = s.kind == Sec::Break || s.kind == Sec::Build2;
    const ChordInstr ci = breakish ? st.chordsBreak : st.chordsDrop;
    const bool bassOn = (s.kind == Sec::Build || drop) && st.bass != BassPat::Acid;
    const int endBar = s.start + s.bars;

    if (st.bass == BassPat::Acid) {
        float bright = s.kind == Sec::Build ? 0.55f : s.kind == Sec::Break ? 0.7f : s.kind == Sec::Build2 ? 1.0f
                     : s.kind == Sec::Drop2 ? 1.3f : 1.0f;
        acidLine(c, s.start, s.bars, root - 12, bright);
    }

    for (int rb = 0; rb < s.bars; ++rb) {
        const int bar = s.start + rb;
        const float prog = (float(rb) + 0.5f) / float(s.bars);
        const int cd = chordDegAt(c, bar);
        const bool chordStart = bar % st.chordBars == 0;
        const bool second = rb >= s.bars / 2;
        const bool odd = (bar & 1) != 0;
        float cutMul = 1.0f;
        if (s.kind == Sec::Build) cutMul = 0.15f + 0.85f * std::pow(prog, 1.5f);
        else if (s.kind == Sec::Break) cutMul = 0.4f + 0.6f * prog;

        // Bass.
        if (bassOn) {
            const int bn = bassMidi(root, cd);
            const double b0 = bar * 4.0;
            switch (st.bass) {
            case BassPat::Offbeat:
                for (int k = 0; k < 4; ++k)
                    bassNote(c, b0 + k + 0.5, 0.35, bn + ((odd && k == 3) ? 12 : 0), 1.0f);
                break;
            case BassPat::Octave:
                for (int k = 0; k < 8; ++k) bassNote(c, b0 + k * 0.5, 0.4, bn + ((k & 1) ? 12 : 0), 1.0f);
                break;
            case BassPat::Rolling:
                for (int step = 0; step < 16; ++step) {
                    if (step % 4 == 0) continue;
                    int note = bn;
                    if (bar % 4 == 3 && step == 15) note += 12;
                    bassNote(c, beatOf(st, bar, step), 0.22, note, step % 4 == 2 ? 1.0f : 0.75f);
                }
                break;
            case BassPat::Reese:
                bassNote(c, b0, 1.4, bn, 1.0f);
                bassNote(c, b0 + 1.5, 0.9, bn, 0.9f);
                bassNote(c, b0 + 2.5, 1.4, odd ? bassMidi(root, cd + 4) : bn, 1.0f);
                break;
            case BassPat::Syncop:
                bassNote(c, beatOf(st, bar, 3), 0.4, bn, 0.9f);
                bassNote(c, beatOf(st, bar, 6), 0.5, bn, 1.0f);
                bassNote(c, beatOf(st, bar, 10), 0.4, bn + 12, 0.8f);
                bassNote(c, beatOf(st, bar, 13), 0.6, odd ? bassMidi(root, cd + 4) : bn, 0.9f);
                break;
            case BassPat::Sub:
                bassNote(c, b0, 2.6, bn, 1.0f);
                bassNote(c, beatOf(st, bar, 11), 1.1, odd ? bassMidi(root, cd + 4) : bn, 0.85f);
                break;
            case BassPat::Acid:
                break;
            case BassPat::TechRoll:
                for (int step : {2, 3, 6, 7, 10, 11, 14}) {
                    int note = bn;
                    if (odd && step == 14) note = bassMidi(root, cd + 4);
                    if (bar % 4 == 3 && step == 7) note += 12;
                    bassNote(c, beatOf(st, bar, step), 0.2, note, step % 4 == 2 ? 1.0f : 0.7f);
                }
                break;
            case BassPat::Disco: {
                static const int dsteps[8][2] = {{0, 0}, {2, 12}, {3, 0}, {6, 12}, {8, 0}, {10, 12}, {11, 10}, {14, 12}};
                for (const auto& d : dsteps) {
                    int off = (odd && d[0] == 11) ? 7 : d[1];
                    bassNote(c, beatOf(st, bar, d[0]), 0.18, bn + off, off ? 0.85f : 1.0f);
                }
                break;
            }
            case BassPat::Growl: {
                static const int gsteps[6][2] = {{2, 2}, {5, 1}, {6, 2}, {10, 2}, {13, 1}, {14, 2}};
                for (const auto& g : gsteps) {
                    int note = bn + 12;
                    if (odd && g[0] == 13) note += 12;
                    if (odd && g[0] == 14) note = bassMidi(root, cd + 4) + 12;
                    growlNote(c, beatOf(st, bar, g[0]), g[1] * 0.25 * 0.9, note, g[1] == 1 ? 0.85f : 1.0f, 0.5f);
                }
                break;
            }
            case BassPat::Garage: {
                static const int gsteps[4][2] = {{0, 3}, {6, 2}, {10, 3}, {14, 2}};
                for (const auto& g : gsteps) {
                    int note = bn;
                    if (odd && g[0] == 10) note += 12;
                    if (odd && g[0] == 14) note = bassMidi(root, cd + 4);
                    bassNote(c, beatOf(st, bar, g[0]), g[1] * 0.25, note, g[0] == 0 ? 1.0f : 0.85f);
                }
                break;
            }
            case BassPat::LiquidSub:
                bassNote(c, b0, 2.4, bn, 1.0f);
                bassNote(c, b0 + 2.5, 1.35, odd ? bassMidi(root, cd + 4) : bn, 0.9f);
                break;
            case BassPat::Neuro: {
                static const double nb[5][2] = {{0, 1.25}, {1.5, 0.25}, {1.75, 0.5}, {2.5, 1.0}, {3.5, 0.4}};
                static const int noff[5] = {0, 0, 12, 0, 7};
                for (int k = 0; k < 5; ++k) {
                    int note = bn + 12 + noff[k] + ((odd && k == 3) ? 3 : 0);
                    growlNote(c, b0 + nb[k][0], nb[k][1], note, k == 0 ? 1.0f : 0.9f, odd ? 0.375f : 0.75f);
                }
                break;
            }
            case BassPat::Wobble: {
                static const float lfoBeats[4] = {0.5f, 0.25f, 0.5f, 1.0f / 3.0f};
                const float lb = lfoBeats[bar % 4];
                growlNote(c, b0, 1.45, bn + 12, 1.0f, lb);
                growlNote(c, b0 + 1.5, 0.45, bn + 24, 0.9f, lb);
                growlNote(c, b0 + 2.0, 0.95, bn + 12, 1.0f, lb);
                growlNote(c, b0 + 3.0, 0.95, bn + 12 + (odd ? 3 : -2), 0.95f, lb);
                break;
            }
            case BassPat::JungleSub:
                if (odd) {
                    bassNote(c, b0, 1.75, bn, 1.0f);
                    bassNote(c, b0 + 2.0, 1.9, bassMidi(root, cd + 4), 0.95f);
                } else {
                    bassNote(c, b0, 3.8, bn, 1.0f);
                }
                break;
            }
        }

        // Chords.
        if (ci == ChordInstr::Pad && chordStart) {
            const int lenBars = std::min(st.chordBars, endBar - bar);
            for (int m : voiceChord(root, cd, st.sevenths, root + st.chordCenter))
                padNote(c, bar * 4.0, lenBars * 4.0 - 0.05, m, cutMul);
        } else if (ci == ChordInstr::Stabs) {
            std::vector<int> steps;
            const bool customSteps = !st.stabStepsA.empty() || !st.stabStepsB.empty();
            const double gate = customSteps ? double(st.stabGate) : 0.3;
            if (customSteps) {
                steps = odd ? st.stabStepsB : st.stabStepsA;
            } else if (st.drums == DrumPat::FourFloor && st.bass == BassPat::Rolling) {
                steps = odd ? std::vector<int>{14} : std::vector<int>{6};  // sparse dub-techno stab
            } else {
                steps = odd ? std::vector<int>{3, 6, 13} : std::vector<int>{3, 10};
            }
            const auto notes = voiceChord(root, cd, st.sevenths, root + st.chordCenter);
            for (int step : steps) {
                float pan = -0.2f;
                for (int m : notes) {
                    pluck(c, beatOf(st, bar, step), gate, m, 1.0f, pan, st.stab, cutMul);
                    pan += 0.4f / float(notes.size());
                }
            }
        } else if (ci == ChordInstr::Rhodes && chordStart) {
            const int lenBars = std::min(st.chordBars, endBar - bar);
            const auto notes = voiceChord(root, cd, st.sevenths, root + st.chordCenter);
            double strum = 0;
            float pan = -0.3f;
            for (int m : notes) {
                rhodes(c, bar * 4.0 + strum, lenBars * 4.0 - 0.3 - strum, m, 0.8f, pan, {0.1f, 0.3f});
                strum += 0.03;
                pan += 0.2f;
            }
            // Bass note of the chord an octave below for body.
            rhodes(c, bar * 4.0, lenBars * 4.0 - 0.3, notes.front() - 12, 0.6f, 0.0f, {0.0f, 0.2f});
        } else if (ci == ChordInstr::Organ) {
            const auto notes = voiceChord(root, cd, st.sevenths, root + st.chordCenter);
            for (int step : odd ? st.stabStepsB : st.stabStepsA) {
                float pan = -0.25f;
                for (int m : notes) {
                    organ(c, beatOf(st, bar, step), st.stabGate, m, 0.9f, pan);
                    pan += 0.5f / float(notes.size());
                }
            }
        }
        if (st.padUnderDrop && drop && chordStart && ci != ChordInstr::Pad) {
            const int lenBars = std::min(st.chordBars, endBar - bar);
            for (int m : voiceChord(root, cd, st.sevenths, root + st.chordCenter + 12))
                padNote(c, bar * 4.0, lenBars * 4.0 - 0.05, m, 0.7f);
        }

        // Arp.
        const bool arpOn = st.arp && (drop || s.kind == Sec::Build2 || (s.kind == Sec::Break && second));
        if (arpOn) {
            auto tones = voiceChord(root, cd, false, root + 14);
            tones.push_back(tones.front() + 12);
            const int n = int(4.0f / st.arpStep);
            for (int k = 0; k < n; ++k) {
                int idx = st.arpPattern[size_t(k) % st.arpPattern.size()];
                int note = tones[size_t(idx) % tones.size()];
                pluck(c, bar * 4.0 + k * st.arpStep, st.arpStep * 0.5, note, (k % 4 == 0) ? 1.0f : 0.7f,
                      (k & 1) ? 0.35f : -0.35f, st.arpP, s.kind == Sec::Break ? cutMul : 1.0f);
            }
        }

        // Lead melody, one motif per two-bar phrase.
        const bool leadOn = (s.kind == Sec::Drop && st.leadDrop1) || (s.kind == Sec::Drop2 && st.leadDrop2) ||
                            (s.kind == Sec::Break && st.leadBreak && second);
        if (leadOn && rb % 2 == 0) {
            const int phrase = rb / 2;
            for (const MNote& mn : c.motif) {
                int d = mn.deg;
                if ((phrase & 1) && mn.step >= 24) d += 2;
                int noteBar = bar + mn.step / 16;
                if (noteBar >= endBar) continue;
                if (mn.step % 4 == 0) d = snapToChord(d, chordDegAt(c, noteBar));
                int midi = degMidi(root + 12, d);
                double beat = bar * 4.0 + mn.step * 0.25;
                if (st.swing8 > 0 && (mn.step & 3) == 2) beat += st.swing8 * 0.5;
                leadNote(c, beat, mn.len * 0.25 * 0.9, midi, mn.step % 8 == 0 ? 1.0f : 0.85f);
            }
        }
    }
}

// ---------------------------------------------------------------- effects

void applyDelay(Ctx& c, Bus& out) {
    out.init(c.total);
    const size_t D = std::max<size_t>(1, size_t(c.st.dlyBeats * c.fpb));
    std::vector<float> bl(D, 0.0f), br(D, 0.0f);
    size_t w = 0;
    float lpl = 0, lpr = 0;
    const float fb = c.st.dlyFb;
    for (size_t i = 0; i < c.total; ++i) {
        float yl = bl[w], yr = br[w];
        lpl += (yl - lpl) * 0.45f;
        lpr += (yr - lpr) * 0.45f;
        float in = (c.dly.l[i] + c.dly.r[i]) * 0.5f;
        bl[w] = in + lpr * fb;
        br[w] = lpl * fb;
        out.l[i] = yl;
        out.r[i] = yr;
        if (++w == D) w = 0;
    }
}

struct Comb {
    std::vector<float> buf;
    size_t idx = 0;
    float store = 0;
    float process(float in, float fb, float damp) {
        float out = buf[idx];
        store = out * (1.0f - damp) + store * damp;
        buf[idx] = in + store * fb;
        if (++idx >= buf.size()) idx = 0;
        return out;
    }
};

struct Allpass {
    std::vector<float> buf;
    size_t idx = 0;
    float process(float in) {
        float b = buf[idx];
        buf[idx] = in + b * 0.5f;
        if (++idx >= buf.size()) idx = 0;
        return b - in;
    }
};

void applyReverb(Ctx& c, Bus& out) {
    out.init(c.total);
    static const int combT[4] = {1116, 1188, 1277, 1356};
    static const int apT[2] = {556, 441};
    const int spread = 23;
    Comb cl[4], cr[4];
    Allpass al[2], ar[2];
    for (int k = 0; k < 4; ++k) {
        cl[k].buf.assign(size_t(combT[k]), 0.0f);
        cr[k].buf.assign(size_t(combT[k] + spread), 0.0f);
    }
    for (int k = 0; k < 2; ++k) {
        al[k].buf.assign(size_t(apT[k]), 0.0f);
        ar[k].buf.assign(size_t(apT[k] + spread), 0.0f);
    }
    const float fb = c.st.revRoom, damp = 0.3f;
    float hpState = 0;
    for (size_t i = 0; i < c.total; ++i) {
        float in = (c.rev.l[i] + c.rev.r[i]) * 0.5f;
        hpState += (in - hpState) * 0.03f;  // ~200 Hz high-pass to keep the tail clean
        in = (in - hpState) * 0.03f;
        float sl = 0, sr = 0;
        for (int k = 0; k < 4; ++k) {
            sl += cl[k].process(in, fb, damp);
            sr += cr[k].process(in, fb, damp);
        }
        for (int k = 0; k < 2; ++k) {
            sl = al[k].process(sl);
            sr = ar[k].process(sr);
        }
        out.l[i] = sl;
        out.r[i] = sr;
    }
}

}  // namespace

// ------------------------------------------------------------------ public

const std::vector<SongSpec>& stockSongs() {
    static const std::vector<SongSpec> songs = {
        {"Neon Arcade", "Pixel Prophet", "Synth House", "A min", 118.0, 57, 0xA11CE5u, SongStyle::SynthHouse},
        {"Midnight Circuit", "Ohm Sweet Ohm", "Techno", "F min", 128.0, 53, 0xC1C0172u, SongStyle::Techno},
        {"Sunset Boulevard", "Velvet Static", "Deep House", "D min", 122.0, 62, 0x5A5E7u, SongStyle::DeepHouse},
        {"Hyperdrive", "Breakneck Bureau", "Drum & Bass", "E min", 174.0, 52, 0xD1B0u, SongStyle::DrumAndBass},
        {"Crystal Caves", "Aurora Protocol", "Trance", "G min", 138.0, 55, 0xC4157A1u, SongStyle::Trance},
        {"Lo-Fi Lagoon", "Sleepy Cassette", "Lo-Fi Hip Hop", "C min", 88.0, 60, 0x10F1u, SongStyle::LoFi},
        {"Acid Rain", "The 303 Collective", "Acid Techno", "C# min", 130.0, 61, 0xAC1Du, SongStyle::AcidTechno},
        {"Cloud Nine", "Stratos & Friends", "Progressive House", "B min", 126.0, 59, 0xC10D9u, SongStyle::ProgHouse},
        {"Warehouse Shuffle", "Concrete Groove", "Tech House", "E min", 126.0, 52, 0x7EC4u, SongStyle::TechHouse},
        {"Mirrorball Motel", "Funk Voyager", "Disco House", "F min", 124.0, 53, 0xD15C0u, SongStyle::DiscoHouse},
        {"Subwoofer Sermon", "Wub Cartel", "Bass House", "F# min", 128.0, 54, 0xBA55u, SongStyle::BassHouse},
        {"Night Bus Home", "Skippy Delgado", "UK Garage", "Bb min", 132.0, 58, 0x2573Eu, SongStyle::UkGarage},
        {"Rainfall Theory", "Soft Circuits", "Liquid DnB", "D min", 172.0, 62, 0x11C1Du, SongStyle::LiquidDnb},
        {"Chrome Mandible", "Neural Gremlin", "Neurofunk", "F min", 174.0, 53, 0x2E60u, SongStyle::Neurofunk},
        {"Bounce Protocol", "MC Trampoline", "Jump-Up DnB", "G min", 175.0, 55, 0x1BA7u, SongStyle::JumpUp},
        {"Rewind Selecta", "Dubplate Ghosts", "Jungle", "A min", 168.0, 57, 0x7A6Eu, SongStyle::Jungle},
    };
    return songs;
}

TrackPtr renderSong(const SongSpec& spec, std::atomic<float>* progress) {
    auto setProgress = [&](float p) {
        if (progress) progress->store(p);
    };
    setProgress(0.0f);

    Ctx c(spec);
    c.fpb = 60.0 / spec.bpm * kSampleRate;
    const auto sections = makeSections(c.st);
    const int totalBars = sections.back().start + sections.back().bars;
    c.total = size_t(std::llround(totalBars * 4.0 * c.fpb));
    for (Bus* b : {&c.drums, &c.bass, &c.music, &c.fx, &c.dly, &c.rev}) b->init(c.total);
    makePatterns(c);

    for (const Section& s : sections) scheduleDrums(c, s);
    setProgress(0.3f);
    for (size_t k = 0; k < sections.size(); ++k) {
        scheduleMusic(c, sections[k]);
        setProgress(0.3f + 0.45f * float(k + 1) / float(sections.size()));
    }

    Bus dOut, rOut;
    applyDelay(c, dOut);
    applyReverb(c, rOut);
    setProgress(0.85f);

    // Sidechain gain from kick hits.
    std::vector<float> sc(c.total, 1.0f);
    {
        const size_t atk = size_t(0.003f * kSr);
        const size_t n = size_t(c.st.scRelease * 5.0f * kSr);
        const float rc = decayCoef(c.st.scRelease);
        for (size_t s0 : c.kicks) {
            float e = 1.0f;
            for (size_t j = 0; j < n && s0 + j < c.total; ++j) {
                float d = j < atk ? float(j) / float(atk) : e;
                if (j >= atk) e *= rc;
                float g = 1.0f - c.st.scDepth * d;
                if (g < sc[s0 + j]) sc[s0 + j] = g;
            }
        }
    }

    // Master sum. Bus gains set the drums / bass / music balance.
    constexpr float kDrumGain = 0.7f, kBassGain = 1.8f, kMusicGain = 2.6f;
    std::vector<float> L(c.total), R(c.total);
    Rng nr(spec.seed ^ 0x5EEDu);
    Svf ml1, ml2, mr1, mr2;
    ml1.set(5200, 0.7f);
    ml2.set(5200, 0.7f);
    mr1.set(5200, 0.7f);
    mr2.set(5200, 0.7f);
    float crackle = 0, hiss = 0;
    float dcxl = 0, dcyl = 0, dcxr = 0, dcyr = 0;
    for (size_t i = 0; i < c.total; ++i) {
        const float s = sc[i], s2 = 0.5f + 0.5f * s;
        float l = kDrumGain * c.drums.l[i] + c.fx.l[i] + s * (kBassGain * c.bass.l[i] + kMusicGain * c.music.l[i]) +
                  s2 * kMusicGain * (dOut.l[i] * c.st.dlyReturn + rOut.l[i] * c.st.revReturn);
        float r = kDrumGain * c.drums.r[i] + c.fx.r[i] + s * (kBassGain * c.bass.r[i] + kMusicGain * c.music.r[i]) +
                  s2 * kMusicGain * (dOut.r[i] * c.st.dlyReturn + rOut.r[i] * c.st.revReturn);
        if (c.st.lofi) {
            l = ml2.lp(ml1.lp(l));
            r = mr2.lp(mr1.lp(r));
            if (nr.chance(9.0f / kSr)) crackle = nr.bi() * (0.03f + 0.08f * nr.uni());
            crackle *= 0.93f;
            hiss += (nr.bi() - hiss) * 0.2f;
            l += crackle + hiss * 0.006f;
            r += crackle * 0.8f + hiss * 0.006f;
        }
        // DC blocker.
        dcyl = l - dcxl + 0.9995f * dcyl;
        dcxl = l;
        dcyr = r - dcxr + 0.9995f * dcyr;
        dcxr = r;
        L[i] = dcyl;
        R[i] = dcyr;
    }

    // Loudness: scale so the 99.9th percentile sits at 0.9, then soft-knee clip.
    float peak = 0;
    for (size_t i = 0; i < c.total; ++i) peak = std::max({peak, std::fabs(L[i]), std::fabs(R[i])});
    if (peak > 0) {
        constexpr int kBins = 2048;
        std::vector<size_t> hist(kBins, 0);
        for (size_t i = 0; i < c.total; ++i) {
            float a = std::max(std::fabs(L[i]), std::fabs(R[i]));
            hist[size_t(std::min(kBins - 1, int(a / peak * (kBins - 1))))]++;
        }
        size_t acc = 0, target = size_t(double(c.total) * 0.999);
        int bin = kBins - 1;
        for (int k = 0; k < kBins; ++k) {
            acc += hist[size_t(k)];
            if (acc >= target) {
                bin = k;
                break;
            }
        }
        const float pct = std::max(peak * float(bin + 1) / float(kBins), peak * 0.05f);
        const float gain = std::min(0.9f / pct, 2.0f / peak);
        constexpr float thr = 0.75f;
        float outPeak = 0;
        for (size_t i = 0; i < c.total; ++i) {
            for (float* x : {&L[i], &R[i]}) {
                float v = *x * gain, a = std::fabs(v);
                if (a > thr) v = std::copysign(thr + (1.0f - thr) * ftanh((a - thr) / (1.0f - thr)), v);
                *x = v;
                outPeak = std::max(outPeak, std::fabs(v));
            }
        }
        const float norm = outPeak > 0 ? 0.891f / outPeak : 1.0f;
        for (size_t i = 0; i < c.total; ++i) {
            L[i] *= norm;
            R[i] *= norm;
        }
    }
    // Short fade at the very end to avoid a click.
    const size_t fade = std::min(c.total, size_t(0.08f * kSr));
    for (size_t k = 0; k < fade; ++k) {
        float g = float(k) / float(fade);
        L[c.total - 1 - k] *= g;
        R[c.total - 1 - k] *= g;
    }

    auto t = std::make_shared<Track>();
    t->name = spec.name;
    t->artist = spec.artist;
    t->genre = spec.genre;
    t->key = spec.keyName;
    t->bpm = spec.bpm;
    t->firstBeatSec = 0.0;
    t->isGenerated = true;
    t->samples.resize(c.total * 2);
    for (size_t i = 0; i < c.total; ++i) {
        t->samples[2 * i] = L[i];
        t->samples[2 * i + 1] = R[i];
    }
    setProgress(1.0f);
    return t;
}
