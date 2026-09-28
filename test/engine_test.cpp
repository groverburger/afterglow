// Headless tests for the audio engine, transitions, BPM detection and persistence.
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "Engine.h"
#include "SetFile.h"
#include "SynthGen.h"
#include "Transitions.h"

static int failures = 0;
#define CHECK(c, ...)                                   \
    do {                                                \
        if (!(c)) {                                     \
            ++failures;                                 \
            std::printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);                   \
            std::printf("\n");                          \
        }                                               \
    } while (0)

static bool renderSeconds(Engine& e, double sec, float* peakOut = nullptr) {
    std::vector<float> buf(1024 * 2);
    int blocks = int(sec * kSampleRate / 1024);
    float peak = 0;
    for (int i = 0; i < blocks; ++i) {
        e.render(buf.data(), 1024, 2);
        for (float s : buf) {
            if (!std::isfinite(s)) return false;
            peak = std::max(peak, std::fabs(s));
        }
    }
    if (peakOut) *peakOut = peak;
    return true;
}

static double phaseDiff(const Engine& e, int a, int b) {
    double ba = e.decks[a].beatPos(), bb = e.decks[b].beatPos() * e.decks[b].syncFold;
    double d = std::fmod(ba - bb, 4.0);
    if (d < -2) d += 4;
    if (d > 2) d -= 4;
    return d;
}

int main() {
    auto t0 = std::chrono::steady_clock::now();
    std::vector<TrackPtr> tracks;
    // The first 8 stock songs are the originals the tests below index into.
    for (size_t i = 0; i < 8 && i < stockSongs().size(); ++i) {
        const auto& s = stockSongs()[i];
        auto t = renderSong(s, nullptr);
        analyzeTrack(*t);
        tracks.push_back(t);
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("rendered %zu songs in %.0f ms\n", tracks.size(), ms);

    // BPM detection accuracy on generated material (exercises the import path).
    std::printf("BPM detection:\n");
    for (auto& t : tracks) {
        double fb;
        double bpm = detectBpm(*t, &fb);
        double spb = 60.0 / t->bpm;
        double phaseErr = std::fmod(fb, spb);
        phaseErr = std::min(phaseErr, spb - phaseErr);
        bool ok = std::fabs(bpm - t->bpm) < 0.3 || std::fabs(bpm - 2 * t->bpm) < 0.5 || std::fabs(bpm - t->bpm / 2) < 0.3;
        std::printf("  %-18s true %6.1f detected %6.2f  firstBeat %.3fs (beat-phase err %.3fs) autoGain %.2f %s\n",
                    t->name.c_str(), t->bpm, bpm, fb, phaseErr, t->autoGain, ok ? "" : "<-- off");
        CHECK(ok, "bpm detection for %s", t->name.c_str());
    }

    // Every stock transition, A -> B, beat-matched pair.
    auto defs = makeStockTransitions();
    for (auto& def : defs) {
        Engine e;
        e.loadTrack(0, tracks[0]);  // 118
        e.loadTrack(1, tracks[2]);  // 122
        e.crossfader = 0;
        e.play(0);
        renderSeconds(e, 5.3);  // arbitrary non-bar position
        std::string why;
        bool ok = e.startTransition(def, 0, &why);
        CHECK(ok, "start %s: %s", def.name.c_str(), why.c_str());
        CHECK(e.run.state == TransitionRun::State::Armed, "armed");
        // It must wait for a bar line.
        double beforeBeat = e.decks[0].beatPos();
        int guard = 0;
        std::vector<float> buf(64 * 2);
        while (e.run.state == TransitionRun::State::Armed && guard++ < 100000) e.render(buf.data(), 64, 2);
        double startBeat = e.decks[0].beatPos();
        double barFrac = std::fmod(startBeat, 4.0);
        barFrac = std::min(barFrac, 4.0 - barFrac);
        CHECK(barFrac < 0.05, "%s started off-bar (beat %.3f, was %.3f)", def.name.c_str(), startBeat, beforeBeat);
        CHECK(e.run.synced, "%s should be synced", def.name.c_str());
        float peak;
        bool finite = renderSeconds(e, def.beats * 60.0 / 118.0 + 2.0, &peak);
        CHECK(finite, "NaN during %s", def.name.c_str());
        CHECK(peak <= 1.0f, "%s clipped: %.3f", def.name.c_str(), peak);
        CHECK(e.run.state == TransitionRun::State::Idle, "%s did not finish", def.name.c_str());
        CHECK(!e.decks[0].playing, "%s: out deck still playing", def.name.c_str());
        CHECK(e.decks[1].playing, "%s: in deck not playing", def.name.c_str());
        CHECK(e.crossfader > 0.99f, "%s: crossfader at %.2f", def.name.c_str(), e.crossfader);
        CHECK(e.masterDeck == 1, "%s: master deck", def.name.c_str());
        CHECK(std::fabs(e.decks[1].eq[0] - 0.5f) < 1e-3 && std::fabs(e.decks[1].filter - 0.5f) < 1e-3,
              "%s: in deck left non-neutral", def.name.c_str());
        double inBeat = e.decks[1].beatPos();
        double inFrac = inBeat - std::floor(inBeat);
        std::printf("  %-18s ok  (in deck at beat %.3f, eff bpm %.2f, peak %.2f)\n", def.name.c_str(), inBeat,
                    e.decks[1].effectiveBpm(), peak);
        CHECK(std::fabs(e.decks[1].effectiveBpm() - 118.0) < 0.01, "%s: tempo not matched", def.name.c_str());
        (void)inFrac;
    }

    // Landing mode: the in deck reaches its cued spot exactly when the blend ends.
    for (auto& def : defs) {
        Engine e;
        e.loadTrack(0, tracks[1]);  // 128
        e.loadTrack(1, tracks[7]);  // 126
        e.play(0);
        renderSeconds(e, 7.3);
        const double targetBeat = 96.0;  // a downbeat deep into the song (bar 25)
        e.decks[1].pos = e.decks[1].frameOfBeat(targetBeat);
        std::string why;
        e.startTransition(def, 0, &why, true);
        std::vector<float> buf(64 * 2);
        int guard = 0;
        bool wasRunning = false;
        while (guard++ < 400000) {
            e.render(buf.data(), 64, 2);
            if (e.run.state == TransitionRun::State::Running) wasRunning = true;
            if (wasRunning && e.run.state == TransitionRun::State::Idle) break;
        }
        double at = e.decks[1].beatPos();
        std::printf("  landing %-18s in deck at beat %.3f when blend ended (target %.0f)\n", def.name.c_str(), at, targetBeat);
        CHECK(std::fabs(at - targetBeat) < 0.05, "%s landed at %.3f", def.name.c_str(), at);
        // The landing beat also has to line up with the outgoing track's beat grid.
        double oFrac = e.decks[0].beatPos() - std::round(e.decks[0].beatPos());
        // (Skipped when the out deck was braked or spun, which moves it off its grid on purpose.)
        if (def.outEffect == OutEffect::None) CHECK(std::fabs(oFrac) < 0.05, "%s off-grid", def.name.c_str());
    }

    // Record a hands-on session, save + parse it, replay it: audio must be identical.
    {
        auto session = [&](Engine& e, bool live, uint64_t* hash) {
            std::vector<float> buf(64 * 2);
            uint64_t h = 1469598103934665603ULL;
            for (int blk = 0; blk < 44100 * 40 / 64; ++blk) {
                if (live) {
                    const double t = blk * 64.0 / kSampleRate;
                    // Scripted "user": the kind of calls the GUI makes.
                    auto at = [&](double s) { return blk == int(s * kSampleRate / 64); };
                    e.beginUserEdits();
                    if (at(0.0)) { e.loadTrack(0, tracks[1]); e.loadTrack(1, tracks[6]); e.crossfader = 0.0f; e.play(0); }
                    if (at(2.0)) e.decks[0].eq[2] = 0.2f;                      // knob turn
                    if (t > 3.0 && t < 5.0) e.decks[0].filter = float(0.5 + (t - 3.0) * 0.1);  // filter sweep
                    if (at(5.0)) e.decks[0].filter = 0.5f;
                    if (at(6.0)) e.setLoop(0, 2.0f);
                    if (at(8.0)) e.exitLoop(0);
                    if (at(9.0)) e.decks[1].pos += 12345.0;                   // jog the paused deck
                    if (at(9.5)) e.setHotCue(0, 1);
                    if (at(11.0)) {
                        std::string why;
                        e.startTransition(defs[1], 0, &why, true);             // Bass Swap, landing
                    }
                    if (at(30.0)) e.jumpHotCue(1, 0);
                    if (at(31.0)) e.decks[1].echo = 0.7f;
                    if (at(32.0)) e.decks[1].echo = 0.0f;
                    e.endUserEdits();
                }
                e.render(buf.data(), 64, 2);
                for (float x : buf) {
                    uint32_t bits;
                    std::memcpy(&bits, &x, 4);
                    h = (h ^ bits) * 1099511628211ULL;
                }
            }
            *hash = h;
        };
        Engine live;
        live.startRecording();
        uint64_t liveHash, replayHash;
        session(live, true, &liveHash);
        SetRecording rec = live.stopRecording("test");
        const std::string setPath = (std::filesystem::temp_directory_path() / "afterglow_test.set").string();
        CHECK(saveSet(setPath, rec), "save set");
        SetRecording loaded;
        std::string err;
        CHECK(loadSet(setPath, loaded, &err), "load set: %s", err.c_str());
        for (auto& ev : loaded.events)
            if (ev.action == SetAction::Load && !ev.track.empty())
                for (auto& t : tracks)
                    if (t->name == ev.track.name) ev.resolved = t;
        Engine replay;
        replay.startReplay(loaded);
        session(replay, false, &replayHash);
        std::printf("record/replay: %zu events, replay %s\n", rec.events.size(),
                    liveHash == replayHash ? "bit-identical" : "DIFFERS");
        CHECK(liveHash == replayHash, "replayed session differs from the live one");
        CHECK(!replay.replayActive() && replay.replayFinished, "replay should have finished");

        // Touching a control during replay hands the mix back to the user.
        Engine t;
        t.startReplay(loaded);
        std::vector<float> buf(64 * 2);
        for (int i = 0; i < 100; ++i) t.render(buf.data(), 64, 2);
        t.beginUserEdits();
        t.crossfader = 0.3f;
        t.endUserEdits();
        CHECK(!t.replayActive() && t.tookOver, "take-over");
    }

    // Manual play with sync + quantize lands in phase (bar-aligned).
    {
        Engine e;
        e.loadTrack(0, tracks[1]);  // 128
        e.loadTrack(1, tracks[6]);  // 130
        e.play(0);
        renderSeconds(e, 3.77);
        e.play(1);
        renderSeconds(e, 10.0);
        double d = phaseDiff(e, 0, 1);
        std::printf("manual sync: phase diff after 10s = %.5f beats, B bpm %.3f\n", d, e.decks[1].effectiveBpm());
        CHECK(std::fabs(d) < 0.01, "manual sync phase %.4f", d);
        // Master tempo change propagates.
        e.decks[0].tempo = 0.04;
        renderSeconds(e, 5.0);
        d = phaseDiff(e, 0, 1);
        CHECK(std::fabs(e.decks[1].effectiveBpm() - e.decks[0].effectiveBpm()) < 0.01, "sync follow");
        std::printf("after master tempo change: A %.2f B %.2f, phase drift %.4f\n", e.decks[0].effectiveBpm(),
                    e.decks[1].effectiveBpm(), d);
    }

    // DnB vs house: transition must refuse to beat-match but still complete.
    {
        Engine e;
        e.loadTrack(0, tracks[5]);  // 88 lofi
        e.loadTrack(1, tracks[3]);  // 174 dnb -> fold 0.5 ~ 87 ok
        e.play(0);
        renderSeconds(e, 2.0);
        std::string why;
        e.startTransition(defs[0], 0, &why);
        renderSeconds(e, 20.0);
        std::printf("lofi 88 -> dnb 174: synced=%d fold=%.1f B eff %.2f note='%s'\n", e.run.synced, e.decks[1].syncFold,
                    e.decks[1].effectiveBpm(), e.transitionNote.c_str());
        CHECK(e.decks[1].playing && !e.decks[0].playing, "lofi->dnb finish");

        Engine f;
        f.loadTrack(0, tracks[4]);  // 138 trance
        f.loadTrack(1, tracks[5]);  // 88 lofi -> ratio 1.57 or 0.78 -> not syncable within 12%
        f.play(0);
        renderSeconds(f, 2.0);
        f.startTransition(defs[0], 0, &why);
        renderSeconds(f, 12.0);
        std::printf("trance 138 -> lofi 88: synced=%d note='%s' B rate %.3f\n", f.run.synced, f.transitionNote.c_str(),
                    f.decks[1].rate());
        CHECK(!f.transitionNote.empty(), "expected no-sync note");
        CHECK(std::fabs(f.decks[1].rate() - 1.0) < 1e-9, "unsynced deck should keep its tempo");
    }

    // Transition with nothing playing just starts the in deck.
    {
        Engine e;
        e.loadTrack(0, tracks[0]);
        e.loadTrack(1, tracks[2]);
        std::string why;
        e.startTransition(defs[4], 1, &why);  // Backspin, out = B (not playing)
        renderSeconds(e, 0.1);
        CHECK(e.decks[0].playing, "cold start should play A immediately");
    }

    // Loops wrap and hot cues keep phase.
    {
        Engine e;
        e.loadTrack(0, tracks[1]);
        e.play(0);
        renderSeconds(e, 4.1);
        e.setLoop(0, 1.0f);
        double in = e.decks[0].loopIn, out = e.decks[0].loopOut;
        renderSeconds(e, 5.0);
        CHECK(e.decks[0].pos >= in && e.decks[0].pos < out, "loop wrap");
        double bIn = e.decks[0].beatAt(in);
        CHECK(std::fabs(bIn - std::round(bIn)) < 1e-6, "loop starts on beat");
        e.exitLoop(0);
    }

    // Transition persistence round trip.
    {
        auto d = defs[1];
        d.stock = false;
        d.name = "Round \"Trip\"";
        d.outEffect = OutEffect::Backspin;
        d.outEffectAt = 0.25f;
        std::vector<TransitionDef> v{defs[0], d};
        const std::string pathStr = (std::filesystem::temp_directory_path() / "afterglow_tr_test.txt").string();
        const char* path = pathStr.c_str();
        CHECK(saveTransitions(path, v), "save");
        auto back = loadTransitions(path);
        CHECK(back.size() == 1, "loaded %zu", back.size());
        if (back.size() == 1) {
            CHECK(back[0].name == d.name, "name '%s'", back[0].name.c_str());
            CHECK(back[0].beats == d.beats && back[0].outEffect == d.outEffect, "fields");
            for (int p = 0; p < kNumParams; ++p) {
                CHECK(back[0].lanes[p].enabled == d.lanes[p].enabled, "lane %d enabled", p);
                for (float t = 0; t <= 1.0f; t += 0.05f)
                    CHECK(std::fabs(back[0].lanes[p].eval(t) - d.lanes[p].eval(t)) < 1e-4 || !d.lanes[p].enabled, "lane %d eval", p);
            }
        }
    }

    std::printf(failures ? "\n%d FAILURES\n" : "\nALL TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}
