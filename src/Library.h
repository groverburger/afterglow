// Track library: built-in generated songs, imported files, the user's music
// folders and saved clips. Tracks are rendered/decoded lazily on a background
// worker thread. Imported files are first only analysed (tempo, length; cached
// on disk) and their audio is decoded again when a deck or editor asks for it.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "SetFile.h"
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
    std::string root;        // music folder the file was found in ("" = dropped / drop folder)
    std::string relDir;      // sub-folder inside `root`, "/"-separated ("" = top level)
    std::string searchText;  // lower-case name, artist, genre and folder
    bool hidden = false;     // its folder was removed, or the file is gone
    bool wanted = false;     // someone asked for the audio (not just the analysis)
    uint64_t lastUse = 0;
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
    // `cachePath` stores tempo analysis of imported files between runs.
    void init(const std::string& musicDir, const std::string& clipDir, const std::string& cachePath = "");

    int size();
    LibraryEntry* entry(int i);
    // Ready track or null. Requests loading if it isn't loaded yet.
    TrackPtr acquire(int i);
    void request(int i, bool urgent);
    int addFile(const std::string& path);

    // Music folders: scanned recursively in the background, contents shown in the library.
    void setFolders(const std::vector<std::string>& roots);  // replaces the list and rescans
    std::vector<std::string> folders();
    void rescan();
    bool scanning() const { return scanning_; }
    int scanFound() const { return scanFound_; }
    int analysisPending();  // files still waiting for tempo analysis
    // Bumped whenever entries are added, hidden or shown (for UI caches).
    uint64_t generation() const { return generation_; }
    int addClip(TrackPtr clip);
    // Entry for a recorded set's track reference (imports missing files that exist). -1 if unknown.
    int findByRef(const TrackRef& ref);
    // Entry whose loaded audio is `t`, or -1.
    int findByTrack(const Track* t);
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
    uint64_t useCounter_ = 0;

    // Folder scanning.
    std::vector<std::string> roots_;
    std::thread scanner_;
    std::condition_variable scanCv_;
    bool scanRequested_ = false;
    std::atomic<bool> scanning_{false};
    std::atomic<int> scanFound_{0};
    std::atomic<uint64_t> generation_{0};

    // Analysis cache: path -> tempo/length, valid while size and mtime match.
    struct CacheRec {
        uint64_t size = 0;
        int64_t mtime = 0;
        double bpm = 0, firstBeat = 0, length = 0;
    };
    std::mutex cacheMutex_;
    std::map<std::string, CacheRec> cache_;
    std::string cachePath_;
    int cacheDirty_ = 0;

    void workerLoop();
    void scannerLoop();
    void scanRoot(const std::string& root);
    int addFileLocked(const std::string& path, const std::string& root, const std::string& relDir, bool* isNew);
    void loadEntry(LibraryEntry& e);
    void evictUnused(const LibraryEntry* keep);  // caller holds mutex_
    bool cacheLookup(const std::string& path, CacheRec* rec);
    void cacheStore(const std::string& path, double bpm, double firstBeat, double length);
    void loadCache();
    void saveCache();
};

bool isAudioFile(const std::string& path);
// Human-readable list of the formats isAudioFile() accepts, e.g. "WAV, MP3, FLAC".
const char* supportedFormats();
