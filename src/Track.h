// Track: decoded stereo audio plus the analysis data the UI needs.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

constexpr int kSampleRate = 44100;
constexpr int kWaveBinFrames = 128;  // frames per waveform analysis bin

// One column of the frequency-colored waveform. All values 0..1.
struct WaveBin {
    float peak = 0, low = 0, mid = 0, high = 0;
};

struct Track {
    std::string name;
    std::string artist;
    std::string genre;
    std::string key;
    std::string sourcePath;      // empty for generated tracks / clips
    double bpm = 120.0;
    double firstBeatSec = 0.0;   // position of the first downbeat
    std::vector<float> samples;  // interleaved stereo, kSampleRate
    std::vector<WaveBin> wave;   // one bin per kWaveBinFrames frames
    float autoGain = 1.0f;       // loudness normalisation factor
    bool isClip = false;
    bool isGenerated = false;

    size_t frames() const { return samples.size() / 2; }
    double lengthSec() const { return double(frames()) / kSampleRate; }
    double secPerBeat() const { return 60.0 / bpm; }
};

using TrackPtr = std::shared_ptr<Track>;

// Builds `wave` and `autoGain`. Call after `samples` is filled.
void analyzeTrack(Track& t);

// Estimates tempo and downbeat offset for imported audio.
double detectBpm(const Track& t, double* firstBeatSec);

// Loads wav/mp3/flac, resampled to kSampleRate stereo. Returns null on failure.
TrackPtr loadAudioFile(const std::string& path, std::string* error);

// Writes 16-bit stereo WAV. Returns false on failure.
bool writeWav(const std::string& path, const float* interleaved, size_t frames);
