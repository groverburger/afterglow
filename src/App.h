// Application state shared by the UI modules.
#pragma once
#include <string>
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

struct App {
    Engine engine;
    Library library;
    Visualizer vis;
    std::vector<TransitionDef> transitions;  // stock first, then custom
    int selectedTransition = 1;
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
    float dt = 1.0f / 60.0f;
    float time = 0;
    std::string userDir;  // per-user data folder, set in init()
    std::string musicDir() const { return userDir + "/music"; }
    ImFont* fontUi = nullptr;
    ImFont* fontBig = nullptr;

    // Lifecycle.
    void init();
    void frame();
    void shutdown();
    void onFilesDropped(const std::vector<std::string>& paths);

    // Helpers (UI must hold engine.mutex when calling engine-touching helpers).
    void toast(const std::string& text, ImU32 color = IM_COL32(255, 255, 255, 255));
    void requestLoad(int deck, int entry);  // asks for confirmation when the deck is live
    int smartTargetDeck() const;            // the deck a "load" should go to
    int liveDeck() const;                   // deck currently driving the mix, -1 if none
    int transitionOutDeck() const;
    void triggerTransition(bool allowLanding = true);
    // Deck that MIX would bring in, and whether a landing overlay should be shown on it.
    bool landingPreview(int deck, double* startFrame) const;
    void saveCustomTransitions();
    int nextAutoDjEntry();
    // Adds a clip to the library and saves it to disk.
    void saveClip(TrackPtr clip);

    // UI modules.
    void drawMenuBar(float& height);
    void drawWaveforms(ImVec2 pos, ImVec2 size);
    void drawDeck(int d, ImVec2 pos, ImVec2 size);
    void drawMixer(ImVec2 pos, ImVec2 size);
    void drawTransitionBar(ImVec2 pos, ImVec2 size);
    void drawBottomTabs(ImVec2 pos, ImVec2 size);
    void drawVisualizerPanel(ImVec2 pos, ImVec2 size);
    void drawLibrary();
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
};

// Camelot-wheel key compatibility; returns false when either key is unknown.
bool keysCompatible(const std::string& a, const std::string& b);
std::string formatTime(double sec);
bool beginPanel(const char* name, ImVec2 pos, ImVec2 size, bool scroll = false);
// Copies [a, b) frames of `src` into a new clip track with click-free fades.
TrackPtr makeClip(const Track& src, double a, double b, const std::string& name, float fadeMs);
