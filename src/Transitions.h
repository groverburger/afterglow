// Transition definitions: automation lanes that move mixer controls over time.
#pragma once
#include <array>
#include <iosfwd>
#include <string>
#include <vector>

// Every automatable control. "Out" is the deck being mixed out, "In" is the
// deck being mixed in. All values are normalised 0..1:
//   Crossfader: 0 = fully Out, 1 = fully In
//   Volume / Echo: 0..1
//   Filter / EQ: 0.5 = neutral (filter: 0 low-pass, 1 high-pass; EQ: 0 = kill)
//   Tempo: 0 = the Out track's tempo, 1 = the In track's own tempo (both decks glide together)
enum class Param {
    Crossfader,
    OutVolume, InVolume,
    OutLow, InLow,
    OutMid, InMid,
    OutHigh, InHigh,
    OutFilter, InFilter,
    OutEcho, InEcho,
    Tempo,
    Count
};
constexpr int kNumParams = int(Param::Count);

const char* paramName(Param p);
const char* paramId(Param p);  // stable identifier for save files
float paramNeutral(Param p);

enum class CurveShape { Linear, Smooth, Step };

struct Keyframe {
    float t = 0;  // 0..1 through the transition
    float v = 0;  // normalised value
    CurveShape shape = CurveShape::Smooth;  // shape of the segment after this key
};

struct Lane {
    bool enabled = false;
    std::vector<Keyframe> keys;  // sorted by t
    float eval(float t) const;
    void sortKeys();
};

enum class OutEffect { None, Brake, Backspin, LoopRoll };
enum class InEffect { None, SpinUp };

struct TransitionDef {
    std::string name;
    std::string description;
    bool stock = false;
    int beats = 16;            // length, in beats of the outgoing track
    float inStartAt = 0.0f;    // when (0..1) the incoming deck starts playing
    OutEffect outEffect = OutEffect::None;
    float outEffectAt = 0.0f;  // when (0..1) the out effect fires
    InEffect inEffect = InEffect::None;  // how the incoming deck starts (at inStartAt)
    std::array<Lane, kNumParams> lanes;

    Lane& lane(Param p) { return lanes[size_t(p)]; }
    const Lane& lane(Param p) const { return lanes[size_t(p)]; }
    // True when both tracks are heard together for a while, so they need matching tempos.
    // Cuts, backspins and echo-outs work across any tempo gap.
    bool overlaps() const { return lane(Param::Tempo).enabled || (inStartAt < 0.5f && beats > 2); }
};

std::vector<TransitionDef> makeStockTransitions();

// Plain-text persistence. parseTransitions() skips any lines outside
// "transition ... end" blocks, so the format can be embedded in other files.
void writeTransition(std::ostream& out, const TransitionDef& def);
std::vector<TransitionDef> parseTransitions(std::istream& in);
std::string quoteString(const std::string& s);
bool readQuotedString(std::istream& in, std::string& out);
bool saveTransitions(const std::string& path, const std::vector<TransitionDef>& defs);
std::vector<TransitionDef> loadTransitions(const std::string& path);
