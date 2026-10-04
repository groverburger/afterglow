// Audio engine: two decks, a mixer, the transition automation runner and a
// clip preview voice. Everything is protected by `mutex`; the audio callback
// holds it while rendering and the UI holds it while touching engine state.
#pragma once
#include <mutex>
#include <string>
#include <vector>

#include "DSP.h"
#include "SetFile.h"
#include "Track.h"
#include "Transitions.h"

constexpr int kNumHotCues = 4;

enum class Motion { Normal, Brake, Backspin, SpinUp };

struct Deck {
    int index = 0;
    TrackPtr track;

    // Transport. `pos` is in source frames and may be negative (pre-roll silence).
    double pos = 0;
    bool playing = false;
    double tempo = 0;          // playback rate offset, 0.05 = +5 %
    float tempoRange = 0.08f;  // slider range, +/-
    double nudge = 0;          // temporary rate bend while a nudge button is held
    bool sync = true;
    double syncFold = 1.0;     // 0.5 / 1 / 2: half/double-time relation to the master
    double cuePoint = 0;
    double hotCues[kNumHotCues] = {};
    bool hotCueSet[kNumHotCues] = {};
    bool loopActive = false;
    double loopIn = 0, loopOut = 0;
    float loopBeats = 0;

    // Mixer channel, all normalised (see Transitions.h for meanings).
    float volume = 1.0f;
    float trim = 0.5f;  // 0.5 = 0 dB, range -12..+12 dB
    float eq[3] = {0.5f, 0.5f, 0.5f};  // low, mid, high
    float filter = 0.5f;
    float echo = 0.0f;

    // Turntable effects.
    Motion motion = Motion::Normal;
    double motionTime = 0;
    double motionStartRate = 1;

    // Hand on the record: while `scratching`, the playhead chases `scratchTarget`
    // (forwards or backwards) instead of playing at `rate()`, paused or not.
    bool scratching = false;
    double scratchTarget = 0;
    double scratchRate = 0;
    // Slip mode: the track keeps running silently underneath a scratch (or loop)
    // and playback continues from there on release, so a mix stays in time.
    bool slip = false;
    double slipPos = 0;

    // DSP state.
    dsp::ThreeBandEq eqDsp;
    dsp::DjFilter filterDsp;
    dsp::Echo echoDsp;
    dsp::Smoother gainSm, echoSm;

    // Meters (written by audio thread).
    float meter = 0;

    bool loaded() const { return track != nullptr; }
    double rate() const { return 1.0 + tempo + nudge; }
    double secPerBeat() const { return track ? track->secPerBeat() : 0.5; }
    double framesPerBeat() const { return secPerBeat() * kSampleRate; }
    double beatAt(double frames) const;
    double beatPos() const { return beatAt(pos); }
    double frameOfBeat(double beat) const;
    double effectiveBpm() const { return track ? track->bpm * rate() : 0.0; }
    double timeSec() const { return pos / kSampleRate; }
    double remainingSec() const;
    void resetMixer();
};

struct TransitionRun {
    enum class State { Idle, Armed, Running };
    State state = State::Idle;
    TransitionDef def;
    int out = 0, in = 1;
    double elapsed = 0;       // frames since start
    double length = 0;        // frames
    double outBeatAtStart = 0;
    double outBpmAtStart = 0; // tempo glide: where the Tempo lane's 0 is
    double inTargetBpm = 0;   // tempo glide: where the Tempo lane's 1 is
    double framesPerBeat = 0; // of the transition clock
    bool inStarted = false;
    bool fxFired = false;
    bool synced = false;
    bool land = false;        // in deck's position is where the blend should END
    float rollBeats = 0;      // current loop-roll length (LoopRoll out effect)
    float progress = 0;
};

class Engine {
public:
    Engine();

    std::mutex mutex;
    Deck decks[2];
    int masterDeck = 0;        // tempo/phase reference for sync
    float crossfader = 0.0f;   // 0 = deck A, 1 = deck B
    int xfCurve = 0;           // 0 smooth, 1 dipless, 2 cut
    float masterVolume = 0.8f;
    bool quantize = true;
    dsp::Limiter limiter;
    float masterMeter[2] = {0, 0};
    float limiterReduction = 1.0f;
    TransitionRun run;
    std::string transitionNote;  // user-facing info about the last transition start

    // Clip preview voice.
    TrackPtr previewTrack;
    double previewPos = 0, previewEnd = 0;
    bool previewing = false;
    float previewVolume = 0.8f;

    // Audio thread entry point. Locks the mutex.
    void render(float* out, int frames, int channels);

    // ---- The functions below expect the caller to hold `mutex`. ----
    void loadTrack(int d, TrackPtr t);
    void ejectTrack(int d);
    void play(int d);
    void pause(int d);
    void togglePlay(int d) { decks[d].playing ? pause(d) : play(d); }
    void cueButton(int d);
    void seek(int d, double frames);
    void setHotCue(int d, int slot);
    void jumpHotCue(int d, int slot);
    void setLoop(int d, float beats);
    void exitLoop(int d);
    void setLoopRange(int d, double inFrame, double outFrame);
    void startMotion(int d, Motion m);
    // Grabs (held) / moves / releases the record. `target` is the frame under the hand.
    void scratch(int d, bool held, double target);
    // Tempo ratio (and half/double-time fold) that would match deck d to the other deck.
    bool computeSync(int d, double* ratio, double* fold) const;
    // Matches tempo to the other deck. Returns false (and sets `why`) if impossible.
    bool syncTempo(int d, std::string* why);
    // Shifts deck d onto the master's beat. barAlign also matches the position in the bar
    // (up to +/-2 beats); otherwise only the sub-beat phase is corrected.
    void alignPhase(int d, double masterBeat, double fold, bool barAlign = true);
    bool deckAudible(int d) const;
    int otherDeck(int d) const { return 1 - d; }
    float xfGain(int d) const;
    void resetAllFx();

    // Transitions. Returns false (and sets `why`) if the transition can't start.
    // With `land`, the incoming deck's current position is where it will be when the
    // blend finishes: it is rewound by the blend length before it starts playing.
    bool startTransition(const TransitionDef& def, int outDeck, std::string* why, bool land = false);
    // Frames the incoming deck will play during `def` (the landing rewind distance).
    double landingPreRoll(const TransitionDef& def, int inDeck) const;
    void cancelTransition();
    bool transitionBusy() const { return run.state != TransitionRun::State::Idle; }
    bool paramAutomated(int deck, int which) const;  // which: 0 vol,1 low,2 mid,3 high,4 filter,5 echo
    bool crossfaderAutomated() const;

    void startPreview(TrackPtr t, double from, double to);
    void stopPreview() { previewing = false; }

    // Copies the newest `frames` master frames (stereo interleaved) for visuals.
    void copyScope(float* dst, int frames);

    uint64_t clock = 0;  // frames rendered

    // Replays of sets recorded before 0.3 use the old transition sync rule so they sound as recorded.
    bool legacyTransitionSync = false;
    // Whether a transition will tempo-match the (not yet playing) incoming deck; fills the rate it will run at.
    bool planTransitionSync(const TransitionDef& def, int inDeck, double* ratio, double* fold) const;

    // Raw playhead jump (no quantize), e.g. for jogging a paused deck.
    void setPosition(int d, double frames);

    // ---- Set recording ----
    // Every outermost action call made from outside the audio thread is recorded.
    // Direct control changes (knobs, faders...) must be bracketed by
    // beginUserEdits()/endUserEdits() so they can be recorded as well.
    void beginUserEdits();
    void endUserEdits();
    void startRecording();  // snapshots the current state as the set's opening events
    SetRecording stopRecording(const std::string& name);
    bool isRecording() const { return recording_; }
    double recordingSec() const { return recording_ ? double(clock - recStart_) / kSampleRate : 0.0; }
    void recordNote(const std::string& text);

    // ---- Set replay (runs on the audio thread, sample accurate) ----
    // Load events must get `resolved` filled in before they fall due; replay
    // waits (stalls) at an unresolved load.
    void startReplay(SetRecording set);
    void stopReplay();
    bool replayActive() const { return replayActive_; }
    SetRecording replay;
    size_t replayNext = 0;
    uint64_t replayClock = 0;
    bool replayStalled = false;
    bool replayFinished = false;
    bool tookOver = false;                 // user touched a control during replay
    std::vector<std::string> replayNotes;  // Note events for the UI to show

private:
    static constexpr int kScopeFrames = 16384;

    // Recorder state.
    struct CallGuard;
    static constexpr int kParamSlots = 2 * int(SetParam::Count);
    struct ParamTrack {
        double snap = 0;  // value at beginUserEdits
    };
    bool recording_ = false;
    uint64_t recStart_ = 0;
    std::vector<SetEvent> rec_;
    ParamTrack params_[kParamSlots];
    double snapPos_[2] = {0, 0};
    bool snapPlaying_[2] = {false, false};
    bool snapHotCue_[2][kNumHotCues] = {};
    bool inAudio_ = false;
    int depth_ = 0;
    bool userTouched_ = false;
    bool replayActive_ = false;

    bool recordable() const { return recording_ && !inAudio_; }
    uint64_t recFrame() const { return clock - recStart_; }
    void recordEvent(SetEvent ev);
    void recordAction(SetAction a, int deck, int param = 0, double v = 0, double v2 = 0, bool flag = false);
    double getParam(int deck, SetParam p) const;
    void setParam(int deck, SetParam p, double v);
    void emitParams();
    void runReplay();
    void applyEvent(const SetEvent& ev);
    std::vector<float> scope_;
    size_t scopeWrite_ = 0;

    void renderBlock(float* out, int frames);
    void renderDeck(Deck& dk, float* mix, int frames, float xfTarget);
    void updateTransition(int frames);
    void beginTransitionNow();
    void updateRoll(float t);
    void startIncoming();
    void finishTransition();
    void applyLanes(float t);
    void followSync();
};
