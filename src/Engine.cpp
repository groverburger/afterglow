#include "Engine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

constexpr int kSubBlock = 64;           // automation / quantize granularity in frames
constexpr double kBrakeSeconds = 1.4;
constexpr double kSpinWindup = 0.07;    // seconds to reach full reverse speed
constexpr double kSpinSpeed = -4.0;
constexpr double kMaxSyncChange = 0.5;  // never sync further than +/-50 %
constexpr double kTransitionSyncLimit = 0.12;  // tempo gap non-blending transitions still match
constexpr double kBigTempoChange = 0.16;
constexpr double kSpinUpSeconds = 0.45;  // turntable start: 0 to full speed
// Scratching: the playhead chases the hand with a short lag, and its speed is
// smoothed a little so 60 Hz mouse updates don't sound like a zipper.
constexpr double kScratchLag = 0.018;
constexpr double kScratchSmooth = 0.02;
constexpr double kMaxScratchRate = 12.0;

// Smallest tempo slider range that shows `tempo`.
float tempoRangeFor(double tempo) {
    const double a = std::fabs(tempo);
    return a <= 0.08 ? 0.08f : a <= 0.16 ? 0.16f : 0.5f;
}

// Frames the incoming deck advances between starting and the end of the blend.
double inPlayFrames(const TransitionDef& def, const Deck& in, double length, double inRate, double inStart, bool synced) {
    // A tempo glide runs on the outgoing track's beats; a synced deck plays a fixed number of its own beats.
    if (def.lane(Param::Tempo).enabled && synced) return (1.0 - inStart) * def.beats / in.syncFold * in.framesPerBeat();
    return (1.0 - inStart) * length * inRate;
}

double wrapBar(double x) {
    // Wrap into [-2, 2) beats.
    x = std::fmod(x + 2.0, 4.0);
    if (x < 0) x += 4.0;
    return x - 2.0;
}

float readFrame(const Track& t, double pos, int ch) {
    const long n = long(t.frames());
    long i = long(std::floor(pos));
    float f = float(pos - double(i));
    auto at = [&](long k) -> float {
        if (k < 0 || k >= n) return 0.0f;
        return t.samples[size_t(k) * 2 + size_t(ch)];
    };
    return dsp::hermite(at(i - 1), at(i), at(i + 1), at(i + 2), f);
}

}  // namespace

// Marks the outermost engine call made from outside the audio thread; only
// those are recorded (nested calls replay themselves).
struct Engine::CallGuard {
    Engine* e;
    bool outer;
    explicit CallGuard(Engine* en) : e(en), outer(en->depth_++ == 0 && !en->inAudio_) {
        if (outer && e->replayActive_) e->userTouched_ = true;
    }
    ~CallGuard() { --e->depth_; }
    CallGuard(const CallGuard&) = delete;
    CallGuard& operator=(const CallGuard&) = delete;
};

#define RECORD_CALL(...)                                 \
    CallGuard guard_(this);                               \
    if (guard_.outer && recordable()) recordAction(__VA_ARGS__)

// ---------------------------------------------------------------- Deck ----

double Deck::beatAt(double frames) const {
    if (!track) return 0.0;
    return (frames / kSampleRate - track->firstBeatSec) / track->secPerBeat();
}

double Deck::frameOfBeat(double beat) const {
    if (!track) return 0.0;
    return (track->firstBeatSec + beat * track->secPerBeat()) * kSampleRate;
}

double Deck::remainingSec() const {
    if (!track) return 0.0;
    double r = std::max(0.05, rate());
    return std::max(0.0, (double(track->frames()) - pos) / kSampleRate / r);
}

void Deck::resetMixer() {
    volume = 1.0f;
    eq[0] = eq[1] = eq[2] = 0.5f;
    filter = 0.5f;
    echo = 0.0f;
}

// -------------------------------------------------------------- Engine ----

Engine::Engine() : scope_(size_t(kScopeFrames) * 2, 0.0f) {
    decks[0].index = 0;
    decks[1].index = 1;
}

void Engine::render(float* out, int frames, int channels) {
    std::lock_guard<std::mutex> lock(mutex);
    inAudio_ = true;
    float block[kSubBlock * 2];
    int done = 0;
    while (done < frames) {
        int n = std::min(kSubBlock, frames - done);
        renderBlock(block, n);
        for (int i = 0; i < n; ++i) {
            float l = block[i * 2], r = block[i * 2 + 1];
            float* o = out + size_t(done + i) * size_t(channels);
            if (channels == 1) {
                o[0] = 0.5f * (l + r);
            } else {
                o[0] = l;
                o[1] = r;
                for (int c = 2; c < channels; ++c) o[c] = 0.0f;
            }
        }
        done += n;
    }
    inAudio_ = false;
}

void Engine::renderBlock(float* out, int frames) {
    std::memset(out, 0, sizeof(float) * size_t(frames) * 2);
    if (replayActive_) {
        runReplay();
        // Waiting for a track: hold everything still so the set stays in time.
        if (replayStalled) return;
    }
    updateTransition(frames);
    followSync();
    for (int d = 0; d < 2; ++d) renderDeck(decks[d], out, frames, xfGain(d));

    if (previewing && previewTrack) {
        const Track& t = *previewTrack;
        for (int i = 0; i < frames; ++i) {
            if (previewPos >= previewEnd) {
                previewing = false;
                break;
            }
            // Short fade-out so previews never end with a click.
            float fade = float(std::min(1.0, (previewEnd - previewPos) / (0.01 * kSampleRate)));
            float g = previewVolume * t.autoGain * fade;
            out[i * 2] += readFrame(t, previewPos, 0) * g;
            out[i * 2 + 1] += readFrame(t, previewPos, 1) * g;
            previewPos += 1.0;
        }
    }

    float peakL = 0, peakR = 0, minGain = 1.0f;
    for (int i = 0; i < frames; ++i) {
        float l = out[i * 2] * masterVolume, r = out[i * 2 + 1] * masterVolume;
        limiter.process(l, r);
        minGain = std::min(minGain, limiter.gainReduction);
        out[i * 2] = l;
        out[i * 2 + 1] = r;
        peakL = std::max(peakL, std::fabs(l));
        peakR = std::max(peakR, std::fabs(r));
        scope_[scopeWrite_ * 2] = l;
        scope_[scopeWrite_ * 2 + 1] = r;
        scopeWrite_ = (scopeWrite_ + 1) % kScopeFrames;
    }
    masterMeter[0] = std::max(masterMeter[0], peakL);
    masterMeter[1] = std::max(masterMeter[1], peakR);
    limiterReduction = std::min(limiterReduction, minGain);
    clock += uint64_t(frames);
    if (replayActive_ && !replayStalled) {
        replayClock += uint64_t(frames);
        if (replayNext >= replay.events.size() && replayClock >= replay.lengthFrames) {
            replayActive_ = false;
            legacyTransitionSync = false;
            replayFinished = true;
        }
    }
}

void Engine::renderDeck(Deck& dk, float* mix, int frames, float xfg) {
    if (!dk.track) {
        dk.gainSm.snap(0);
        return;
    }
    const Track& t = *dk.track;
    const double lastFrame = double(t.frames()) - 1.0;
    dk.eqDsp.set(dk.eq[0], dk.eq[1], dk.eq[2]);
    dk.filterDsp.set(dk.filter);
    const bool eqFlat = dk.eqDsp.flat();
    const bool filterOff = dk.filterDsp.bypassed();
    const float trimGain = dsp::dbToGain((dk.trim - 0.5f) * 24.0f) * t.autoGain;
    const float targetGain = dk.volume * xfg;
    if (dk.motion == Motion::Normal)
        dk.echoDsp.delayFrames = float(0.75 * dk.framesPerBeat() / std::max(0.25, dk.rate()));

    float peak = 0;
    for (int i = 0; i < frames; ++i) {
        float l = 0, r = 0;
        if (dk.scratching) {
            double want = std::clamp((dk.scratchTarget - dk.pos) / (kScratchLag * kSampleRate), -kMaxScratchRate, kMaxScratchRate);
            dk.scratchRate += (want - dk.scratchRate) * kScratchSmooth;
            if (dk.pos >= 0 && dk.pos < lastFrame) {
                l = readFrame(t, dk.pos, 0);
                r = readFrame(t, dk.pos, 1);
            }
            dk.pos = std::clamp(dk.pos + dk.scratchRate, -10.0 * kSampleRate, lastFrame);
            if (dk.slip && dk.playing) dk.slipPos += dk.rate();
        } else if (dk.playing) {
            double rr = dk.rate();
            if (dk.motion == Motion::SpinUp) {
                dk.motionTime += 1.0 / kSampleRate;
                double a = dk.motionTime / kSpinUpSeconds;
                rr *= std::min(1.0, a);
                if (a >= 1.0) dk.motion = Motion::Normal;
            } else if (dk.motion == Motion::Brake) {
                dk.motionTime += 1.0 / kSampleRate;
                double m = 1.0 - dk.motionTime / kBrakeSeconds;
                rr = dk.motionStartRate * std::max(0.0, m);
                if (m <= 0) {
                    dk.playing = false;
                    dk.motion = Motion::Normal;
                }
            } else if (dk.motion == Motion::Backspin) {
                dk.motionTime += 1.0 / kSampleRate;
                double tm = dk.motionTime;
                if (tm < kSpinWindup) {
                    double a = tm / kSpinWindup;
                    rr = dk.motionStartRate * (1 - a) + kSpinSpeed * a;
                } else {
                    rr = kSpinSpeed * std::exp(-(tm - kSpinWindup) * 2.6);
                }
                if (tm > kSpinWindup && std::fabs(rr) < 0.03) {
                    dk.playing = false;
                    dk.motion = Motion::Normal;
                }
            }
            if (dk.pos >= 0 && dk.pos < lastFrame) {
                l = readFrame(t, dk.pos, 0);
                r = readFrame(t, dk.pos, 1);
            }
            dk.pos += rr;
            if (dk.loopActive && rr > 0 && dk.pos >= dk.loopOut && dk.loopOut > dk.loopIn)
                dk.pos -= dk.loopOut - dk.loopIn;
            if (dk.pos >= lastFrame) {
                dk.pos = lastFrame;
                dk.playing = false;
                dk.motion = Motion::Normal;
            }
            if (dk.pos < 0 && rr < 0) {
                dk.pos = 0;
                dk.playing = false;
                dk.motion = Motion::Normal;
            }
        }
        l *= trimGain;
        r *= trimGain;
        if (!eqFlat) dk.eqDsp.process(l, r);
        if (!filterOff) {
            l = dk.filterDsp.process(l, 0);
            r = dk.filterDsp.process(r, 1);
        }
        float g = dk.gainSm.next(targetGain);
        l *= g;
        r *= g;
        // Echo return bypasses the faders so tails keep ringing after a cut.
        float es = dk.echoSm.next(dk.echo, 0.001f);
        float el, er;
        dk.echoDsp.process(l * es, r * es, el, er);
        l += el;
        r += er;
        mix[i * 2] += l;
        mix[i * 2 + 1] += r;
        peak = std::max(peak, std::max(std::fabs(l), std::fabs(r)));
    }
    dk.meter = std::max(dk.meter, peak);
}

float Engine::xfGain(int d) const {
    // p = 0 means the crossfader is fully on this deck's side.
    float p = d == 0 ? crossfader : 1.0f - crossfader;
    p = std::clamp(p, 0.0f, 1.0f);
    switch (xfCurve) {
        case 1: return p <= 0.5f ? 1.0f : std::cos((p - 0.5f) * dsp::kPi);
        case 2: return std::clamp((0.98f - p) / 0.06f, 0.0f, 1.0f);
        default: return std::cos(p * dsp::kPi * 0.5f);
    }
}

bool Engine::deckAudible(int d) const {
    const Deck& dk = decks[d];
    return dk.loaded() && dk.playing && dk.volume > 0.05f && xfGain(d) > 0.05f && masterVolume > 0.01f;
}

// ----------------------------------------------------------- transport ----

void Engine::loadTrack(int d, TrackPtr t) {
    CallGuard guard_(this);
    if (guard_.outer && recordable()) {
        SetEvent ev;
        ev.action = SetAction::Load;
        ev.deck = d;
        if (t) ev.track = trackRefFor(*t);
        recordEvent(std::move(ev));
    }
    Deck& dk = decks[d];
    if (run.state != TransitionRun::State::Idle && (run.in == d || run.out == d)) run.state = TransitionRun::State::Idle;
    dk.track = std::move(t);
    dk.playing = false;
    dk.motion = Motion::Normal;
    dk.loopActive = false;
    dk.nudge = 0;
    dk.tempo = 0;
    dk.tempoRange = 0.08f;
    dk.syncFold = 1.0;
    dk.scratching = false;
    for (int i = 0; i < kNumHotCues; ++i) dk.hotCueSet[i] = false;
    // Start at the first downbeat so "play" always lands on the grid.
    dk.cuePoint = dk.track ? std::max(0.0, dk.track->firstBeatSec * kSampleRate) : 0.0;
    dk.pos = dk.cuePoint;
    dk.eqDsp = dsp::ThreeBandEq{};
    dk.filterDsp.reset();
    dk.gainSm.snap(0);
}

void Engine::ejectTrack(int d) {
    RECORD_CALL(SetAction::Load, d);
    loadTrack(d, nullptr);
}

void Engine::play(int d) {
    RECORD_CALL(SetAction::Play, d);
    Deck& dk = decks[d];
    if (!dk.loaded()) return;
    if (dk.pos >= double(dk.track->frames()) - 2) dk.pos = dk.cuePoint;  // at the end: restart
    const int o = otherDeck(d);
    if (decks[o].playing && decks[o].loaded()) {
        masterDeck = o;
        if (dk.sync) {
            std::string why;
            if (syncTempo(d, &why) && quantize) alignPhase(d, decks[o].beatPos(), dk.syncFold);
        }
    } else {
        masterDeck = d;
    }
    dk.motion = Motion::Normal;
    dk.playing = true;
}

void Engine::pause(int d) {
    RECORD_CALL(SetAction::Pause, d);
    decks[d].playing = false;
    decks[d].motion = Motion::Normal;
}

void Engine::cueButton(int d) {
    RECORD_CALL(SetAction::CueButton, d);
    Deck& dk = decks[d];
    if (!dk.loaded()) return;
    if (dk.playing) {
        dk.pos = dk.cuePoint;
        dk.playing = false;
        dk.motion = Motion::Normal;
        return;
    }
    double p = dk.pos;
    if (quantize) p = dk.frameOfBeat(std::round(dk.beatAt(p)));
    dk.cuePoint = std::clamp(p, 0.0, double(dk.track->frames()) - 1);
    dk.pos = dk.cuePoint;
}

void Engine::seek(int d, double frames) {
    RECORD_CALL(SetAction::Seek, d, 0, frames);
    Deck& dk = decks[d];
    if (!dk.loaded()) return;
    const double last = double(dk.track->frames()) - 1;
    frames = std::clamp(frames, 0.0, last);
    if (dk.playing && quantize) {
        // Keep the current beat phase so a synced mix stays in time.
        double cur = dk.beatPos();
        double frac = cur - std::floor(cur);
        double target = dk.beatAt(frames);
        double snapped = std::round(target - frac) + frac;
        frames = std::clamp(dk.frameOfBeat(snapped), 0.0, last);
    }
    if (dk.loopActive && (frames < dk.loopIn || frames >= dk.loopOut)) dk.loopActive = false;
    dk.pos = frames;
}

void Engine::setHotCue(int d, int slot) {
    RECORD_CALL(SetAction::HotCueSet, d, slot);
    Deck& dk = decks[d];
    if (!dk.loaded()) return;
    double p = dk.pos;
    if (quantize) p = std::max(0.0, dk.frameOfBeat(std::round(dk.beatAt(p))));
    dk.hotCues[slot] = p;
    dk.hotCueSet[slot] = true;
}

void Engine::jumpHotCue(int d, int slot) {
    RECORD_CALL(SetAction::HotCueJump, d, slot);
    Deck& dk = decks[d];
    if (!dk.loaded()) return;
    if (!dk.hotCueSet[slot]) {
        setHotCue(d, slot);
        return;
    }
    double target = dk.hotCues[slot];
    if (dk.playing && quantize) {
        double cur = dk.beatPos();
        target += (cur - std::floor(cur)) * dk.framesPerBeat();
    }
    if (dk.loopActive && (target < dk.loopIn || target >= dk.loopOut)) dk.loopActive = false;
    dk.pos = target;
}

void Engine::setLoop(int d, float beats) {
    RECORD_CALL(SetAction::Loop, d, 0, beats);
    Deck& dk = decks[d];
    if (!dk.loaded()) return;
    if (dk.loopActive && dk.loopBeats == beats) {
        exitLoop(d);
        return;
    }
    if (!dk.loopActive) {
        double b = dk.beatPos();
        double start = quantize ? std::floor(b + 1e-6) : b;
        dk.loopIn = std::max(0.0, dk.frameOfBeat(start));
    }
    dk.loopOut = dk.loopIn + beats * dk.framesPerBeat();
    dk.loopBeats = beats;
    dk.loopActive = true;
    if (dk.pos >= dk.loopOut) dk.pos = dk.loopIn + std::fmod(dk.pos - dk.loopIn, dk.loopOut - dk.loopIn);
}

void Engine::exitLoop(int d) {
    RECORD_CALL(SetAction::LoopExit, d);
    decks[d].loopActive = false;
}

void Engine::setLoopRange(int d, double inFrame, double outFrame) {
    RECORD_CALL(SetAction::LoopRange, d, 0, inFrame, outFrame);
    Deck& dk = decks[d];
    if (!dk.loaded() || outFrame <= inFrame) return;
    dk.loopIn = inFrame;
    dk.loopOut = outFrame;
    dk.loopBeats = float((outFrame - inFrame) / dk.framesPerBeat());
    dk.loopActive = true;
    dk.pos = inFrame;
    dk.cuePoint = inFrame;
}

void Engine::startMotion(int d, Motion m) {
    RECORD_CALL(SetAction::Motion, d, int(m));
    Deck& dk = decks[d];
    if (!dk.playing || dk.motion != Motion::Normal) return;
    dk.motion = m;
    dk.motionTime = 0;
    dk.motionStartRate = dk.rate();
    dk.loopActive = false;
}

void Engine::scratch(int d, bool held, double target) {
    RECORD_CALL(SetAction::Scratch, d, 0, target, 0, held);
    Deck& dk = decks[d];
    if (!dk.loaded()) return;
    if (held) {
        if (!dk.scratching) {
            dk.scratching = true;
            dk.scratchRate = dk.playing ? dk.rate() : 0.0;  // the record was moving under the hand
            dk.slipPos = dk.pos;
            dk.motion = Motion::Normal;
            dk.loopActive = false;
        }
        dk.scratchTarget = target;
        return;
    }
    if (!dk.scratching) return;
    dk.scratching = false;
    if (dk.slip && dk.playing) dk.pos = std::min(dk.slipPos, double(dk.track->frames()) - 1);
}

bool Engine::computeSync(int d, double* ratio, double* fold) const {
    const Deck& dk = decks[d];
    const Deck& o = decks[otherDeck(d)];
    if (!dk.loaded() || !o.loaded()) return false;
    const double masterBpm = o.track->bpm * (1.0 + o.tempo);
    double best = 1.0, bestErr = 1e9;
    for (double f : {0.5, 1.0, 2.0}) {
        double r = masterBpm / (dk.track->bpm * f);
        double err = std::fabs(std::log(r));
        if (err < bestErr) {
            bestErr = err;
            best = f;
        }
    }
    *fold = best;
    *ratio = masterBpm / (dk.track->bpm * best);
    return true;
}

bool Engine::syncTempo(int d, std::string* why) {
    RECORD_CALL(SetAction::SyncTempo, d);
    Deck& dk = decks[d];
    double ratio, fold;
    if (!computeSync(d, &ratio, &fold)) {
        if (why) *why = "Both decks need a track to sync";
        return false;
    }
    double change = ratio - 1.0;
    if (std::fabs(change) > kMaxSyncChange) {
        if (why) *why = "Tempos are too far apart to sync";
        return false;
    }
    dk.tempo = change;
    dk.syncFold = fold;
    for (float r : {0.08f, 0.16f, 0.5f}) {
        if (std::fabs(change) <= r) {
            dk.tempoRange = std::max(dk.tempoRange, r);
            break;
        }
    }
    if (why) why->clear();
    if (std::fabs(change) > 0.16 && why) *why = "Large tempo change - this pairing may sound unusual";
    return true;
}

void Engine::alignPhase(int d, double masterBeat, double fold, bool barAlign) {
    RECORD_CALL(SetAction::AlignPhase, d, 0, masterBeat, fold, barAlign);
    Deck& dk = decks[d];
    if (!dk.loaded()) return;
    double b = dk.beatPos();
    double raw = masterBeat - b * fold;
    double diff = barAlign ? wrapBar(raw) : raw - std::round(raw);
    double nb = b + diff / fold;
    dk.pos = dk.frameOfBeat(nb);
    if (dk.pos < -dk.framesPerBeat() * 4) dk.pos += 4 * dk.framesPerBeat() / fold;
}

void Engine::followSync() {
    int m = masterDeck;
    if (!decks[m].playing && decks[1 - m].playing) masterDeck = m = 1 - m;
    const int s = 1 - m;
    Deck& slave = decks[s];
    if (!slave.sync || !slave.loaded() || !decks[m].loaded() || !decks[m].playing) return;
    if (run.state == TransitionRun::State::Running && run.in == s && !run.synced) return;
    double ratio, fold;
    if (!computeSync(s, &ratio, &fold)) return;
    if (!slave.playing) {
        // Pre-match a cued track so its BPM display already agrees.
        if (std::fabs(ratio - 1.0) <= 0.16) {
            slave.tempo = ratio - 1.0;
            slave.syncFold = fold;
            slave.tempoRange = std::fabs(ratio - 1.0) <= 0.08 ? std::max(slave.tempoRange, 0.08f) : 0.16f;
        }
        return;
    }
    if (fold == slave.syncFold && std::fabs(ratio - 1.0) <= kMaxSyncChange) slave.tempo = ratio - 1.0;
}

void Engine::resetAllFx() {
    RECORD_CALL(SetAction::ResetFx, 0);
    for (auto& dk : decks) {
        dk.eq[0] = dk.eq[1] = dk.eq[2] = 0.5f;
        dk.filter = 0.5f;
        dk.echo = 0.0f;
        dk.echoDsp.clear();
        dk.motion = Motion::Normal;
        dk.nudge = 0;
    }
}

// ---------------------------------------------------------- transitions ----

bool Engine::startTransition(const TransitionDef& def, int outDeck, std::string* why, bool land) {
    CallGuard guard_(this);
    if (transitionBusy()) {
        if (why) *why = "A transition is already running";
        return false;
    }
    const int in = otherDeck(outDeck);
    if (!decks[in].loaded()) {
        if (why) *why = std::string("Load a track into deck ") + char('A' + in) + " first";
        return false;
    }
    run = TransitionRun{};
    run.def = def;
    run.out = outDeck;
    run.in = in;
    run.land = land;
    run.state = TransitionRun::State::Armed;
    transitionNote.clear();
    if (guard_.outer && recordable()) {
        SetEvent ev;
        ev.action = SetAction::Transition;
        ev.deck = outDeck;
        ev.flag = land;
        ev.text = def.name;
        ev.def = def;
        recordEvent(std::move(ev));
    }
    return true;
}

double Engine::landingPreRoll(const TransitionDef& def, int inDeck) const {
    const Deck& in = decks[inDeck];
    const Deck& o = decks[otherDeck(inDeck)];
    if (!in.loaded()) return 0.0;
    const bool outRunning = o.loaded() && o.playing;
    double fpb = outRunning ? o.framesPerBeat() / std::max(0.25, o.rate()) : in.framesPerBeat() / std::max(0.25, in.rate());
    double length = def.beats * fpb;
    double inStart = outRunning ? def.inStartAt : 0.0;
    // Predict the tempo the incoming deck will run at once the transition syncs it.
    double inRate = in.rate();
    double ratio, fold;
    if (outRunning && !in.playing && planTransitionSync(def, inDeck, &ratio, &fold)) {
        if (def.lane(Param::Tempo).enabled) return (1.0 - inStart) * def.beats / fold * in.framesPerBeat();
        inRate = ratio;
    }
    return inPlayFrames(def, in, length, inRate, inStart, false);
}

bool Engine::planTransitionSync(const TransitionDef& def, int inDeck, double* ratio, double* fold) const {
    const Deck& in = decks[inDeck];
    if (!computeSync(inDeck, ratio, fold)) return false;
    const double change = std::fabs(*ratio - 1.0);
    if (legacyTransitionSync) return change <= kTransitionSyncLimit;
    if (!in.sync || change > kMaxSyncChange) return false;
    // Already matched by hand or with SYNC: respect it, however big the change.
    const bool matched = std::fabs((1.0 + in.tempo) / *ratio - 1.0) < 0.002;
    return matched || change <= kTransitionSyncLimit || def.overlaps();
}

void Engine::cancelTransition() {
    RECORD_CALL(SetAction::CancelTransition, 0);
    run.state = TransitionRun::State::Idle;
}

void Engine::updateTransition(int frames) {
    if (run.state == TransitionRun::State::Armed) {
        Deck& o = decks[run.out];
        bool now = !quantize || !o.loaded() || !o.playing || o.motion != Motion::Normal;
        if (!now) {
            // Wait for the next bar line of the outgoing track.
            double b0 = o.beatPos();
            double b1 = o.beatAt(o.pos + frames * o.rate());
            now = std::floor(b0 / 4.0) != std::floor(b1 / 4.0) || std::fabs(b0 / 4.0 - std::round(b0 / 4.0)) < 1e-4;
        }
        if (now) beginTransitionNow();
    }
    if (run.state != TransitionRun::State::Running) return;

    float t = run.length > 0 ? float(std::min(1.0, run.elapsed / run.length)) : 1.0f;
    if (!run.inStarted && t >= run.def.inStartAt) startIncoming();
    if (!run.fxFired && run.def.outEffect != OutEffect::None && t >= run.def.outEffectAt) {
        if (run.def.outEffect != OutEffect::LoopRoll)
            startMotion(run.out, run.def.outEffect == OutEffect::Brake ? Motion::Brake : Motion::Backspin);
        run.fxFired = true;
    }
    if (run.fxFired && run.def.outEffect == OutEffect::LoopRoll) updateRoll(t);
    applyLanes(t);
    run.progress = t;
    if (t >= 1.0f) {
        finishTransition();
        return;
    }
    // During a tempo glide the clock follows the outgoing track's beats, so the blend still ends on a bar.
    const Deck& o = decks[run.out];
    if (run.outBpmAtStart > 0 && o.loaded() && o.playing && o.motion == Motion::Normal)
        run.elapsed += frames * (o.track->bpm * (1.0 + o.tempo)) / run.outBpmAtStart;
    else
        run.elapsed += frames;
}

void Engine::updateRoll(float t) {
    // 1 beat, 1/2, 1/4, 1/8: four equal stages from the effect point to the end.
    Deck& o = decks[run.out];
    if (!o.loaded() || !o.playing || o.scratching) return;
    const float span = 1.0f - run.def.outEffectAt;
    const float u = span > 1e-4f ? (t - run.def.outEffectAt) / span : 1.0f;
    const float beats = 1.0f / float(1 << std::clamp(int(u * 4.0f), 0, 3));
    if (beats == run.rollBeats) return;
    if (run.rollBeats == 0) o.loopIn = std::max(0.0, o.frameOfBeat(std::floor(o.beatPos() + 1e-6)));
    o.loopOut = o.loopIn + beats * o.framesPerBeat();
    o.loopBeats = beats;
    o.loopActive = true;
    if (o.pos >= o.loopOut) o.pos = o.loopIn + std::fmod(o.pos - o.loopIn, o.loopOut - o.loopIn);
    run.rollBeats = beats;
}

void Engine::beginTransitionNow() {
    Deck& o = decks[run.out];
    Deck& in = decks[run.in];
    const bool outRunning = o.loaded() && o.playing;
    run.state = TransitionRun::State::Running;
    run.elapsed = 0;
    run.framesPerBeat = outRunning ? o.framesPerBeat() / std::max(0.25, o.rate())
                                   : in.framesPerBeat() / std::max(0.25, in.rate());
    run.length = run.def.beats * run.framesPerBeat;
    run.outBeatAtStart = outRunning ? std::round(o.beatPos()) : 0.0;
    run.synced = false;
    run.rollBeats = 0;
    run.outBpmAtStart = run.inTargetBpm = 0;
    if (!outRunning) {
        // Nothing to mix out of: just bring the new track in right away.
        run.def.inStartAt = 0.0f;
        run.def.outEffect = OutEffect::None;
    }

    if (!in.playing) {
        // Fresh channel for the incoming track (lanes override where they apply).
        in.resetMixer();
        in.motion = Motion::Normal;
        const bool glide = outRunning && run.def.lane(Param::Tempo).enabled;
        double inRate = in.rate();
        if (outRunning) {
            double ratio, fold;
            const bool matched = computeSync(run.in, &ratio, &fold) && std::fabs((1.0 + in.tempo) / ratio - 1.0) < 0.002;
            if (planTransitionSync(run.def, run.in, &ratio, &fold)) {
                in.tempo = ratio - 1.0;
                in.syncFold = fold;
                in.tempoRange = std::max(in.tempoRange, tempoRangeFor(in.tempo));
                inRate = ratio;
                run.synced = true;
                if (!legacyTransitionSync && !matched && !glide && std::fabs(in.tempo) > kBigTempoChange) {
                    char buf[160];
                    std::snprintf(buf, sizeof(buf), "Deck %c is %s %.0f%% to beat-match. For big tempo jumps, try the Tempo Ramp transition",
                                  'A' + run.in, in.tempo > 0 ? "sped up" : "slowed down", std::fabs(in.tempo) * 100.0);
                    transitionNote = buf;
                }
            } else if (legacyTransitionSync) {
                transitionNote = "Tempos too different to beatmatch - mixing without sync";
            } else if (in.sync && run.def.overlaps()) {
                transitionNote = "Tempos are too far apart to beat-match - try Echo Out, Backspin or Power Swap";
            }
            if (glide) {
                run.outBpmAtStart = o.track->bpm * (1.0 + o.tempo);
                double r, f;
                run.inTargetBpm = computeSync(run.in, &r, &f) ? in.track->bpm * (run.synced ? in.syncFold : f) : 0.0;
            }
        }
        if (run.land) {
            // Rewind so the cued spot arrives exactly as the blend completes.
            in.pos -= inPlayFrames(run.def, in, run.length, inRate, run.def.inStartAt, run.synced);
            in.loopActive = false;
        }
    } else {
        run.synced = in.sync;
        if (outRunning && run.def.lane(Param::Tempo).enabled && in.loaded()) {
            run.outBpmAtStart = o.track->bpm * (1.0 + o.tempo);
            run.inTargetBpm = in.track->bpm * (run.synced ? in.syncFold : 1.0);
        }
    }
    applyLanes(0.0f);
}

void Engine::startIncoming() {
    Deck& in = decks[run.in];
    Deck& o = decks[run.out];
    run.inStarted = true;
    if (in.playing) return;
    if (run.synced) {
        // A braked, spun or loop-rolled out deck is off its grid: use the transition's own clock.
        const bool rolling = run.def.outEffect == OutEffect::LoopRoll && run.fxFired;
        double masterBeat = (o.playing && o.motion == Motion::Normal && !rolling)
                                ? o.beatPos()
                                : run.outBeatAtStart + run.elapsed / run.framesPerBeat;
        alignPhase(run.in, masterBeat, in.syncFold, !run.land);
    }
    in.motion = Motion::Normal;
    if (run.def.inEffect == InEffect::SpinUp) {
        // It starts slow: begin a little ahead so it's on the beat once up to speed.
        in.motion = Motion::SpinUp;
        in.motionTime = 0;
        in.pos += 0.5 * kSpinUpSeconds * kSampleRate * in.rate();
    }
    in.playing = true;
}

void Engine::finishTransition() {
    applyLanes(1.0f);
    Deck& o = decks[run.out];
    o.playing = false;
    o.motion = Motion::Normal;
    o.loopActive = false;
    o.resetMixer();  // echo send off; the echo tail keeps ringing out
    if (!run.def.lane(Param::Crossfader).enabled) crossfader = run.in == 1 ? 1.0f : 0.0f;
    masterDeck = run.in;
    run.state = TransitionRun::State::Idle;
    run.progress = 1.0f;
}

void Engine::applyLanes(float t) {
    Deck& o = decks[run.out];
    Deck& in = decks[run.in];
    for (int p = 0; p < kNumParams; ++p) {
        const Lane& lane = run.def.lanes[size_t(p)];
        if (!lane.enabled || lane.keys.empty()) continue;
        float v = std::clamp(lane.eval(t), 0.0f, 1.0f);
        switch (Param(p)) {
            case Param::Crossfader: crossfader = run.out == 0 ? v : 1.0f - v; break;
            case Param::OutVolume: o.volume = v; break;
            case Param::InVolume: in.volume = v; break;
            case Param::OutLow: o.eq[0] = v; break;
            case Param::InLow: in.eq[0] = v; break;
            case Param::OutMid: o.eq[1] = v; break;
            case Param::InMid: in.eq[1] = v; break;
            case Param::OutHigh: o.eq[2] = v; break;
            case Param::InHigh: in.eq[2] = v; break;
            case Param::OutFilter: o.filter = v; break;
            case Param::InFilter: in.filter = v; break;
            case Param::OutEcho: o.echo = v; break;
            case Param::InEcho: in.echo = v; break;
            case Param::Tempo:
                if (run.outBpmAtStart > 0 && run.inTargetBpm > 0 && o.loaded()) {
                    const double bpm = run.outBpmAtStart + (run.inTargetBpm - run.outBpmAtStart) * double(v);
                    o.tempo = bpm / o.track->bpm - 1.0;
                    o.tempoRange = std::max(o.tempoRange, tempoRangeFor(o.tempo));
                    if (run.synced && in.loaded()) in.tempo = bpm / (in.track->bpm * in.syncFold) - 1.0;
                }
                break;
            case Param::Count: break;
        }
    }
}

bool Engine::paramAutomated(int deck, int which) const {
    if (run.state != TransitionRun::State::Running) return false;
    static const Param outMap[6] = {Param::OutVolume, Param::OutLow, Param::OutMid,
                                    Param::OutHigh, Param::OutFilter, Param::OutEcho};
    static const Param inMap[6] = {Param::InVolume, Param::InLow, Param::InMid,
                                   Param::InHigh, Param::InFilter, Param::InEcho};
    if (deck == run.out) return run.def.lane(outMap[which]).enabled;
    if (deck == run.in) return run.def.lane(inMap[which]).enabled;
    return false;
}

bool Engine::crossfaderAutomated() const {
    return run.state == TransitionRun::State::Running && run.def.lane(Param::Crossfader).enabled;
}

// ------------------------------------------------------------- preview ----

void Engine::startPreview(TrackPtr t, double from, double to) {
    if (!t) return;
    previewTrack = std::move(t);
    previewPos = std::max(0.0, from);
    previewEnd = std::min(to, double(previewTrack->frames()) - 2);
    previewing = previewEnd > previewPos;
}

void Engine::copyScope(float* dst, int frames) {
    frames = std::min(frames, kScopeFrames);
    size_t start = (scopeWrite_ + kScopeFrames - size_t(frames)) % kScopeFrames;
    for (int i = 0; i < frames; ++i) {
        size_t k = (start + size_t(i)) % kScopeFrames;
        dst[i * 2] = scope_[k * 2];
        dst[i * 2 + 1] = scope_[k * 2 + 1];
    }
}

// ------------------------------------------------------------ recording ----

void Engine::setPosition(int d, double frames) {
    RECORD_CALL(SetAction::SetPos, d, 0, frames);
    decks[d].pos = frames;
}

void Engine::recordEvent(SetEvent ev) {
    ev.frame = recFrame();
    rec_.push_back(std::move(ev));
}

void Engine::recordAction(SetAction a, int deck, int param, double v, double v2, bool flag) {
    SetEvent ev;
    ev.action = a;
    ev.deck = deck;
    ev.param = param;
    ev.value = v;
    ev.value2 = v2;
    ev.flag = flag;
    recordEvent(std::move(ev));
}

void Engine::recordNote(const std::string& text) {
    if (!recording_) return;
    SetEvent ev;
    ev.action = SetAction::Note;
    ev.text = text;
    recordEvent(std::move(ev));
}

static bool isGlobalParam(SetParam p) { return int(p) >= int(SetParam::Crossfader); }

double Engine::getParam(int deck, SetParam p) const {
    const Deck& dk = decks[deck];
    switch (p) {
        case SetParam::Volume: return dk.volume;
        case SetParam::Trim: return dk.trim;
        case SetParam::EqLow: return dk.eq[0];
        case SetParam::EqMid: return dk.eq[1];
        case SetParam::EqHigh: return dk.eq[2];
        case SetParam::Filter: return dk.filter;
        case SetParam::Echo: return dk.echo;
        case SetParam::Tempo: return dk.tempo;
        case SetParam::TempoRange: return dk.tempoRange;
        case SetParam::Sync: return dk.sync ? 1.0 : 0.0;
        case SetParam::Nudge: return dk.nudge;
        case SetParam::CuePoint: return dk.cuePoint;
        case SetParam::Slip: return dk.slip ? 1.0 : 0.0;
        case SetParam::Crossfader: return crossfader;
        case SetParam::XfCurve: return xfCurve;
        case SetParam::MasterVolume: return masterVolume;
        case SetParam::Quantize: return quantize ? 1.0 : 0.0;
        case SetParam::Count: break;
    }
    return 0.0;
}

void Engine::setParam(int deck, SetParam p, double v) {
    Deck& dk = decks[deck];
    switch (p) {
        case SetParam::Volume: dk.volume = float(v); break;
        case SetParam::Trim: dk.trim = float(v); break;
        case SetParam::EqLow: dk.eq[0] = float(v); break;
        case SetParam::EqMid: dk.eq[1] = float(v); break;
        case SetParam::EqHigh: dk.eq[2] = float(v); break;
        case SetParam::Filter: dk.filter = float(v); break;
        case SetParam::Echo: dk.echo = float(v); break;
        case SetParam::Tempo: dk.tempo = v; break;
        case SetParam::TempoRange: dk.tempoRange = float(v); break;
        case SetParam::Sync: dk.sync = v != 0.0; break;
        case SetParam::Nudge: dk.nudge = v; break;
        case SetParam::CuePoint: dk.cuePoint = v; break;
        case SetParam::Slip: dk.slip = v != 0.0; break;
        case SetParam::Crossfader: crossfader = float(v); break;
        case SetParam::XfCurve: xfCurve = int(v); break;
        case SetParam::MasterVolume: masterVolume = float(v); break;
        case SetParam::Quantize: quantize = v != 0.0; break;
        case SetParam::Count: break;
    }
}

void Engine::beginUserEdits() {
    for (int d = 0; d < 2; ++d) {
        for (int p = 0; p < int(SetParam::Count); ++p)
            params_[d * int(SetParam::Count) + p].snap = getParam(d, SetParam(p));
        snapPos_[d] = decks[d].pos;
        snapPlaying_[d] = decks[d].playing;
        for (int i = 0; i < kNumHotCues; ++i) snapHotCue_[d][i] = decks[d].hotCueSet[i];
    }
    userTouched_ = false;
}

void Engine::endUserEdits() {
    bool changed = false;
    for (int d = 0; d < 2; ++d) {
        Deck& dk = decks[d];
        // Direct playhead moves on a paused deck (jog / scrub).
        if (!snapPlaying_[d] && !dk.playing && dk.pos != snapPos_[d]) {
            changed = true;
            if (recordable()) recordAction(SetAction::SetPos, d, 0, dk.pos);
        }
        for (int i = 0; i < kNumHotCues; ++i) {
            if (snapHotCue_[d][i] && !dk.hotCueSet[i]) {
                changed = true;
                if (recordable()) recordAction(SetAction::HotCueClear, d, i);
            }
        }
        for (int p = 0; p < int(SetParam::Count); ++p) {
            if (d == 1 && isGlobalParam(SetParam(p))) continue;
            if (getParam(d, SetParam(p)) != params_[d * int(SetParam::Count) + p].snap) changed = true;
        }
    }
    emitParams();
    if (replayActive_ && (changed || userTouched_)) {
        stopReplay();
        tookOver = true;
    }
    userTouched_ = false;
}

void Engine::emitParams() {
    // Records exactly the changes made since beginUserEdits(). Changes the engine
    // makes by itself (transition lanes, sync) happen outside that window and
    // replay on their own. Values are exact so replay matches sample for sample.
    if (!recordable()) return;
    for (int d = 0; d < 2; ++d) {
        for (int p = 0; p < int(SetParam::Count); ++p) {
            if (d == 1 && isGlobalParam(SetParam(p))) continue;
            const double cur = getParam(d, SetParam(p));
            if (cur != params_[d * int(SetParam::Count) + p].snap) recordAction(SetAction::Param, d, p, cur);
        }
    }
}

void Engine::startRecording() {
    recording_ = true;
    recStart_ = clock;
    rec_.clear();
    // Opening snapshot so the set replays from the exact starting state.
    for (int d = 0; d < 2; ++d) {
        const Deck& dk = decks[d];
        SetEvent load;
        load.action = SetAction::Load;
        load.deck = d;
        if (dk.track) load.track = trackRefFor(*dk.track);
        recordEvent(std::move(load));
        if (!dk.track) continue;
        for (int i = 0; i < kNumHotCues; ++i)
            if (dk.hotCueSet[i]) recordAction(SetAction::HotCueAt, d, i, dk.hotCues[i]);
        if (dk.loopActive) recordAction(SetAction::LoopRange, d, 0, dk.loopIn, dk.loopOut);
        for (int p = 0; p < int(SetParam::Count); ++p)
            if (!isGlobalParam(SetParam(p))) recordAction(SetAction::Param, d, p, getParam(d, SetParam(p)));
        recordAction(SetAction::SetPos, d, 0, dk.pos);
        recordAction(SetAction::SetPlaying, d, 0, 0, 0, dk.playing);
    }
    for (int p = int(SetParam::Crossfader); p < int(SetParam::Count); ++p)
        recordAction(SetAction::Param, 0, p, getParam(0, SetParam(p)));
    recordAction(SetAction::MasterDeck, masterDeck);
}

SetRecording Engine::stopRecording(const std::string& name) {
    SetRecording set;
    set.name = name;
    set.lengthFrames = recFrame();
    set.events = std::move(rec_);
    rec_.clear();
    recording_ = false;
    return set;
}

// --------------------------------------------------------------- replay ----

void Engine::startReplay(SetRecording set) {
    run.state = TransitionRun::State::Idle;
    for (auto& dk : decks) {
        dk.playing = false;
        dk.motion = Motion::Normal;
        dk.nudge = 0;
        dk.scratching = false;
    }
    previewing = false;
    replay = std::move(set);
    replayNext = 0;
    replayClock = 0;
    replayStalled = false;
    replayFinished = false;
    tookOver = false;
    replayNotes.clear();
    legacyTransitionSync = replay.version < 2;
    replayActive_ = true;
}

void Engine::stopReplay() {
    replayActive_ = false;
    legacyTransitionSync = false;
}

void Engine::runReplay() {
    replayStalled = false;
    while (replayNext < replay.events.size()) {
        SetEvent& ev = replay.events[replayNext];
        if (ev.frame > replayClock) break;
        if (ev.action == SetAction::Load && !ev.track.empty() && !ev.resolved && !ev.resolveFailed) {
            replayStalled = true;  // wait for the UI / renderer to provide the audio
            return;
        }
        applyEvent(ev);
        ev.resolved.reset();  // the deck holds its own reference now
        ++replayNext;
    }
}

void Engine::applyEvent(const SetEvent& ev) {
    const int d = std::clamp(ev.deck, 0, 1);
    switch (ev.action) {
        case SetAction::Note: replayNotes.push_back(ev.text); break;
        case SetAction::Load: loadTrack(d, ev.resolved); break;
        case SetAction::Play: play(d); break;
        case SetAction::Pause: pause(d); break;
        case SetAction::CueButton: cueButton(d); break;
        case SetAction::Seek: seek(d, ev.value); break;
        case SetAction::SetPos: decks[d].pos = ev.value; break;
        case SetAction::SetPlaying:
            decks[d].playing = ev.flag && decks[d].loaded();
            decks[d].motion = Motion::Normal;
            break;
        case SetAction::HotCueSet: setHotCue(d, std::clamp(ev.param, 0, kNumHotCues - 1)); break;
        case SetAction::HotCueJump: jumpHotCue(d, std::clamp(ev.param, 0, kNumHotCues - 1)); break;
        case SetAction::HotCueClear: decks[d].hotCueSet[std::clamp(ev.param, 0, kNumHotCues - 1)] = false; break;
        case SetAction::HotCueAt: {
            int slot = std::clamp(ev.param, 0, kNumHotCues - 1);
            decks[d].hotCues[slot] = ev.value;
            decks[d].hotCueSet[slot] = true;
            break;
        }
        case SetAction::Loop: setLoop(d, float(ev.value)); break;
        case SetAction::LoopExit: exitLoop(d); break;
        case SetAction::LoopRange: setLoopRange(d, ev.value, ev.value2); break;
        case SetAction::Motion: startMotion(d, Motion(std::clamp(ev.param, 0, 3))); break;
        case SetAction::Scratch: scratch(d, ev.flag, ev.value); break;
        case SetAction::SyncTempo: syncTempo(d, nullptr); break;
        case SetAction::AlignPhase: alignPhase(d, ev.value, ev.value2, ev.flag); break;
        case SetAction::Transition: startTransition(ev.def, d, nullptr, ev.flag); break;
        case SetAction::CancelTransition: cancelTransition(); break;
        case SetAction::ResetFx: resetAllFx(); break;
        case SetAction::MasterDeck: masterDeck = d; break;
        case SetAction::Param:
            if (ev.param >= 0 && ev.param < int(SetParam::Count)) setParam(d, SetParam(ev.param), ev.value);
            break;
    }
}
