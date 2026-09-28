// Procedural music generator: renders the built-in demo library.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "Track.h"

enum class SongStyle {
    SynthHouse,
    Techno,
    DeepHouse,
    DrumAndBass,
    Trance,
    LoFi,
    AcidTechno,
    ProgHouse,
    TechHouse,
    DiscoHouse,
    BassHouse,
    UkGarage,
    LiquidDnb,
    Neurofunk,
    JumpUp,
    Jungle,
};

struct SongSpec {
    std::string name;
    std::string artist;
    std::string genre;
    std::string keyName;  // e.g. "A min"
    double bpm;
    int rootMidi;         // root note of the key, e.g. 57 = A3
    uint32_t seed;
    SongStyle style;
};

// Arrangement of a generated song, in bars from the start.
struct SongSection {
    const char* name;  // "Intro", "Build", "Drop", "Breakdown", "Build 2", "Drop 2", "Outro"
    int startBar;
    int bars;
};
std::vector<SongSection> songSections(const SongSpec& spec);

// The stock library, in display order.
const std::vector<SongSpec>& stockSongs();

// Renders a full song. Output starts exactly on a downbeat (firstBeatSec = 0),
// has a drums-only intro and outro for easy mixing, and is peak-normalised.
// `progress` (optional) is updated from 0 to 1 while rendering.
// Does not call analyzeTrack(); the caller does that.
TrackPtr renderSong(const SongSpec& spec, std::atomic<float>* progress);
