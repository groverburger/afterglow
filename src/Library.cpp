#include "Library.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "SynthGen.h"

namespace fs = std::filesystem;

bool isAudioFile(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    for (auto& c : ext) c = char(std::tolower((unsigned char)c));
    return ext == ".wav" || ext == ".mp3" || ext == ".flac";
}

namespace {

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

}  // namespace

Library::Library() { worker_ = std::thread([this] { workerLoop(); }); }

Library::~Library() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    cv_.notify_all();
    worker_.join();
}

void Library::init(const std::string& musicDir, const std::string& clipDir) {
    clipDir_ = clipDir;
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
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.push_back(std::move(e));
    }
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
        return e->track;
    }
    request(i, true);
    return nullptr;
}

void Library::request(int i, bool urgent) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (i < 0 || i >= int(entries_.size())) return;
    LibraryEntry& e = *entries_[size_t(i)];
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

int Library::addFile(const std::string& path) {
    int idx;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < entries_.size(); ++i)
            if (entries_[i]->path == path) return int(i);
        auto e = std::make_unique<LibraryEntry>();
        e->name = fs::path(path).stem().string();
        e->artist = "Imported";
        e->genre = "Imported";
        e->key = "?";
        e->kind = EntryKind::Imported;
        e->path = path;
        entries_.push_back(std::move(e));
        idx = int(entries_.size()) - 1;
    }
    request(idx, false);  // analyse in the background so BPM shows up
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
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.push_back(std::move(e));
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
        std::lock_guard<std::mutex> lock(mutex_);
        --active_;
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
        t = loadAudioFile(e.path, &err);
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
    e.track = t;
    e.bpm = t->bpm;
    e.lengthSec = t->lengthSec();
    e.progress = 1.0f;
    e.state = EntryState::Ready;
}
