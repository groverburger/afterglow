// Composes and renders Afterglow DJ sets.
//
//   afterglow_set compose <out.set> [out.wav] [tracklist.txt]
//       Performs the scripted demo set below on an offline engine while
//       recording it, saves the recording (the app can replay it live), and
//       optionally renders it to WAV by replaying the saved file.
//   afterglow_set render <in.set> <out.wav>
//       Renders any recorded set (stock songs and audio files) to WAV.
//
// The script is a list of songs; each says how it is mixed in (transition,
// which bar of the outgoing track to start on, where the incoming track lands)
// and which live moves to perform on it (filter builds, loop rolls, echo
// throws, rewinds).
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "Engine.h"
#include "SynthGen.h"
#include "Transitions.h"
#include "SetFile.h"
#include "dr_wav.h"

namespace {

enum class MoveType { FilterBuild, LoopRoll, EchoThrow, Rewind };

struct Move {
    MoveType type;
    int bar;  // FilterBuild: build start (ends at the next drop); others: the bar/beat they land on
};

struct SetEntry {
    int song;               // index into stockSongs()
    double targetBpm;       // tempo to drift to once this track is live
    std::string transition; // how it comes in (ignored for the opener)
    int mixAtBar;           // bar of the OUTGOING track where the blend starts
    bool land;              // true: inBar is where it lands when the blend ends; false: where it starts
    int inBar;
    std::vector<Move> moves;
    std::string note;
};

// ---- the set ----------------------------------------------------------------
// Songs: 0 Neon Arcade, 1 Midnight Circuit, 2 Sunset Boulevard, 3 Hyperdrive, 4 Crystal Caves,
// 5 Lo-Fi Lagoon, 6 Acid Rain, 7 Cloud Nine, 8 Warehouse Shuffle, 9 Mirrorball Motel,
// 10 Subwoofer Sermon, 11 Night Bus Home, 12 Rainfall Theory, 13 Chrome Mandible,
// 14 Bounce Protocol, 15 Rewind Selecta. House songs: drops at bar 16 and 48, outro 64-72.
// DnB (12-15): drops at 16 and 72, outro 104-112.
const std::vector<SetEntry> kSet = {
    {2, 122, "", 0, false, 0, {{MoveType::FilterBuild, 40}}, "Deep house warm-up, from the very first bar"},
    {0, 122, "Bass Swap", 64, true, 16, {{MoveType::EchoThrow, 40}}, "Up one on the Camelot wheel (7A -> 8A)"},
    {8, 124, "Filter Sweep", 64, true, 16, {}, "Tech house shuffle (9A)"},
    {7, 125, "Long EQ Blend", 56, true, 16, {{MoveType::LoopRoll, 48}}, "16-bar EQ blend under the last drop (10A)"},
    {10, 127, "Bass Swap", 64, true, 16, {{MoveType::EchoThrow, 40}, {MoveType::FilterBuild, 40}}, "Bass house (11A)"},
    {6, 129, "Wash Out", 64, true, 16, {{MoveType::FilterBuild, 40}}, "Acid peak (12A)"},
    {1, 128, "Echo Out", 60, true, 16, {}, "Echo out to reset the key: techno in 4A"},
    {9, 129, "Smooth Crossfade", 64, true, 16, {{MoveType::LoopRoll, 48}}, "Same key, disco house"},
    {11, 132, "Bass Swap", 64, true, 16, {{MoveType::EchoThrow, 40}}, "UK garage lifts the tempo (3A)"},
    {13, 174, "Backspin", 60, false, 16, {}, "Backspin straight into a neurofunk drop"},
    {14, 174, "Long EQ Blend", 88, true, 16, {}, "Jump-up (4A -> 6A, energy boost)"},
    {12, 173, "Bass Swap", 96, true, 16, {{MoveType::FilterBuild, 64}}, "Liquid breather (7A)"},
    {15, 172, "Quick Cut", 96, false, 16, {{MoveType::Rewind, 24}}, "Jungle! ...and pull it up for a rewind"},
    {3, 174, "Bass Swap", 40, true, 16, {{MoveType::LoopRoll, 48}}, "Out of the jungle before its breakdown: classic rollers (9A)"},
    {5, 87, "Power Down", 64, false, 8, {}, "Power down into a lo-fi cool-down"},
};

std::string mmss(double sec) {
    char b[16];
    std::snprintf(b, sizeof(b), "%d:%02d", int(sec) / 60, int(sec) % 60);
    return b;
}

// Continuous moves (tempo glides, filter sweeps) update at about the rate a
// GUI frame would, so recorded sets stay compact.
constexpr uint64_t kGlideStep = 2048;

// A running automation: returns false when finished.
using Automation = std::function<bool()>;

// FNV-1a over the 16-bit samples, i.e. exactly what ends up in the WAV.
uint64_t hashPcm(uint64_t h, const float* x, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        int16_t s = int16_t(std::lrint(std::clamp(x[i], -1.0f, 1.0f) * 32767.0f));
        h = (h ^ uint16_t(s)) * 1099511628211ULL;
    }
    return h;
}

uint64_t hashWav(const std::string& path) {
    unsigned ch = 0, sr = 0;
    drwav_uint64 n = 0;
    float* pcm = drwav_open_file_and_read_pcm_frames_f32(path.c_str(), &ch, &sr, &n, nullptr);
    if (!pcm) return 0;
    // Undo the int16 -> float conversion so the hash matches hashPcm().
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < size_t(n) * ch; ++i) {
        int16_t s = int16_t(std::lrint(pcm[i] * 32768.0f));
        h = (h ^ uint16_t(s)) * 1099511628211ULL;
    }
    drwav_free(pcm, nullptr);
    return h;
}

TrackPtr resolveTrack(const TrackRef& ref) {
    if (ref.kind == "stock") {
        for (const auto& s : stockSongs()) {
            if (s.name != ref.name) continue;
            TrackPtr t = renderSong(s, nullptr);
            analyzeTrack(*t);
            return t;
        }
    } else if (ref.kind == "file") {
        std::string err;
        if (TrackPtr t = loadAudioFile(ref.path, &err)) return t;
        std::fprintf(stderr, "warning: %s\n", err.c_str());
    }
    std::fprintf(stderr, "warning: can't find track '%s' (%s)\n", ref.name.c_str(), ref.kind.c_str());
    return nullptr;
}

int renderSet(const std::string& setPath, const std::string& wavPath, const char* tracklistPath) {
    SetRecording set;
    std::string err;
    if (!loadSet(setPath, set, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    std::vector<std::string> lines;
    auto onNote = [&](double sec, const std::string& text) {
        lines.push_back("[" + mmss(sec) + "] " + text);
        std::printf("%s\n", lines.back().c_str());
    };
    if (!renderSetToWav(set, resolveTrack, wavPath, nullptr, &err, onNote)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    std::printf("wrote %s (%s)\n", wavPath.c_str(), mmss(set.lengthSec()).c_str());
    if (tracklistPath) {
        std::ofstream out(tracklistPath);
        out << set.name << "\n\n";
        for (auto& l : lines) out << l << "\n";
    }
    return 0;
}

int composeSet(const std::string& setPath, uint64_t* liveHash) {
    const auto& songs = stockSongs();
    const auto defs = makeStockTransitions();
    auto findDef = [&](const std::string& name) -> const TransitionDef& {
        for (auto& d : defs)
            if (d.name == name) return d;
        std::fprintf(stderr, "unknown transition %s\n", name.c_str());
        std::exit(1);
    };
    auto load = [&](int song) {
        TrackPtr t = renderSong(songs[size_t(song)], nullptr);
        analyzeTrack(*t);
        return t;
    };

    Engine e;
    e.startRecording();
    auto now = [&] { return double(e.clock) / kSampleRate; };
    auto say = [&](const std::string& s) {
        std::printf("[%s] %s\n", mmss(now()).c_str(), s.c_str());
        std::fflush(stdout);
        e.recordNote(s);
    };
    std::vector<Automation> autos;
    // Tempo drift: glide the live deck to the set's target tempo over ~16 bars.
    auto driftTempo = [&](int d, double targetBpm) {
        Deck& dk = e.decks[d];
        const double from = dk.tempo, to = targetBpm / dk.track->bpm - 1.0;
        const double start = now(), dur = 16 * 4 * 60.0 / targetBpm;
        autos.push_back([&, d, from, to, start, dur] {
            if (e.clock % kGlideStep != 0) return true;  // knob-like update rate keeps the set file small
            double x = std::min(1.0, (now() - start) / dur);
            e.decks[d].tempo = from + (to - from) * x;
            return x < 1.0;
        });
    };

    // Schedules the live moves of one entry on deck d.
    auto scheduleMoves = [&](int d, const SetEntry& en) {
        const std::string name = songs[size_t(en.song)].name;
        for (const Move& m : en.moves) {
            const double beat = m.bar * 4.0;
            switch (m.type) {
                case MoveType::FilterBuild: {
                    // High-pass rises through the 8-bar build and snaps open on the drop.
                    bool announced = false;
                    autos.push_back([&, d, beat, announced, name]() mutable {
                        Deck& dk = e.decks[d];
                        double b = dk.beatPos();
                        if (b < beat) return true;
                        if (!announced) {
                            say("  filter build on " + name);
                            announced = true;
                        }
                        double x = (b - beat) / 32.0;
                        if (x >= 1.0) {
                            dk.filter = 0.5f;  // snaps open right on the drop
                            return false;
                        }
                        if (e.clock % kGlideStep != 0) return true;
                        dk.filter = float(0.5 + 0.3 * x * x);
                        return true;
                    });
                    break;
                }
                case MoveType::EchoThrow: {
                    // Last beat before the bar goes into the echo, then the send closes.
                    int stage = 0;
                    autos.push_back([&, d, beat, stage, name]() mutable {
                        Deck& dk = e.decks[d];
                        double b = dk.beatPos();
                        if (stage == 0 && b >= beat - 1.0) {
                            dk.echo = 1.0f;
                            stage = 1;
                            say("  echo throw on " + name);
                        } else if (stage == 1 && b >= beat) {
                            dk.echo = 0.0f;
                            return false;
                        }
                        return true;
                    });
                    break;
                }
                case MoveType::LoopRoll: {
                    // 1 -> 1/2 -> 1/4 beat loops over the last two beats, releasing on the drop.
                    double rollStart = -1, spb = 0;
                    int stage = 0;
                    autos.push_back([&, d, beat, rollStart, spb, stage, name]() mutable {
                        Deck& dk = e.decks[d];
                        if (stage == 0) {
                            if (dk.beatPos() < beat - 2.0) return true;
                            rollStart = now();
                            spb = dk.secPerBeat() / dk.rate();
                            e.setLoop(d, 1.0f);
                            stage = 1;
                            say("  loop roll into the drop on " + name);
                            return true;
                        }
                        double t = (now() - rollStart) / spb;  // beats since the roll began
                        if (stage == 1 && t >= 1.0) { e.setLoop(d, 0.5f); stage = 2; }
                        if (stage == 2 && t >= 1.5) { e.setLoop(d, 0.25f); stage = 3; }
                        if (stage == 3 && t >= 2.0) {
                            e.exitLoop(d);
                            e.setPosition(d, dk.frameOfBeat(beat) + (t - 2.0) * spb * kSampleRate * dk.rate());
                            return false;
                        }
                        return true;
                    });
                    break;
                }
                case MoveType::Rewind: {
                    // Pull the record back and drop it again from the top of the drop.
                    int stage = 0;
                    autos.push_back([&, d, beat, stage, name]() mutable {
                        Deck& dk = e.decks[d];
                        if (stage == 0 && dk.beatPos() >= beat) {
                            e.startMotion(d, Motion::Backspin);
                            stage = 1;
                            say("  REWIND! on " + name);
                        } else if (stage == 1 && !dk.playing) {
                            e.setPosition(d, dk.frameOfBeat(16 * 4));
                            e.play(d);
                            return false;
                        }
                        return true;
                    });
                    break;
                }
            }
        }
    };

    // ---- run ----
    int live = 0;
    size_t cur = 0;
    e.loadTrack(0, load(kSet[0].song));
    e.beginUserEdits();
    e.crossfader = 0.0f;
    e.endUserEdits();
    e.setPosition(0, e.decks[0].frameOfBeat(kSet[0].inBar * 4.0));
    e.play(0);
    say(songs[size_t(kSet[0].song)].name + " - " + songs[size_t(kSet[0].song)].artist + "  (" + kSet[0].note + ")");
    scheduleMoves(0, kSet[0]);

    bool prepared = false, triggered = false, wasRunning = false;
    double endAt = -1;
    std::vector<float> block(64 * 2);
    uint64_t hash = 1469598103934665603ULL;

    for (;;) {
        const bool hasNext = cur + 1 < kSet.size();
        if (hasNext && !prepared && !e.transitionBusy()) {
            const SetEntry& nx = kSet[cur + 1];
            int d = 1 - live;
            e.loadTrack(d, load(nx.song));
            e.setPosition(d, e.decks[d].frameOfBeat(nx.inBar * 4.0));
            prepared = true;
        }
        if (hasNext && prepared && !triggered) {
            const SetEntry& nx = kSet[cur + 1];
            if (e.decks[live].beatPos() >= nx.mixAtBar * 4.0 - 2.0) {
                std::string why;
                if (!e.startTransition(findDef(nx.transition), live, &why, nx.land)) {
                    std::fprintf(stderr, "transition failed: %s\n", why.c_str());
                    return 1;
                }
                triggered = true;
                wasRunning = false;
            }
        }
        if (triggered) {
            if (e.run.state == TransitionRun::State::Running && !wasRunning) {
                wasRunning = true;
                const SetEntry& nx = kSet[cur + 1];
                say("  mixing: " + nx.transition + (nx.land ? " (landing on the drop)" : ""));
            }
            if (!e.transitionNote.empty()) {
                say("  " + e.transitionNote);
                e.transitionNote.clear();
            }
            if (wasRunning && e.run.state == TransitionRun::State::Idle) {
                live = 1 - live;
                ++cur;
                const SetEntry& en = kSet[cur];
                say(songs[size_t(en.song)].name + " - " + songs[size_t(en.song)].artist + "  (" + en.note + ")");
                driftTempo(live, en.targetBpm);
                scheduleMoves(live, en);
                triggered = prepared = false;
            }
        }
        e.beginUserEdits();  // automations turn knobs directly; record them
        for (size_t i = 0; i < autos.size();) {
            if (!autos[i]()) autos.erase(autos.begin() + long(i));
            else ++i;
        }
        e.endUserEdits();
        if (!hasNext && endAt < 0 && !e.decks[live].playing) {
            endAt = now() + 4.0;  // let echoes and reverb tails ring out
            say("Thanks for listening!");
        }
        if (endAt >= 0 && now() >= endAt) break;

        e.render(block.data(), 64, 2);
        hash = hashPcm(hash, block.data(), block.size());
    }
    SetRecording set = e.stopRecording("Sunset to Jungle");
    if (!saveSet(setPath, set)) {
        std::fprintf(stderr, "cannot write %s\n", setPath.c_str());
        return 1;
    }
    std::printf("recorded %zu events, %s -> %s\n", set.events.size(), mmss(set.lengthSec()).c_str(), setPath.c_str());
    if (liveHash) *liveHash = hash;
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "compose" && argc >= 3) {
        uint64_t liveHash = 0;
        if (int rc = composeSet(argv[2], &liveHash)) return rc;
        if (argc < 4) return 0;
        if (int rc = renderSet(argv[2], argv[3], argc >= 5 ? argv[4] : nullptr)) return rc;
        // The WAV was rendered by replaying the saved file: it must match the live run exactly.
        if (hashWav(argv[3]) != liveHash) {
            std::fprintf(stderr, "error: replay of the saved set differs from the live performance\n");
            return 2;
        }
        std::printf("verified: replaying the saved set is bit-identical to the live performance\n");
        return 0;
    }
    if (mode == "render" && argc >= 4) return renderSet(argv[2], argv[3], argc >= 5 ? argv[4] : nullptr);
    std::fprintf(stderr,
                 "usage:\n  %s compose <out.set> [out.wav] [tracklist.txt]\n  %s render <in.set> <out.wav> [tracklist.txt]\n",
                 argv[0], argv[0]);
    return 1;
}
