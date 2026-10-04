// Recorded DJ sets: a list of engine actions stamped with the audio sample
// clock. Replaying them through the (deterministic) engine reproduces the mix.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Track.h"
#include "Transitions.h"

enum class SetAction {
    Note,              // text shown to the listener
    Load,              // track -> deck (empty track = eject)
    Play, Pause, CueButton,
    Seek,              // value = frame (quantized seek)
    SetPos,            // value = frame (raw)
    SetPlaying,        // flag
    HotCueSet, HotCueJump, HotCueClear,  // param = slot
    HotCueAt,          // param = slot, value = frame (restores a saved cue)
    Loop,              // value = beats
    LoopExit,
    LoopRange,         // value = in frame, value2 = out frame
    Motion,            // param = Motion
    SyncTempo,
    AlignPhase,        // value = master beat, value2 = fold, flag = bar align
    Transition,        // text = name, deck = out deck, flag = land
    CancelTransition,
    ResetFx,
    MasterDeck,        // deck
    Scratch,           // flag = hand on the record, value = target frame
    Param,             // param = SetParam, value
};

enum class SetParam {
    Volume, Trim, EqLow, EqMid, EqHigh, Filter, Echo, Tempo, TempoRange, Sync, Nudge, CuePoint, Slip,
    // Global parameters (recorded on deck 0) from here on.
    Crossfader, XfCurve, MasterVolume, Quantize,
    Count
};

struct TrackRef {
    std::string kind;  // "stock", "clip", "file" or "" for none
    std::string name;
    std::string path;
    bool empty() const { return kind.empty(); }
};

struct SetEvent {
    uint64_t frame = 0;  // sample clock relative to the start of the set
    SetAction action = SetAction::Note;
    int deck = 0;
    int param = 0;
    double value = 0, value2 = 0;
    bool flag = false;
    std::string text;
    TrackRef track;
    TransitionDef def;   // Transition events

    // Runtime only (not saved).
    TrackPtr resolved;
    bool resolveFailed = false;
};

constexpr int kSetFormatVersion = 2;  // 2: transitions beat-match across bigger tempo gaps

struct SetRecording {
    std::string name;
    int version = kSetFormatVersion;
    uint64_t lengthFrames = 0;
    std::vector<SetEvent> events;

    double lengthSec() const { return double(lengthFrames) / kSampleRate; }
};

const char* setActionName(SetAction a);
const char* setParamName(SetParam p);
TrackRef trackRefFor(const Track& t);

bool saveSet(const std::string& path, const SetRecording& set);
bool parseSet(const std::string& text, SetRecording& set, std::string* error);
bool loadSet(const std::string& path, SetRecording& set, std::string* error);

// Plays a set through a private engine and writes the result to a 16-bit WAV.
// `resolve` turns track references into audio (may block while it renders/loads).
// `onNote` (optional) receives each Note with its time in seconds.
using TrackResolver = std::function<TrackPtr(const TrackRef&)>;
bool renderSetToWav(const SetRecording& set, const TrackResolver& resolve, const std::string& wavPath,
                    std::atomic<float>* progress, std::string* error,
                    const std::function<void(double, const std::string&)>& onNote = nullptr);
