// Headless tests for the audio engine, transitions, BPM detection and persistence.
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

#include "Engine.h"
#include "Library.h"
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
        // A tempo glide hands over to the incoming track's own tempo; everything else keeps the outgoing one.
        const double wantBpm = def.lane(Param::Tempo).enabled ? e.decks[1].track->bpm : 118.0;
        CHECK(std::fabs(e.decks[1].effectiveBpm() - wantBpm) < 0.01, "%s: tempo not matched", def.name.c_str());
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
                    if (at(32.5)) e.decks[1].slip = true;
                    if (t > 33.0 && t < 34.5) {                                 // scratch (as the waveform drag does, per UI frame)
                        if (blk % 11 == 0) e.scratch(1, true, e.decks[1].slipPos + std::sin(t * 9.0) * 6000.0);
                    }
                    if (at(34.5)) e.scratch(1, false, 0);
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
        f.loadTrack(1, tracks[5]);  // 88 lofi -> ratio 1.57 or 0.78: a big (22%) stretch
        f.play(0);
        renderSeconds(f, 2.0);
        f.startTransition(defs[0], 0, &why);
        renderSeconds(f, 12.0);
        std::printf("trance 138 -> lofi 88: synced=%d note='%s' B rate %.3f\n", f.run.synced, f.transitionNote.c_str(),
                    f.decks[1].rate());
        // A blend beat-matches anyway (it would trainwreck otherwise), but suggests Tempo Ramp.
        CHECK(f.run.synced || f.decks[1].playing, "big-gap blend should beat-match");
        CHECK(std::fabs(f.decks[1].effectiveBpm() * f.decks[1].syncFold - 138.0) < 0.01, "big-gap blend: B at %.2f", f.decks[1].effectiveBpm());
        CHECK(f.transitionNote.find("Tempo Ramp") != std::string::npos, "expected a Tempo Ramp hint");

        // A cut-style transition (Backspin) leaves the new track at its own tempo instead.
        Engine g;
        g.loadTrack(0, tracks[4]);
        g.loadTrack(1, tracks[5]);
        g.decks[1].tempo = 0;
        g.play(0);
        renderSeconds(g, 2.0);
        g.decks[1].tempo = 0;  // undo the cued-deck pre-match (only happens within 16%, but be explicit)
        g.startTransition(defs[4], 0, &why);
        renderSeconds(g, 8.0);
        std::printf("trance 138 -> lofi 88 (Backspin): B rate %.3f note='%s'\n", g.decks[1].rate(), g.transitionNote.c_str());
        CHECK(std::fabs(g.decks[1].rate() - 1.0) < 1e-9, "cut transition should keep the new track's tempo");
        CHECK(g.transitionNote.empty(), "cut transition shouldn't warn about tempo");

        // Pressing SYNC by hand first is respected, and no warning is shown.
        Engine h;
        h.loadTrack(0, tracks[4]);
        h.loadTrack(1, tracks[5]);
        h.play(0);
        renderSeconds(h, 2.0);
        h.syncTempo(1, &why);
        h.startTransition(defs[0], 0, &why);
        renderSeconds(h, 12.0);
        CHECK(h.transitionNote.empty(), "hand-synced deck got a note: %s", h.transitionNote.c_str());
        CHECK(std::fabs(h.decks[1].effectiveBpm() * h.decks[1].syncFold - 138.0) < 0.01, "hand-synced: B at %.2f", h.decks[1].effectiveBpm());

        // Sets recorded before 0.3 keep the old rule (no sync beyond 12%) so they replay as recorded.
        Engine old;
        old.legacyTransitionSync = true;
        old.loadTrack(0, tracks[4]);
        old.loadTrack(1, tracks[5]);
        old.play(0);
        renderSeconds(old, 2.0);
        old.startTransition(defs[0], 0, &why);
        renderSeconds(old, 12.0);
        CHECK(std::fabs(old.decks[1].rate() - 1.0) < 1e-9, "legacy rule: unsynced deck should keep its tempo");
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

    // Scratching: the record follows the hand, stops when the hand stops, plays on after.
    {
        Engine e;
        e.loadTrack(0, tracks[1]);
        e.play(0);
        renderSeconds(e, 2.0);
        const double start = e.decks[0].pos;
        e.scratch(0, true, start);  // hand down, holding still
        renderSeconds(e, 0.3);
        const double held = e.decks[0].pos;
        renderSeconds(e, 0.5);
        std::printf("scratch hold: moved %.1f frames while held\n", e.decks[0].pos - held);
        CHECK(std::fabs(e.decks[0].pos - held) < 50.0, "record kept moving under the hand");
        const double back = start - e.decks[0].framesPerBeat();
        e.scratch(0, true, back);  // pull it back a beat
        renderSeconds(e, 0.3);
        CHECK(std::fabs(e.decks[0].pos - back) < 100.0, "scratch target not reached: %.0f vs %.0f", e.decks[0].pos, back);
        e.scratch(0, false, 0);
        const double released = e.decks[0].pos;
        renderSeconds(e, 1.0);
        double advanced = (e.decks[0].pos - released) / kSampleRate;
        CHECK(std::fabs(advanced - 1.0) < 0.05 && e.decks[0].playing, "didn't play on after release (%.3fs)", advanced);

        // Slip: scratch for a second, release, and it's where it would have been.
        e.decks[0].slip = true;
        const double before = e.decks[0].pos;
        std::vector<float> buf(64 * 2);
        for (int i = 0; i < kSampleRate / 64; ++i) {
            if (i % 11 == 0) e.scratch(0, true, before - 20000.0 * std::sin(i * 0.05));
            e.render(buf.data(), 64, 2);
        }
        e.scratch(0, false, 0);
        const double expect = before + (kSampleRate / 64) * 64.0 * e.decks[0].rate();
        std::printf("slip: %.0f frames from where it would have been\n", e.decks[0].pos - expect);
        CHECK(std::fabs(e.decks[0].pos - expect) < 2.0, "slip lost its place by %.0f frames", e.decks[0].pos - expect);

        // A paused record can be scratched too, and it is heard.
        Engine p;
        p.loadTrack(0, tracks[1]);
        p.crossfader = 0.0f;
        const double p0 = p.decks[0].frameOfBeat(32);
        p.decks[0].pos = p0;
        float peak = 0;
        p.scratch(0, true, p0 + 30000.0);
        renderSeconds(p, 0.5, &peak);
        CHECK(peak > 0.01f, "paused scratch was silent");
        CHECK(std::fabs(p.decks[0].pos - (p0 + 30000.0)) < 100.0 && !p.decks[0].playing, "paused scratch position");
        p.scratch(0, false, 0);
        const double rest = p.decks[0].pos;
        renderSeconds(p, 0.3);
        CHECK(p.decks[0].pos == rest, "paused deck moved after release");
    }

    // Transition persistence round trip.
    {
        auto d = defs[1];
        d.stock = false;
        d.name = "Round \"Trip\"";
        d.outEffect = OutEffect::LoopRoll;
        d.outEffectAt = 0.25f;
        d.inEffect = InEffect::SpinUp;
        d.lane(Param::Tempo).enabled = true;
        d.lane(Param::Tempo).keys = {{0, 0, CurveShape::Smooth}, {1, 1, CurveShape::Linear}};
        std::vector<TransitionDef> v{defs[0], d};
        const std::string pathStr = (std::filesystem::temp_directory_path() / "afterglow_tr_test.txt").string();
        const char* path = pathStr.c_str();
        CHECK(saveTransitions(path, v), "save");
        auto back = loadTransitions(path);
        CHECK(back.size() == 1, "loaded %zu", back.size());
        if (back.size() == 1) {
            CHECK(back[0].name == d.name, "name '%s'", back[0].name.c_str());
            CHECK(back[0].beats == d.beats && back[0].outEffect == d.outEffect && back[0].inEffect == d.inEffect, "fields");
            for (int p = 0; p < kNumParams; ++p) {
                CHECK(back[0].lanes[p].enabled == d.lanes[p].enabled, "lane %d enabled", p);
                for (float t = 0; t <= 1.0f; t += 0.05f)
                    CHECK(std::fabs(back[0].lanes[p].eval(t) - d.lanes[p].eval(t)) < 1e-4 || !d.lanes[p].enabled, "lane %d eval", p);
            }
        }
    }

    // Music folders: scan sub-folders, analyse once (cached), hide when the folder is removed.
    {
        namespace fs = std::filesystem;
        const fs::path dir = fs::temp_directory_path() / "afterglow_lib_test";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir / "Music" / "House" / "Deep", ec);
        fs::create_directories(dir / "Music" / ".hidden", ec);
        fs::create_directories(dir / "data", ec);
        const Track& src = *tracks[1];
        const size_t frames = size_t(kSampleRate) * 12;
        writeWav((dir / "Music" / "Artist - Top Level.wav").string(), src.samples.data(), frames);
        writeWav((dir / "Music" / "House" / "Deep" / "Nested.wav").string(), src.samples.data(), frames);
        writeWav((dir / "Music" / ".hidden" / "Skip me.wav").string(), src.samples.data(), frames);
        std::ofstream(dir / "Music" / "notes.txt") << "not audio";
        const std::string root = (dir / "Music").string(), data = (dir / "data").string();
        auto waitIdle = [](Library& lib) {
            for (int i = 0; i < 2000 && (lib.scanning() || lib.busy()); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            for (int i = 0; i < 2000 && (lib.scanning() || lib.busy()); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        };
        auto imported = [](Library& lib, std::vector<LibraryEntry*>* out) {
            out->clear();
            for (int i = 0; i < lib.size(); ++i)
                if (lib.entry(i)->kind == EntryKind::Imported && !lib.entry(i)->hidden) out->push_back(lib.entry(i));
        };
        std::vector<LibraryEntry*> found;
        {
            Library lib;
            lib.init(data + "/music", data + "/clips", data + "/cache.txt");
            lib.setFolders({root});
            waitIdle(lib);
            imported(lib, &found);
            CHECK(found.size() == 2, "folder scan found %zu files (want 2)", found.size());
            for (auto* e : found) {
                CHECK(e->bpm > 100 && e->bpm < 150, "%s: bpm %.1f", e->name.c_str(), e->bpm);
                CHECK(!e->track, "%s: audio kept after analysis", e->name.c_str());
                if (e->name == "Nested") CHECK(e->relDir == "House/Deep", "relDir '%s'", e->relDir.c_str());
                if (e->name == "Top Level") CHECK(e->artist == "Artist", "artist '%s'", e->artist.c_str());
            }
            lib.setFolders({});
            imported(lib, &found);
            CHECK(found.empty(), "removed folder still shows %zu files", found.size());
        }
        {
            // Second run: everything comes from the cache, nothing is re-analysed.
            Library lib;
            lib.init(data + "/music", data + "/clips", data + "/cache.txt");
            lib.setFolders({root});
            for (int i = 0; i < 200 && lib.scanning(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            imported(lib, &found);
            CHECK(found.size() == 2 && lib.analysisPending() == 0, "cache: %zu files, %d pending", found.size(), lib.analysisPending());
            for (auto* e : found) CHECK(e->bpm > 0 && e->lengthSec > 11.9, "cached %s: bpm %.1f len %.1f", e->name.c_str(), e->bpm, e->lengthSec);
            // A deck asking for it gets the audio; the cached tempo is used.
            int idx = -1;
            for (int i = 0; i < lib.size(); ++i)
                if (lib.entry(i)->name == "Nested") idx = i;
            TrackPtr t;
            for (int i = 0; i < 500 && !(t = lib.acquire(idx)); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            CHECK(t && std::fabs(t->bpm - lib.entry(idx)->bpm) < 1e-9, "acquire from folder");
        }
        fs::remove_all(dir, ec);
        std::printf("music folders: ok\n");
    }

    std::printf(failures ? "\n%d FAILURES\n" : "\nALL TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}
