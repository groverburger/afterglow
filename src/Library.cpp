#include "Library.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "SynthGen.h"
#include "Transitions.h"  // quoteString / readQuotedString

namespace fs = std::filesystem;

namespace {

constexpr int kKeepLoaded = 6;        // decoded imported tracks kept around besides the ones in use
constexpr int kMaxScanFiles = 100000; // per music folder
constexpr int kCacheSaveEvery = 16;   // analyses between cache writes

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

std::string sanitizeFileName(const std::string& s) {
    std::string out;
    for (char c : s) out += (std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == ' ') ? c : '_';
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out.empty() ? "clip" : out;
}

// Clip sidecar: simple "key value" lines.
void readClipMeta(const std::string& metaPath, LibraryEntry& e) {
    std::ifstream in(metaPath);
    std::string line;
    while (std::getline(in, line)) {
        auto sp = line.find(' ');
        if (sp == std::string::npos) continue;
        std::string k = line.substr(0, sp), v = line.substr(sp + 1);
        if (k == "name") e.name = v;
        else if (k == "bpm") e.bpm = std::atof(v.c_str());
        else if (k == "first_beat") e.firstBeatSec = std::atof(v.c_str());
        else if (k == "key") e.key = v;
        else if (k == "genre") e.genre = v;
        else if (k == "artist") e.artist = v;
    }
}

void updateSearchText(LibraryEntry& e) {
    e.searchText = lower(e.name + " " + e.artist + " " + e.genre + " " + e.relDir);
}

// Folders that are app bundles or media databases rather than music.
bool skipDirectory(const fs::path& p) {
    std::string name;
    try {
        name = p.filename().string();
    } catch (const std::exception&) {
        return true;  // name not representable as UTF-8 (Windows)
    }
    if (name.empty() || name[0] == '.' || name[0] == '$') return true;  // hidden, $RECYCLE.BIN
    std::string ext = lower(fs::path(name).extension().string());
    return ext == ".musiclibrary" || ext == ".photoslibrary" || ext == ".app" || ext == ".tvlibrary" ||
           ext == ".band" || ext == ".logicx";
}

bool fileStamp(const std::string& path, uint64_t* size, int64_t* mtime) {
    std::error_code ec;
    auto sz = fs::file_size(path, ec);
    if (ec) return false;
    auto t = fs::last_write_time(path, ec);
    if (ec) return false;
    *size = uint64_t(sz);
    *mtime = int64_t(t.time_since_epoch().count());
    return true;
}

}  // namespace

bool isAudioFile(const std::string& path) {
    std::string ext = lower(fs::path(path).extension().string());
    if (ext == ".wav" || ext == ".mp3" || ext == ".flac") return true;
#if defined(__APPLE__)
    // Decoded by the system (Core Audio).
    if (ext == ".m4a" || ext == ".aac" || ext == ".aif" || ext == ".aiff" || ext == ".aifc" || ext == ".caf") return true;
#elif defined(_WIN32)
    // Decoded by the system (Media Foundation).
    if (ext == ".m4a" || ext == ".aac" || ext == ".wma") return true;
#endif
    return false;
}

const char* supportedFormats() {
#if defined(__APPLE__)
    return "WAV, MP3, FLAC, M4A/AAC, AIFF";
#elif defined(_WIN32)
    return "WAV, MP3, FLAC, M4A/AAC, WMA";
#else
    return "WAV, MP3, FLAC";
#endif
}

Library::Library() {
    worker_ = std::thread([this] { workerLoop(); });
    scanner_ = std::thread([this] { scannerLoop(); });
}

Library::~Library() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    cv_.notify_all();
    scanCv_.notify_all();
    worker_.join();
    scanner_.join();
    saveCache();
}

void Library::init(const std::string& musicDir, const std::string& clipDir, const std::string& cachePath) {
    clipDir_ = clipDir;
    cachePath_ = cachePath;
    loadCache();
    const auto& songs = stockSongs();
    for (size_t i = 0; i < songs.size(); ++i) {
        auto e = std::make_unique<LibraryEntry>();
        e->name = songs[i].name;
        e->artist = songs[i].artist;
        e->genre = songs[i].genre;
        e->key = songs[i].keyName;
        e->bpm = songs[i].bpm;
        e->kind = EntryKind::Stock;
        e->stockIndex = int(i);
        updateSearchText(*e);
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.push_back(std::move(e));
    }

    std::error_code ec;
    fs::create_directories(musicDir, ec);
    fs::create_directories(clipDir, ec);

    std::vector<std::string> files;
    for (auto& it : fs::directory_iterator(musicDir, ec))
        if (it.is_regular_file() && isAudioFile(it.path().string())) files.push_back(it.path().string());
    std::sort(files.begin(), files.end());
    for (auto& f : files) addFile(f);

    std::vector<std::string> clips;
    for (auto& it : fs::directory_iterator(clipDir, ec))
        if (it.is_regular_file() && it.path().extension() == ".wav") clips.push_back(it.path().string());
    std::sort(clips.begin(), clips.end());
    for (auto& c : clips) {
        auto e = std::make_unique<LibraryEntry>();
        e->name = fs::path(c).stem().string();
        e->artist = "My Clips";
        e->genre = "Clip";
        e->key = "?";
        e->kind = EntryKind::Clip;
        e->path = c;
        readClipMeta(fs::path(c).replace_extension(".txt").string(), *e);
        updateSearchText(*e);
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.push_back(std::move(e));
    }
    ++generation_;
}

int Library::size() {
    std::lock_guard<std::mutex> lock(mutex_);
    return int(entries_.size());
}

LibraryEntry* Library::entry(int i) {
    std::lock_guard<std::mutex> lock(mutex_);
    return i >= 0 && i < int(entries_.size()) ? entries_[size_t(i)].get() : nullptr;
}

TrackPtr Library::acquire(int i) {
    LibraryEntry* e = entry(i);
    if (!e) return nullptr;
    if (e->state == EntryState::Ready) {
        std::lock_guard<std::mutex> lock(mutex_);
        e->lastUse = ++useCounter_;
        if (e->track) return e->track;
    }
    request(i, true);
    return nullptr;
}

void Library::request(int i, bool urgent) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (i < 0 || i >= int(entries_.size())) return;
    LibraryEntry& e = *entries_[size_t(i)];
    if (urgent) {
        e.wanted = true;
        e.lastUse = ++useCounter_;
    }
    EntryState s = e.state;
    if (s == EntryState::Ready || s == EntryState::Loading) return;
    if (s == EntryState::Queued) {
        if (!urgent) return;
        queue_.erase(std::remove(queue_.begin(), queue_.end(), i), queue_.end());
    }
    e.state = EntryState::Queued;
    e.error.clear();
    if (urgent) queue_.push_front(i);
    else queue_.push_back(i);
    cv_.notify_one();
}

int Library::addFileLocked(const std::string& path, const std::string& root, const std::string& relDir, bool* isNew) {
    *isNew = false;
    for (size_t i = 0; i < entries_.size(); ++i) {
        LibraryEntry& e = *entries_[i];
        if (e.path != path) continue;
        if (e.hidden || (e.root.empty() && !root.empty())) {
            e.hidden = false;
            e.root = root;
            e.relDir = relDir;
            updateSearchText(e);
            ++generation_;
        }
        return int(i);
    }
    auto e = std::make_unique<LibraryEntry>();
    std::string stem = fs::path(path).stem().string();
    // "Artist - Title" file names are common; otherwise use the containing folder as the artist.
    auto dash = stem.find(" - ");
    if (dash != std::string::npos && dash > 0) {
        e->artist = stem.substr(0, dash);
        e->name = stem.substr(dash + 3);
    } else {
        e->name = stem;
        e->artist = root.empty() ? "Imported" : relDir.empty() ? "" : fs::path(relDir).filename().string();
    }
    e->genre = root.empty() ? "Imported" : "";
    e->key = "?";
    e->kind = EntryKind::Imported;
    e->path = path;
    e->root = root;
    e->relDir = relDir;
    updateSearchText(*e);
    CacheRec rec;
    if (cacheLookup(path, &rec)) {
        e->bpm = rec.bpm;
        e->firstBeatSec = rec.firstBeat;
        e->lengthSec = rec.length;
    }
    entries_.push_back(std::move(e));
    ++generation_;
    *isNew = true;
    return int(entries_.size()) - 1;
}

int Library::addFile(const std::string& path) {
    int idx;
    bool isNew;
    bool analysed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        idx = addFileLocked(path, "", "", &isNew);
        analysed = entries_[size_t(idx)]->bpm > 0;
    }
    if (isNew && !analysed) request(idx, false);  // analyse in the background so BPM shows up
    return idx;
}

int Library::addClip(TrackPtr clip) {
    auto e = std::make_unique<LibraryEntry>();
    e->name = clip->name;
    e->artist = clip->artist;
    e->genre = clip->genre;
    e->key = clip->key;
    e->bpm = clip->bpm;
    e->lengthSec = clip->lengthSec();
    e->kind = EntryKind::Clip;
    e->path = clip->sourcePath;
    e->track = std::move(clip);
    e->state = EntryState::Ready;
    updateSearchText(*e);
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.push_back(std::move(e));
    ++generation_;
    return int(entries_.size()) - 1;
}

int Library::findByRef(const TrackRef& ref) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < entries_.size(); ++i) {
            const LibraryEntry& e = *entries_[i];
            if (ref.kind == "stock" && e.kind == EntryKind::Stock && e.name == ref.name) return int(i);
            if (ref.kind == "clip" && e.kind == EntryKind::Clip && e.name == ref.name) return int(i);
            if (ref.kind == "file" && e.kind == EntryKind::Imported && e.path == ref.path) return int(i);
        }
    }
    std::error_code ec;
    if (ref.kind == "file" && fs::exists(ref.path, ec)) return addFile(ref.path);
    return -1;
}

int Library::findByTrack(const Track* t) {
    if (!t) return -1;
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < entries_.size(); ++i)
        if (entries_[i]->track.get() == t) return int(i);
    return -1;
}

bool Library::persistClip(const Track& clip, std::string* error) {
    std::error_code ec;
    fs::create_directories(clipDir_, ec);
    std::string base = sanitizeFileName(clip.name);
    fs::path wav = fs::path(clipDir_) / (base + ".wav");
    for (int n = 2; fs::exists(wav); ++n) wav = fs::path(clipDir_) / (base + " " + std::to_string(n) + ".wav");
    if (!writeWav(wav.string(), clip.samples.data(), clip.frames())) {
        if (error) *error = "Could not write " + wav.string();
        return false;
    }
    std::ofstream meta(fs::path(wav).replace_extension(".txt"));
    meta << "name " << clip.name << "\nbpm " << clip.bpm << "\nfirst_beat " << clip.firstBeatSec << "\nkey " << clip.key << "\ngenre " << clip.genre
         << "\nartist " << clip.artist << "\n";
    return true;
}

bool Library::busy() {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_ > 0 || !queue_.empty();
}

int Library::analysisPending() {
    std::lock_guard<std::mutex> lock(mutex_);
    int n = active_;
    for (int i : queue_)
        if (!entries_[size_t(i)]->wanted) ++n;
    return n;
}

// ---------------------------------------------------------------- folders ----

void Library::setFolders(const std::vector<std::string>& roots) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        roots_ = roots;
        // Entries from folders that are no longer in the list disappear from view.
        for (auto& e : entries_) {
            if (e->root.empty() || e->hidden) continue;
            if (std::find(roots_.begin(), roots_.end(), e->root) == roots_.end()) e->hidden = true;
        }
        ++generation_;
        scanRequested_ = true;
    }
    scanCv_.notify_one();
}

std::vector<std::string> Library::folders() {
    std::lock_guard<std::mutex> lock(mutex_);
    return roots_;
}

void Library::rescan() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        scanRequested_ = true;
    }
    scanCv_.notify_one();
}

void Library::scannerLoop() {
    for (;;) {
        std::vector<std::string> roots;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            scanCv_.wait(lock, [this] { return quit_ || scanRequested_; });
            if (quit_) return;
            scanRequested_ = false;
            roots = roots_;
        }
        scanning_ = true;
        scanFound_ = 0;
        for (const auto& r : roots) {
            scanRoot(r);
            std::lock_guard<std::mutex> lock(mutex_);
            if (quit_ || scanRequested_) break;  // list changed: start over
        }
        scanning_ = false;
    }
}

void Library::scanRoot(const std::string& root) {
    // Walk without holding the lock; only adding entries needs it.
    std::vector<std::string> files;
    std::error_code ec;
    auto opts = fs::directory_options::skip_permission_denied;
    for (auto it = fs::recursive_directory_iterator(root, opts, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        std::error_code e2;
        if (it->is_directory(e2)) {
            if (skipDirectory(it->path())) it.disable_recursion_pending();
            continue;
        }
        std::string file;
        try {
            if (it->is_regular_file(e2)) file = it->path().string();
        } catch (const std::exception&) {
            continue;  // name not representable as UTF-8 (Windows); skip it rather than crash
        }
        if (!file.empty() && isAudioFile(file)) {
            files.push_back(std::move(file));
            ++scanFound_;
            if (int(files.size()) >= kMaxScanFiles) break;
        }
        if (quit_) return;
    }
    std::sort(files.begin(), files.end());

    // Add in chunks so the UI (which reads entries every frame) never waits long.
    std::vector<int> toAnalyse, found;
    for (size_t start = 0; start < files.size(); start += 256) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (quit_ || std::find(roots_.begin(), roots_.end(), root) == roots_.end()) return;  // removed meanwhile
        for (size_t k = start; k < std::min(files.size(), start + 256); ++k) {
            const std::string& f = files[k];
            std::string rel = fs::path(f).parent_path().lexically_relative(root).generic_string();
            if (rel == ".") rel.clear();
            bool isNew;
            int idx = addFileLocked(f, root, rel, &isNew);
            found.push_back(idx);
            if (isNew && entries_[size_t(idx)]->bpm <= 0) toAnalyse.push_back(idx);
        }
    }
    {
        // Files that vanished since the last scan.
        std::lock_guard<std::mutex> lock(mutex_);
        if (std::find(roots_.begin(), roots_.end(), root) == roots_.end()) return;
        std::vector<bool> seen(entries_.size(), false);
        for (int i : found) seen[size_t(i)] = true;
        for (size_t i = 0; i < entries_.size(); ++i) {
            LibraryEntry& e = *entries_[i];
            if (e.root == root && !seen[i] && !e.hidden) {
                e.hidden = true;
                ++generation_;
            }
        }
    }
    for (int i : toAnalyse) request(i, false);
}

// ----------------------------------------------------------------- cache ----

bool Library::cacheLookup(const std::string& path, CacheRec* rec) {
    uint64_t size;
    int64_t mtime;
    if (!fileStamp(path, &size, &mtime)) return false;
    std::lock_guard<std::mutex> lock(cacheMutex_);
    auto it = cache_.find(path);
    if (it == cache_.end() || it->second.size != size || it->second.mtime != mtime || it->second.bpm <= 0) return false;
    *rec = it->second;
    return true;
}

void Library::cacheStore(const std::string& path, double bpm, double firstBeat, double length) {
    CacheRec rec;
    if (!fileStamp(path, &rec.size, &rec.mtime)) return;
    rec.bpm = bpm;
    rec.firstBeat = firstBeat;
    rec.length = length;
    bool save;
    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        cache_[path] = rec;
        save = ++cacheDirty_ >= kCacheSaveEvery;
    }
    if (save) saveCache();
}

// Format: one line per file: "<path>" size mtime bpm first_beat length
void Library::loadCache() {
    if (cachePath_.empty()) return;
    std::ifstream in(cachePath_);
    std::string line;
    std::lock_guard<std::mutex> lock(cacheMutex_);
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string path;
        CacheRec r;
        if (!readQuotedString(ls, path)) continue;
        if (ls >> r.size >> r.mtime >> r.bpm >> r.firstBeat >> r.length) cache_[path] = r;
    }
}

void Library::saveCache() {
    if (cachePath_.empty()) return;
    std::lock_guard<std::mutex> lock(cacheMutex_);
    if (cacheDirty_ == 0) return;
    const std::string tmp = cachePath_ + ".tmp";
    {
        std::ofstream out(tmp);
        if (!out) return;
        out.precision(12);
        for (const auto& [path, r] : cache_)
            out << quoteString(path) << " " << r.size << " " << r.mtime << " " << r.bpm << " " << r.firstBeat << " " << r.length << "\n";
        if (!out) return;
    }
    std::error_code ec;
    fs::rename(tmp, cachePath_, ec);
    cacheDirty_ = 0;
}

// ---------------------------------------------------------------- worker ----

void Library::workerLoop() {
    for (;;) {
        LibraryEntry* e = nullptr;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return quit_ || !queue_.empty(); });
            if (quit_) return;
            int i = queue_.front();
            queue_.pop_front();
            e = entries_[size_t(i)].get();
            e->state = EntryState::Loading;
            ++active_;
        }
        loadEntry(*e);
        bool idle;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            --active_;
            idle = queue_.empty();
        }
        if (idle) saveCache();
    }
}

void Library::loadEntry(LibraryEntry& e) {
    TrackPtr t;
    std::string err;
    e.progress = 0.0f;
    if (e.kind == EntryKind::Stock) {
        t = renderSong(stockSongs()[size_t(e.stockIndex)], &e.progress);
        if (t) analyzeTrack(*t);
    } else {
        CacheRec rec;
        const bool cached = e.kind == EntryKind::Imported && cacheLookup(e.path, &rec);
        t = loadAudioFile(e.path, &err, !cached);
        if (t && cached) {
            t->bpm = rec.bpm;
            t->firstBeatSec = rec.firstBeat;
        } else if (t && e.kind == EntryKind::Imported) {
            cacheStore(e.path, t->bpm, t->firstBeatSec, t->lengthSec());
        }
        if (t && e.kind == EntryKind::Imported) {
            t->name = e.name;
            t->artist = e.artist;
            if (!e.genre.empty()) t->genre = e.genre;
        }
        if (t && e.kind == EntryKind::Clip) {
            // Clips keep the tempo and beat grid they were cut with.
            if (e.bpm > 0) {
                t->bpm = e.bpm;
                t->firstBeatSec = e.firstBeatSec;
            }
            t->name = e.name;
            t->artist = e.artist;
            t->genre = e.genre;
            t->key = e.key;
            t->isClip = true;
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!t) {
        e.error = err.empty() ? "Failed to load" : err;
        e.state = EntryState::Failed;
        return;
    }
    e.bpm = t->bpm;
    e.firstBeatSec = t->firstBeatSec;
    e.lengthSec = t->lengthSec();
    e.progress = 1.0f;
    if (e.kind == EntryKind::Imported && !e.wanted) {
        // Analysis only: keep the numbers, not the (large) audio.
        e.state = EntryState::NotLoaded;
        return;
    }
    e.track = t;
    e.state = EntryState::Ready;
    evictUnused(&e);
}

void Library::evictUnused(const LibraryEntry* keep) {
    // Imported audio is big; keep only the most recently used few that nothing else holds.
    std::vector<LibraryEntry*> idle;
    for (auto& p : entries_) {
        LibraryEntry* e = p.get();
        if (e == keep || e->kind != EntryKind::Imported || e->state != EntryState::Ready || !e->track) continue;
        if (e->track.use_count() == 1) idle.push_back(e);
    }
    if (int(idle.size()) <= kKeepLoaded) return;
    std::sort(idle.begin(), idle.end(), [](const LibraryEntry* a, const LibraryEntry* b) { return a->lastUse > b->lastUse; });
    for (size_t i = size_t(kKeepLoaded); i < idle.size(); ++i) {
        idle[i]->track.reset();
        idle[i]->wanted = false;
        idle[i]->state = EntryState::NotLoaded;
    }
}
