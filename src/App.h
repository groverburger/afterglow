// Application state shared by the UI modules.
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Engine.h"
#include "Library.h"
#include "Transitions.h"
#include "Visuals.h"
#include "Widgets.h"

struct ImFont;

struct Toast {
    std::string text;
    float time = 4.0f;
    ImU32 color = IM_COL32(255, 255, 255, 255);
};

struct ClipEditorState {
    int entry = -1;         // library entry being edited
    double viewStart = 0;   // visible frame range
    double viewEnd = 0;
    double selA = -1, selB = -1;
    bool dragging = false;
    int dragMode = 0;       // 0 new selection, 1 move start handle, 2 move end handle
    int snap = 1;           // 0 off, 1 beat, 2 bar
    char name[96] = "";
    float fadeMs = 8.0f;
    int counter = 1;
};

struct TransitionEditorState {
    int selected = 0;
    int lane = int(Param::Crossfader);
    int dragKey = -1;
    bool dirty = false;
};

enum class AutoDjOrder { Library, Shuffle, ByTempo };

// What the Library table shows (and Auto DJ plays from).
struct LibrarySource {
    enum Kind { All, BuiltIn, Clips, Dropped, Folder };
    Kind kind = All;
    std::string root;  // Folder: music folder
    std::string rel;   // Folder: sub-folder ("" = the whole music folder)
};

// Sub-folder tree of one music folder, rebuilt when the library changes.
struct FolderNode {
    std::string name, rel;
    int count = 0;  // tracks in this folder and below
    std::vector<FolderNode> kids;
};

struct SetInfo {
    std::string name;
    std::string path;
    double lengthSec = 0;
};

struct App {
    Engine engine;
    Library library;
    Visualizer vis;
    std::vector<TransitionDef> transitions;  // stock first, then custom
    int selectedTransition = 1;
    float transitionScale = 1.0f;  // length multiplier applied to the selected transition (1/4 .. 4)
    int pickerFilter = 0;          // transition picker: 0 all, 1 blends, 2 any tempo, 3 effects, 4 custom
    bool landOnCue = false;  // incoming deck's cue marks where the blend ENDS instead of starts

    int pendingLoad[2] = {-1, -1};  // library entry waiting to be loaded
    int deckEntry[2] = {-1, -1};    // library entry currently on each deck
    int confirmDeck = -1, confirmEntry = -1;
    ui::Meter deckMeters[2], masterMeters[2];
    float limiterLight = 0;
    float waveSeconds = 8.0f;  // visible time in the scrolling waveforms
    std::vector<Toast> toasts;
    ClipEditorState clip;
    TransitionEditorState trEdit;
    bool autoDj = false;
    AutoDjOrder autoDjOrder = AutoDjOrder::Library;
    bool autoDjRandomTransition = false;
    int autoDjPlayed = 0;
    bool partyMode = false;
    bool showHelp = true;
    int bottomTab = 0;
    int requestTab = -1;
    char search[128] = "";
    LibrarySource libSource;
    std::vector<std::string> musicFolders;  // watched folders, saved in music_folders.txt
    std::vector<FolderNode> folderTrees;    // one per music folder
    uint64_t folderTreeGen = ~0ull;
    bool wantFolderPicker = false;          // opened after the frame releases the engine
    char folderPathInput[512] = "";
    float dt = 1.0f / 60.0f;
    float time = 0;
    std::string userDir;  // per-user data folder, set in init()
    std::string musicDir() const { return userDir + "/music"; }
    std::string setsDir() const { return userDir + "/sets"; }
    ImFont* fontUi = nullptr;
    ImFont* fontBig = nullptr;

    // Sets: recording, live replay and audio export.
    std::vector<SetInfo> sets;
    bool setsDirty = true;
    char recName[96] = "";
    std::string replayName;
    std::string pendingReplayPath;  // started after the UI frame, so it isn't mistaken for a take-over
    const Track* lastDeckTrack[2] = {nullptr, nullptr};
    std::thread exportThread;
    std::atomic<bool> exportRunning{false};
    std::atomic<float> exportProgress{0.0f};
    std::mutex exportMutex;
    std::string exportMessage;  // set by the export thread when it finishes
    std::string exportName;

    // Lifecycle.
    void init();
    void frame();
    void shutdown();
    void onFilesDropped(const std::vector<std::string>& paths);
    void afterFrame();  // runs without the engine lock (blocking dialogs)

    // Helpers (UI must hold engine.mutex when calling engine-touching helpers).
    void toast(const std::string& text, ImU32 color = IM_COL32(255, 255, 255, 255));
    void requestLoad(int deck, int entry);  // asks for confirmation when the deck is live
    int smartTargetDeck() const;            // the deck a "load" should go to
    int liveDeck() const;                   // deck currently driving the mix, -1 if none
    int transitionOutDeck() const;
    void triggerTransition(bool allowLanding = true);
    // The selected transition with the length multiplier applied (what MIX will run).
    TransitionDef activeTransition() const;
    bool scaleAllowed(float scale) const;  // would the selected transition stay within 1..256 beats?
    // Deck that MIX would bring in, and whether a landing overlay should be shown on it.
    bool landingPreview(int deck, double* startFrame) const;
    void saveCustomTransitions();
    int nextAutoDjEntry();
    // Music folders.
    void loadMusicFolders();
    void saveMusicFolders();
    bool addMusicFolder(const std::string& path);  // false (and a toast) if it isn't a usable folder
    void removeMusicFolder(const std::string& path);
    bool entryInSource(const LibraryEntry& e, const LibrarySource& src) const;
    std::string sourceName(const LibrarySource& src) const;
    // Adds a clip to the library and saves it to disk.
    void saveClip(TrackPtr clip);

    // UI modules.
    void drawMenuBar(float& height);
    void drawWaveforms(ImVec2 pos, ImVec2 size);
    void drawDeck(int d, ImVec2 pos, ImVec2 size);
    void drawMixer(ImVec2 pos, ImVec2 size);
    void drawTransitionBar(ImVec2 pos, ImVec2 size);
    void drawTransitionPicker();
    void drawBottomTabs(ImVec2 pos, ImVec2 size);
    void drawVisualizerPanel(ImVec2 pos, ImVec2 size);
    void drawLibrary();
    void drawLibrarySources();
    void drawClipEditor();
    void drawTransitionEditor();
    void drawAutoDj();
    void drawHelpWindow();
    void drawParty();
    void drawToasts();
    void drawConfirmModal();
    void handleShortcuts();
    void processPendingLoads();
    void updateAutoDj();
    void drawSets();
    void refreshSets();
    void installDemoSet();
    void playSet(const std::string& path);   // queues; starts after the current UI frame
    void startPendingReplay();
    void updateSets();                       // per frame: notes, take-over, load lookahead
    void stopAndSaveRecording();
    void exportSet(const SetInfo& info);
};

// Transition previews, shared by the transition bar picker and the editor.
ImU32 transitionLaneColor(int param);
void drawTransitionThumb(ImDrawList* dl, const TransitionDef& def, ImVec2 p0, ImVec2 p1, float alpha = 1.0f);
const char* lengthScaleLabel(float scale);  // "x1/4", "x1/2", "x2"...

// Shows a folder in Finder / Explorer / the file manager.
void revealFolder(const std::string& path);

// Camelot-wheel key compatibility; returns false when either key is unknown.
bool keysCompatible(const std::string& a, const std::string& b);
std::string formatTime(double sec);
bool beginPanel(const char* name, ImVec2 pos, ImVec2 size, bool scroll = false);
// Copies [a, b) frames of `src` into a new clip track with click-free fades.
TrackPtr makeClip(const Track& src, double a, double b, const std::string& name, float fadeMs);
