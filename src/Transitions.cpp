#include "Transitions.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace {

struct ParamInfo {
    const char* name;
    const char* id;
    float neutral;
};

const ParamInfo kParamInfo[kNumParams] = {
    {"Crossfader", "xfade", 0.0f},
    {"Out Volume", "out_vol", 1.0f},  {"In Volume", "in_vol", 1.0f},
    {"Out Low EQ", "out_low", 0.5f},  {"In Low EQ", "in_low", 0.5f},
    {"Out Mid EQ", "out_mid", 0.5f},  {"In Mid EQ", "in_mid", 0.5f},
    {"Out High EQ", "out_high", 0.5f}, {"In High EQ", "in_high", 0.5f},
    {"Out Filter", "out_filter", 0.5f}, {"In Filter", "in_filter", 0.5f},
    {"Out Echo", "out_echo", 0.0f},   {"In Echo", "in_echo", 0.0f},
};

float smoothstep(float x) { return x * x * (3 - 2 * x); }

// Builder helper so stock definitions read like a score.
struct Builder {
    TransitionDef d;
    Builder(const char* name, const char* desc, int beats) {
        d.name = name;
        d.description = desc;
        d.beats = beats;
        d.stock = true;
    }
    Builder& keys(Param p, std::initializer_list<Keyframe> ks) {
        Lane& l = d.lane(p);
        l.enabled = true;
        l.keys = ks;
        l.sortKeys();
        return *this;
    }
    Builder& inStart(float t) { d.inStartAt = t; return *this; }
    Builder& outFx(OutEffect e, float t) { d.outEffect = e; d.outEffectAt = t; return *this; }
};

constexpr CurveShape L = CurveShape::Linear;
constexpr CurveShape S = CurveShape::Smooth;
constexpr CurveShape H = CurveShape::Step;

std::string quoted(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (c == '\n') { out += "\\n"; continue; }
        out += c;
    }
    return out + "\"";
}

bool readQuoted(std::istream& in, std::string& out) {
    out.clear();
    in >> std::ws;
    if (in.get() != '"') return false;
    for (int c; (c = in.get()) != EOF;) {
        if (c == '"') return true;
        if (c == '\\') {
            c = in.get();
            if (c == 'n') c = '\n';
        }
        out += char(c);
    }
    return false;
}

const char* effectId(OutEffect e) {
    switch (e) {
        case OutEffect::Brake: return "brake";
        case OutEffect::Backspin: return "backspin";
        default: return "none";
    }
}

}  // namespace

const char* paramName(Param p) { return kParamInfo[int(p)].name; }
const char* paramId(Param p) { return kParamInfo[int(p)].id; }
float paramNeutral(Param p) { return kParamInfo[int(p)].neutral; }

void Lane::sortKeys() {
    std::stable_sort(keys.begin(), keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.t < b.t; });
}

float Lane::eval(float t) const {
    if (keys.empty()) return 0.0f;
    if (t <= keys.front().t) return keys.front().v;
    if (t >= keys.back().t) return keys.back().v;
    for (size_t i = 0; i + 1 < keys.size(); ++i) {
        const Keyframe& a = keys[i];
        const Keyframe& b = keys[i + 1];
        if (t < b.t) {
            float span = b.t - a.t;
            float x = span > 1e-6f ? (t - a.t) / span : 1.0f;
            switch (a.shape) {
                case CurveShape::Step: x = 0; break;
                case CurveShape::Smooth: x = smoothstep(x); break;
                case CurveShape::Linear: break;
            }
            return a.v + (b.v - a.v) * x;
        }
    }
    return keys.back().v;
}

std::vector<TransitionDef> makeStockTransitions() {
    std::vector<TransitionDef> v;

    v.push_back(Builder("Smooth Crossfade", "A classic, even blend over 4 bars. Safe for almost any pair of tracks.", 16)
                    .keys(Param::Crossfader, {{0, 0, S}, {1, 1, S}})
                    .d);

    v.push_back(Builder("Bass Swap", "Brings the new track in without its bass, then swaps the basslines on the drop. "
                                     "The go-to club transition.", 32)
                    .keys(Param::Crossfader, {{0, 0, S}, {0.25f, 0.5f, L}, {0.75f, 0.5f, S}, {1, 1, L}})
                    .keys(Param::InLow, {{0, 0, H}, {0.5f, 0.5f, L}, {1, 0.5f, L}})
                    .keys(Param::OutLow, {{0, 0.5f, H}, {0.5f, 0, L}, {1, 0, L}})
                    .d);

    v.push_back(Builder("Filter Sweep", "The outgoing track is thinned out with a rising high-pass while the new "
                                        "track opens up from a low-pass.", 16)
                    .keys(Param::OutFilter, {{0, 0.5f, S}, {1, 0.92f, L}})
                    .keys(Param::InFilter, {{0, 0.08f, S}, {0.85f, 0.5f, L}, {1, 0.5f, L}})
                    .keys(Param::Crossfader, {{0, 0, S}, {0.3f, 0.5f, L}, {0.7f, 0.5f, S}, {1, 1, L}})
                    .d);

    v.push_back(Builder("Echo Out", "The old track dissolves into echoes, then the new track slams in on the "
                                    "next bar.", 8)
                    .keys(Param::OutEcho, {{0, 0, S}, {0.45f, 1, L}, {1, 1, L}})
                    .keys(Param::OutVolume, {{0, 1, L}, {0.45f, 1, L}, {0.5f, 0, L}, {1, 0, L}})
                    .keys(Param::Crossfader, {{0, 0, H}, {0.5f, 1, L}, {1, 1, L}})
                    .inStart(0.5f)
                    .d);

    v.push_back(Builder("Backspin", "Rewinds the old record like a turntablist, then drops the new track on the "
                                    "next downbeat.", 4)
                    .keys(Param::Crossfader, {{0, 0, H}, {0.98f, 1, L}, {1, 1, L}})
                    .outFx(OutEffect::Backspin, 0.0f)
                    .inStart(1.0f)
                    .d);

    v.push_back(Builder("Power Down", "The old track slows to a stop like a turntable losing power, then the new "
                                      "track kicks in.", 4)
                    .keys(Param::Crossfader, {{0, 0, H}, {0.98f, 1, L}, {1, 1, L}})
                    .outFx(OutEffect::Brake, 0.0f)
                    .inStart(1.0f)
                    .d);

    v.push_back(Builder("Quick Cut", "Instantly switches tracks on the next bar. Punchy and hard to mess up.", 1)
                    .keys(Param::Crossfader, {{0, 1, H}, {1, 1, L}})
                    .d);

    v.push_back(Builder("Long EQ Blend", "A patient 16-bar mix: highs first, basses swapped at the midpoint, then "
                                         "the old track fades away.", 64)
                    .keys(Param::Crossfader, {{0, 0, S}, {0.2f, 0.5f, L}, {0.75f, 0.5f, S}, {1, 1, L}})
                    .keys(Param::InLow, {{0, 0, H}, {0.5f, 0.5f, L}, {1, 0.5f, L}})
                    .keys(Param::OutLow, {{0, 0.5f, H}, {0.5f, 0, L}, {1, 0, L}})
                    .keys(Param::InHigh, {{0, 0.2f, S}, {0.35f, 0.5f, L}, {1, 0.5f, L}})
                    .keys(Param::OutHigh, {{0, 0.5f, L}, {0.5f, 0.5f, S}, {0.9f, 0.2f, L}, {1, 0.2f, L}})
                    .keys(Param::InMid, {{0, 0.3f, S}, {0.5f, 0.5f, L}, {1, 0.5f, L}})
                    .d);

    v.push_back(Builder("Wash Out", "High-pass plus echo washes the old track into the distance while the new "
                                    "one fades up underneath.", 16)
                    .keys(Param::OutFilter, {{0, 0.5f, S}, {0.75f, 0.85f, L}, {1, 0.85f, L}})
                    .keys(Param::OutEcho, {{0, 0, S}, {0.6f, 0.8f, L}, {1, 0.8f, L}})
                    .keys(Param::OutVolume, {{0, 1, L}, {0.7f, 1, S}, {0.8f, 0, L}, {1, 0, L}})
                    .keys(Param::Crossfader, {{0, 0, S}, {0.5f, 0.5f, S}, {0.8f, 1, L}, {1, 1, L}})
                    .d);
    return v;
}

bool saveTransitions(const std::string& path, const std::vector<TransitionDef>& defs) {
    std::ofstream out(path);
    if (!out) return false;
    out << "# Afterglow custom transitions. Format: key/value lines, one block per transition.\n";
    for (const auto& d : defs) {
        if (d.stock) continue;
        out << "transition " << quoted(d.name) << "\n";
        out << "description " << quoted(d.description) << "\n";
        out << "beats " << d.beats << "\n";
        out << "in_start " << d.inStartAt << "\n";
        out << "out_effect " << effectId(d.outEffect) << " " << d.outEffectAt << "\n";
        for (int p = 0; p < kNumParams; ++p) {
            const Lane& l = d.lanes[size_t(p)];
            if (!l.enabled) continue;
            out << "lane " << paramId(Param(p));
            for (const auto& k : l.keys) out << " " << k.t << ":" << k.v << ":" << int(k.shape);
            out << "\n";
        }
        out << "end\n\n";
    }
    return bool(out);
}

std::vector<TransitionDef> loadTransitions(const std::string& path) {
    std::vector<TransitionDef> defs;
    std::ifstream in(path);
    if (!in) return defs;
    TransitionDef cur;
    bool open = false;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string word;
        if (!(ls >> word) || word[0] == '#') continue;
        if (word == "transition") {
            cur = TransitionDef{};
            open = readQuoted(ls, cur.name);
        } else if (!open) {
            continue;
        } else if (word == "description") {
            readQuoted(ls, cur.description);
        } else if (word == "beats") {
            ls >> cur.beats;
            cur.beats = std::clamp(cur.beats, 1, 256);
        } else if (word == "in_start") {
            ls >> cur.inStartAt;
            cur.inStartAt = std::clamp(cur.inStartAt, 0.0f, 1.0f);
        } else if (word == "out_effect") {
            std::string e;
            ls >> e >> cur.outEffectAt;
            cur.outEffect = e == "brake" ? OutEffect::Brake : e == "backspin" ? OutEffect::Backspin : OutEffect::None;
            cur.outEffectAt = std::clamp(cur.outEffectAt, 0.0f, 1.0f);
        } else if (word == "lane") {
            std::string id;
            ls >> id;
            for (int p = 0; p < kNumParams; ++p) {
                if (id != paramId(Param(p))) continue;
                Lane& l = cur.lanes[size_t(p)];
                l.enabled = true;
                l.keys.clear();
                std::string tok;
                while (ls >> tok) {
                    Keyframe k;
                    int shape = 1;
                    if (std::sscanf(tok.c_str(), "%f:%f:%d", &k.t, &k.v, &shape) >= 2) {
                        k.t = std::clamp(k.t, 0.0f, 1.0f);
                        k.v = std::clamp(k.v, 0.0f, 1.0f);
                        k.shape = CurveShape(std::clamp(shape, 0, 2));
                        l.keys.push_back(k);
                    }
                }
                l.sortKeys();
                if (l.keys.empty()) l.enabled = false;
            }
        } else if (word == "end") {
            if (!cur.name.empty()) defs.push_back(cur);
            open = false;
        }
    }
    return defs;
}
