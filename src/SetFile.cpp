#include "SetFile.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>

#include "Engine.h"
#include "dr_wav.h"

namespace {

const char* kActionNames[] = {"note", "load", "play", "pause", "cue", "seek", "setpos", "setplaying",
                              "hotcue_set", "hotcue_jump", "hotcue_clear", "hotcue_at", "loop", "loop_exit",
                              "loop_range", "motion", "sync", "align", "transition", "cancel_transition",
                              "reset_fx", "master", "scratch", "param"};
const char* kParamNames[] = {"volume", "trim", "eq_low", "eq_mid", "eq_high", "filter", "echo", "tempo",
                             "tempo_range", "sync", "nudge", "cue_point", "slip", "crossfader", "xf_curve",
                             "master_volume", "quantize"};
constexpr int kNumActions = int(sizeof(kActionNames) / sizeof(kActionNames[0]));
static_assert(kNumActions == int(SetAction::Param) + 1, "action names out of date");
static_assert(sizeof(kParamNames) / sizeof(kParamNames[0]) == size_t(SetParam::Count), "param names out of date");

std::string num(double v) {
    char b[40];
    std::snprintf(b, sizeof(b), "%.17g", v);
    return b;
}

}  // namespace

const char* setActionName(SetAction a) { return kActionNames[int(a)]; }
const char* setParamName(SetParam p) { return kParamNames[int(p)]; }

TrackRef trackRefFor(const Track& t) {
    TrackRef r;
    r.name = t.name;
    if (t.isGenerated) r.kind = "stock";
    else if (t.isClip) r.kind = "clip";
    else {
        r.kind = "file";
        r.path = t.sourcePath;
    }
    return r;
}

// Format (one item per line):
//   afterglow-set 1
//   name "My Set"
//   length <frames>
//   transition "..." ... end           (definitions used by transition events)
//   ev <frame> <action> <deck> <param|param-name> <value> <value2> <flag> "<text>" <kind|-> "<track>" "<path>"
bool saveSet(const std::string& path, const SetRecording& set) {
    std::ofstream out(path);
    if (!out) return false;
    out << "afterglow-set " << set.version << "\n";
    out << "name " << quoteString(set.name) << "\n";
    out << "length " << set.lengthFrames << "\n\n";
    std::map<std::string, bool> written;
    for (const SetEvent& ev : set.events) {
        if (ev.action != SetAction::Transition || written[ev.def.name]) continue;
        written[ev.def.name] = true;
        writeTransition(out, ev.def);
    }
    for (const SetEvent& ev : set.events) {
        out << "ev " << ev.frame << " " << setActionName(ev.action) << " " << ev.deck << " ";
        if (ev.action == SetAction::Param) out << setParamName(SetParam(ev.param));
        else out << ev.param;
        out << " " << num(ev.value) << " " << num(ev.value2) << " " << (ev.flag ? 1 : 0) << " "
            << quoteString(ev.text) << " " << (ev.track.kind.empty() ? "-" : ev.track.kind) << " "
            << quoteString(ev.track.name) << " " << quoteString(ev.track.path) << "\n";
    }
    return bool(out);
}

bool parseSet(const std::string& text, SetRecording& set, std::string* error) {
    set = SetRecording{};
    std::istringstream all(text);
    std::string line;
    if (!std::getline(all, line) || line.rfind("afterglow-set", 0) != 0) {
        if (error) *error = "Not an Afterglow set file";
        return false;
    }
    set.version = std::max(1, std::atoi(line.c_str() + std::strlen("afterglow-set")));
    int lineNo = 1;
    while (std::getline(all, line)) {
        ++lineNo;
        std::istringstream ls(line);
        std::string word;
        if (!(ls >> word)) continue;
        if (word == "name") {
            readQuotedString(ls, set.name);
        } else if (word == "length") {
            ls >> set.lengthFrames;
        } else if (word == "ev") {
            SetEvent ev;
            std::string action, param, kind;
            int flag = 0;
            ls >> ev.frame >> action >> ev.deck >> param >> ev.value >> ev.value2 >> flag;
            ev.flag = flag != 0;
            bool ok = bool(ls) && readQuotedString(ls, ev.text);
            ls >> kind;
            ok = ok && readQuotedString(ls, ev.track.name) && readQuotedString(ls, ev.track.path);
            int a = -1;
            for (int i = 0; i < kNumActions; ++i)
                if (action == kActionNames[i]) a = i;
            if (!ok || a < 0) {
                if (error) *error = "Bad event on line " + std::to_string(lineNo);
                return false;
            }
            ev.action = SetAction(a);
            ev.track.kind = kind == "-" ? "" : kind;
            if (ev.action == SetAction::Param) {
                ev.param = -1;
                for (int i = 0; i < int(SetParam::Count); ++i)
                    if (param == kParamNames[i]) ev.param = i;
                if (ev.param < 0) continue;  // unknown parameter from a newer version: skip
            } else {
                ev.param = std::atoi(param.c_str());
            }
            ev.deck = std::clamp(ev.deck, 0, 1);
            set.events.push_back(std::move(ev));
        }
    }
    // Attach transition definitions by name (fall back to the stock ones).
    std::istringstream again(text);
    std::vector<TransitionDef> defs = parseTransitions(again);
    std::vector<TransitionDef> stock = makeStockTransitions();
    for (SetEvent& ev : set.events) {
        if (ev.action != SetAction::Transition) continue;
        bool found = false;
        for (auto& d : defs)
            if (d.name == ev.text) { ev.def = d; found = true; break; }
        if (!found)
            for (auto& d : stock)
                if (d.name == ev.text) { ev.def = d; found = true; break; }
        if (!found) ev.def = stock[0];
    }
    std::stable_sort(set.events.begin(), set.events.end(),
                     [](const SetEvent& a, const SetEvent& b) { return a.frame < b.frame; });
    if (set.lengthFrames == 0 && !set.events.empty()) set.lengthFrames = set.events.back().frame;
    return true;
}

bool loadSet(const std::string& path, SetRecording& set, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "Can't open " + path;
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return parseSet(ss.str(), set, error);
}

bool renderSetToWav(const SetRecording& set, const TrackResolver& resolve, const std::string& wavPath,
                    std::atomic<float>* progress, std::string* error,
                    const std::function<void(double, const std::string&)>& onNote) {
    drwav_data_format fmt{};
    fmt.container = drwav_container_riff;
    fmt.format = DR_WAVE_FORMAT_PCM;
    fmt.channels = 2;
    fmt.sampleRate = kSampleRate;
    fmt.bitsPerSample = 16;
    drwav wav;
    if (!drwav_init_file_write(&wav, wavPath.c_str(), &fmt, nullptr)) {
        if (error) *error = "Can't write " + wavPath;
        return false;
    }
    auto engine = std::make_unique<Engine>();
    Engine& e = *engine;
    e.startReplay(set);
    constexpr int kBlock = 64;
    std::vector<float> buf(kBlock * 2);
    std::vector<int16_t> pcm(kBlock * 2);
    const double total = double(std::max<uint64_t>(1, set.lengthFrames));
    while (e.replayActive()) {
        // Provide audio for the next load just before it falls due.
        for (size_t i = e.replayNext; i < e.replay.events.size(); ++i) {
            SetEvent& ev = e.replay.events[i];
            if (ev.frame > e.replayClock + kBlock) break;
            if (ev.action == SetAction::Load && !ev.track.empty() && !ev.resolved && !ev.resolveFailed) {
                ev.resolved = resolve(ev.track);
                ev.resolveFailed = !ev.resolved;
            }
        }
        e.render(buf.data(), kBlock, 2);
        for (auto& n : e.replayNotes)
            if (onNote) onNote(double(e.replayClock) / kSampleRate, n);
        e.replayNotes.clear();
        for (size_t i = 0; i < buf.size(); ++i)
            pcm[i] = int16_t(std::lrint(std::clamp(buf[i], -1.0f, 1.0f) * 32767.0f));
        drwav_write_pcm_frames(&wav, kBlock, pcm.data());
        if (progress) progress->store(float(double(e.replayClock) / total));
    }
    drwav_uninit(&wav);
    if (progress) progress->store(1.0f);
    return true;
}
