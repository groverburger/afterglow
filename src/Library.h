// Track library: built-in generated songs, imported files and saved clips.
// Tracks are rendered/decoded lazily on a background worker thread.
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Track.h"

enum class EntryKind { Stock, Imported, Clip };
enum class EntryState { NotLoaded, Queued, Loading, Ready, Failed };

struct LibraryEntry {
    std::string name, artist, genre, key;
    double bpm = 0;          // 0 = unknown until analysed
    double lengthSec = 0;    // 0 = unknown until loaded
    double firstBeatSec = 0; // clips: stored beat-grid offset
    EntryKind kind = EntryKind::Stock;
    int stockIndex = -1;
    std::string path;        // imported files and clips
    std::atomic<EntryState> state{EntryState::NotLoaded};
    std::atomic<float> progress{0.0f};
    TrackPtr track;          // valid once state == Ready (guarded by Library mutex)
    std::string error;
};

class Library {
public:
    Library();
    ~Library();

    // Populate: stock songs, `musicDir` audio files and saved clips in `clipDir`.
    void init(const std::string& musicDir, const std::string& clipDir);

    int size();
    LibraryEntry* entry(int i);
    // Ready track or null. Requests loading if it isn't loaded yet.
    TrackPtr acquire(int i);
    void request(int i, bool urgent);
    int addFile(const std::string& path);
    int addClip(TrackPtr clip);
    // Saves a clip as WAV + metadata next to the other clips. Returns false on error.
    bool persistClip(const Track& clip, std::string* error);
    bool busy();
    std::string clipDir() const { return clipDir_; }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::unique_ptr<LibraryEntry>> entries_;
    std::deque<int> queue_;
    std::thread worker_;
    bool quit_ = false;
    int active_ = 0;
    std::string clipDir_;

    void workerLoop();
    void loadEntry(LibraryEntry& e);
};

bool isAudioFile(const std::string& path);
