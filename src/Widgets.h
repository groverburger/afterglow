// Custom ImGui widgets: knobs, meters, waveforms, beat indicators.
#pragma once
#include "imgui.h"
#include "Track.h"

namespace ui {

struct Theme {
    ImU32 deck[2] = {IM_COL32(40, 200, 255, 255), IM_COL32(255, 80, 170, 255)};
    ImU32 waveLow = IM_COL32(30, 100, 255, 255);
    ImU32 waveMid = IM_COL32(255, 160, 40, 255);
    ImU32 waveHigh = IM_COL32(240, 240, 255, 255);
    ImU32 panelBg = IM_COL32(18, 18, 26, 255);
    ImU32 grid = IM_COL32(255, 255, 255, 40);
    ImU32 warn = IM_COL32(255, 70, 70, 255);
    ImU32 good = IM_COL32(80, 230, 120, 255);
};
extern Theme theme;

ImU32 withAlpha(ImU32 c, float a);
ImU32 lerpColor(ImU32 a, ImU32 b, float t);
ImU32 hsv(float h, float s, float v, float a = 1.0f);

// Rotary knob. Drag up/down, double-click resets to `def`. Returns true on change.
bool Knob(const char* label, float* v, float def, float minV, float maxV, ImU32 color, float radius = 18.0f,
          const char* tooltip = nullptr, bool locked = false, bool bipolar = false);

// Vertical fader with a big grip. Double-click resets.
bool VFader(const char* id, float* v, float def, ImVec2 size, ImU32 color, bool locked = false);

// Horizontal crossfader.
bool Crossfader(const char* id, float* v, ImVec2 size, bool locked = false);

// Level meter from a linear peak value (with its own decay/hold state).
struct Meter {
    float level = 0, hold = 0, holdTime = 0;
    void push(float peak, float dt);
    void draw(ImVec2 size, bool vertical = true) const;
};

// Draws frequency-coloured waveform columns for the frame range starting at
// `frameStart`, `framesPerPixel` frames per column.
void DrawWaveform(ImDrawList* dl, const Track& t, ImVec2 p0, ImVec2 p1, double frameStart, double framesPerPixel,
                  float alpha = 1.0f);

// Four-beat bar indicator.
void BeatPhase(double beatPos, bool playing, ImU32 color, ImVec2 size);

// Help marker "(?)" with a tooltip.
void Help(const char* text);
// Tooltip on the last item when hovered (also for disabled items).
void Tip(const char* text);

bool ColoredButton(const char* label, ImU32 color, ImVec2 size = ImVec2(0, 0), bool active = false);

}  // namespace ui
