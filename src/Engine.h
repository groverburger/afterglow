// Audio engine: two decks, a mixer, the transition automation runner and a
// clip preview voice. Everything is protected by `mutex`; the audio callback
// holds it while rendering and the UI holds it while touching engine state.
#pragma once
#include <mutex>
#include <string>
#include <vector>

#include "DSP.h"
#include "Track.h"
#include "Transitions.h"

constexpr int kNumHotCues = 4;

enum class Motion { Normal, Brake, Backspin };

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
    double framesPerBeat = 0; // of the transition clock
    bool inStarted = false;
    bool fxFired = false;
    bool synced = false;
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
    // Matches tempo to the other deck. Returns false (and sets `why`) if impossible.
    bool syncTempo(int d, std::string* why);
    void alignPhase(int d, double masterBeat, double fold);
    bool deckAudible(int d) const;
    int otherDeck(int d) const { return 1 - d; }
    float xfGain(int d) const;
    void resetAllFx();

    // Transitions. Returns false (and sets `why`) if the transition can't start.
    bool startTransition(const TransitionDef& def, int outDeck, std::string* why);
    void cancelTransition();
    bool transitionBusy() const { return run.state != TransitionRun::State::Idle; }
    bool paramAutomated(int deck, int which) const;  // which: 0 vol,1 low,2 mid,3 high,4 filter,5 echo
    bool crossfaderAutomated() const;

    void startPreview(TrackPtr t, double from, double to);
    void stopPreview() { previewing = false; }

    // Copies the newest `frames` master frames (stereo interleaved) for visuals.
    void copyScope(float* dst, int frames);

    uint64_t clock = 0;  // frames rendered

private:
    static constexpr int kScopeFrames = 16384;
    std::vector<float> scope_;
    size_t scopeWrite_ = 0;

    void renderBlock(float* out, int frames);
    void renderDeck(Deck& dk, float* mix, int frames, float xfTarget);
    void updateTransition(int frames);
    void beginTransitionNow();
    void startIncoming();
    void finishTransition();
    void applyLanes(float t);
    void followSync();
    bool computeSync(int d, double* ratio, double* fold) const;
};
