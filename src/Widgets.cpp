#include "Widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ui {

Theme theme;

namespace {
constexpr float kPi = 3.14159265f;

ImVec4 toVec(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }
}  // namespace

ImU32 withAlpha(ImU32 c, float a) {
    ImVec4 v = toVec(c);
    v.w *= std::clamp(a, 0.0f, 1.0f);
    return ImGui::ColorConvertFloat4ToU32(v);
}

ImU32 lerpColor(ImU32 a, ImU32 b, float t) {
    ImVec4 x = toVec(a), y = toVec(b);
    t = std::clamp(t, 0.0f, 1.0f);
    return ImGui::ColorConvertFloat4ToU32(
        ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t));
}

ImU32 hsv(float h, float s, float v, float a) {
    float r, g, b;
    h = h - std::floor(h);
    ImGui::ColorConvertHSVtoRGB(h, s, v, r, g, b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, a));
}

void Tip(const char* text) {
    if (text && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void Help(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    Tip(text);
}

bool ColoredButton(const char* label, ImU32 color, ImVec2 size, bool active) {
    ImVec4 c = toVec(color);
    ImVec4 base = active ? c : ImVec4(c.x * 0.35f, c.y * 0.35f, c.z * 0.35f, 1.0f);
    ImVec4 hov = active ? ImVec4(std::min(1.f, c.x * 1.15f), std::min(1.f, c.y * 1.15f), std::min(1.f, c.z * 1.15f), 1)
                        : ImVec4(c.x * 0.5f, c.y * 0.5f, c.z * 0.5f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, base);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hov);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, c);
    ImGui::PushStyleColor(ImGuiCol_Text, active ? ImVec4(0.05f, 0.05f, 0.08f, 1) : ImVec4(1, 1, 1, 1));
    bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return pressed;
}

bool Knob(const char* label, float* v, float def, float minV, float maxV, ImU32 color, float radius,
          const char* tooltip, bool locked, bool bipolar) {
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const char* shown = label;
    const char* hash = std::strstr(label, "##");
    std::string text = hash ? std::string(label, hash) : std::string(label);
    (void)shown;

    float lineH = ImGui::GetTextLineHeight();
    float width = std::max(radius * 2.0f, ImGui::CalcTextSize(text.c_str()).x);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 center(pos.x + width * 0.5f, pos.y + radius);
    ImGui::InvisibleButton(label, ImVec2(width, radius * 2.0f + lineH + 2.0f));
    bool changed = false;
    bool active = ImGui::IsItemActive();
    bool hovered = ImGui::IsItemHovered();

    if (!locked) {
        if (active && io.MouseDelta.y != 0.0f) {
            float speed = (maxV - minV) / (io.KeyShift ? 800.0f : 180.0f);
            *v = std::clamp(*v - io.MouseDelta.y * speed, minV, maxV);
            changed = true;
        }
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            *v = def;
            changed = true;
        }
        if (hovered && io.MouseWheel != 0.0f) {
            *v = std::clamp(*v + io.MouseWheel * (maxV - minV) * 0.02f, minV, maxV);
            changed = true;
        }
    }

    const float a0 = kPi * 0.75f, a1 = kPi * 2.25f;
    float t = (*v - minV) / (maxV - minV);
    float av = a0 + (a1 - a0) * t;
    ImU32 col = locked ? IM_COL32(255, 210, 60, 255) : color;

    dl->AddCircleFilled(center, radius, IM_COL32(28, 28, 38, 255), 32);
    dl->PathArcTo(center, radius - 3.0f, a0, a1, 32);
    dl->PathStroke(IM_COL32(60, 60, 75, 255), 0, 4.0f);
    float from = bipolar ? a0 + (a1 - a0) * ((def - minV) / (maxV - minV)) : a0;
    if (std::fabs(av - from) > 0.01f) {
        dl->PathArcTo(center, radius - 3.0f, std::min(from, av), std::max(from, av), 32);
        dl->PathStroke(col, 0, 4.0f);
    }
    dl->AddCircleFilled(center, radius * 0.62f, hovered || active ? IM_COL32(70, 70, 90, 255) : IM_COL32(50, 50, 64, 255), 24);
    ImVec2 tip(center.x + std::cos(av) * radius * 0.62f, center.y + std::sin(av) * radius * 0.62f);
    dl->AddLine(ImVec2(center.x + std::cos(av) * radius * 0.2f, center.y + std::sin(av) * radius * 0.2f), tip,
                IM_COL32(255, 255, 255, 230), 2.5f);
    // Neutral tick so users can see where "normal" is.
    float ad = a0 + (a1 - a0) * ((def - minV) / (maxV - minV));
    dl->AddLine(ImVec2(center.x + std::cos(ad) * (radius + 1), center.y + std::sin(ad) * (radius + 1)),
                ImVec2(center.x + std::cos(ad) * (radius + 4), center.y + std::sin(ad) * (radius + 4)),
                IM_COL32(200, 200, 200, 160), 1.5f);
    ImVec2 ts = ImGui::CalcTextSize(text.c_str());
    dl->AddText(ImVec2(center.x - ts.x * 0.5f, pos.y + radius * 2.0f + 2.0f),
                locked ? IM_COL32(255, 210, 60, 255) : IM_COL32(200, 200, 210, 255), text.c_str());

    if (hovered || active) {
        ImGui::BeginTooltip();
        if (locked) ImGui::TextColored(ImVec4(1, 0.82f, 0.2f, 1), "Controlled by the running transition");
        if (tooltip) ImGui::TextUnformatted(tooltip);
        ImGui::TextDisabled("Drag up/down (Shift = fine), scroll, double-click to reset");
        ImGui::EndTooltip();
    }
    return changed;
}

bool VFader(const char* id, float* v, float def, ImVec2 size, ImU32 color, bool locked) {
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, size);
    bool changed = false;
    if (!locked) {
        if (ImGui::IsItemActive() && io.MouseDelta.y != 0.0f) {
            *v = std::clamp(*v - io.MouseDelta.y / (size.y - 20.0f), 0.0f, 1.0f);
            changed = true;
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            *v = def;
            changed = true;
        }
        if (ImGui::IsItemHovered() && io.MouseWheel != 0.0f) {
            *v = std::clamp(*v + io.MouseWheel * 0.03f, 0.0f, 1.0f);
            changed = true;
        }
    }
    float cx = p.x + size.x * 0.5f;
    float top = p.y + 10.0f, bot = p.y + size.y - 10.0f;
    dl->AddRectFilled(ImVec2(cx - 3, top), ImVec2(cx + 3, bot), IM_COL32(10, 10, 14, 255), 3.0f);
    for (int i = 0; i <= 10; ++i) {
        float y = top + (bot - top) * i / 10.0f;
        dl->AddLine(ImVec2(cx - 12, y), ImVec2(cx - 7, y), IM_COL32(120, 120, 140, 160));
        dl->AddLine(ImVec2(cx + 7, y), ImVec2(cx + 12, y), IM_COL32(120, 120, 140, 160));
    }
    float y = bot - (bot - top) * *v;
    dl->AddRectFilled(ImVec2(cx - 2, y), ImVec2(cx + 2, bot), withAlpha(locked ? IM_COL32(255, 210, 60, 255) : color, 0.7f));
    ImU32 grip = locked ? IM_COL32(255, 210, 60, 255) : (ImGui::IsItemHovered() ? IM_COL32(230, 230, 240, 255) : IM_COL32(190, 190, 205, 255));
    dl->AddRectFilled(ImVec2(p.x + 2, y - 8), ImVec2(p.x + size.x - 2, y + 8), grip, 3.0f);
    dl->AddLine(ImVec2(p.x + 4, y), ImVec2(p.x + size.x - 4, y), IM_COL32(20, 20, 20, 255), 2.0f);
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        if (locked) ImGui::TextColored(ImVec4(1, 0.82f, 0.2f, 1), "Controlled by the running transition");
        ImGui::Text("Channel volume %d%%", int(*v * 100 + 0.5f));
        ImGui::TextDisabled("Drag, scroll, double-click to reset");
        ImGui::EndTooltip();
    }
    return changed;
}

bool Crossfader(const char* id, float* v, ImVec2 size, bool locked) {
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, size);
    bool changed = false;
    float left = p.x + 16, right = p.x + size.x - 16;
    if (!locked) {
        if (ImGui::IsItemActive() && io.MouseDelta.x != 0.0f) {
            *v = std::clamp(*v + io.MouseDelta.x / (right - left), 0.0f, 1.0f);
            changed = true;
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            *v = 0.5f;
            changed = true;
        }
    }
    float cy = p.y + size.y * 0.5f;
    dl->AddRectFilledMultiColor(ImVec2(left, cy - 3), ImVec2(right, cy + 3), theme.deck[0], theme.deck[1],
                                theme.deck[1], theme.deck[0]);
    dl->AddLine(ImVec2((left + right) * 0.5f, cy - 10), ImVec2((left + right) * 0.5f, cy + 10), IM_COL32(200, 200, 200, 120));
    dl->AddText(ImVec2(p.x, cy - 7), theme.deck[0], "A");
    dl->AddText(ImVec2(p.x + size.x - 9, cy - 7), theme.deck[1], "B");
    float x = left + (right - left) * *v;
    ImU32 grip = locked ? IM_COL32(255, 210, 60, 255) : (ImGui::IsItemHovered() ? IM_COL32(235, 235, 245, 255) : IM_COL32(195, 195, 210, 255));
    dl->AddRectFilled(ImVec2(x - 9, p.y + 2), ImVec2(x + 9, p.y + size.y - 2), grip, 3.0f);
    dl->AddLine(ImVec2(x, p.y + 4), ImVec2(x, p.y + size.y - 4), IM_COL32(20, 20, 20, 255), 2.0f);
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        if (locked) ImGui::TextColored(ImVec4(1, 0.82f, 0.2f, 1), "Controlled by the running transition");
        ImGui::TextUnformatted("Crossfader: slide left for deck A, right for deck B.");
        ImGui::TextDisabled("Double-click to centre");
        ImGui::EndTooltip();
    }
    return changed;
}

void Meter::push(float peak, float dt) {
    level = std::max(peak, level * std::exp(-dt * 6.0f));
    if (peak >= hold) {
        hold = peak;
        holdTime = 1.0f;
    } else {
        holdTime -= dt;
        if (holdTime <= 0) hold = std::max(level, hold * std::exp(-dt * 3.0f));
    }
}

void Meter::draw(ImVec2 size, bool vertical) const {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    auto norm = [](float lin) {
        float db = 20.0f * std::log10(std::max(lin, 1e-5f));
        return std::clamp((db + 48.0f) / 48.0f, 0.0f, 1.0f);
    };
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), IM_COL32(10, 10, 14, 255), 2.0f);
    const int segs = vertical ? int(size.y / 5.0f) : int(size.x / 5.0f);
    float lv = norm(level);
    for (int i = 0; i < segs; ++i) {
        float t = (i + 0.5f) / segs;
        ImU32 c = t > 0.94f ? IM_COL32(255, 60, 60, 255) : t > 0.8f ? IM_COL32(255, 210, 60, 255) : IM_COL32(60, 220, 110, 255);
        if (t > lv) c = withAlpha(c, 0.12f);
        if (vertical) {
            float y1 = p.y + size.y - i * 5.0f - 1.0f;
            dl->AddRectFilled(ImVec2(p.x + 1, y1 - 3.5f), ImVec2(p.x + size.x - 1, y1), c);
        } else {
            float x0 = p.x + i * 5.0f + 1.0f;
            dl->AddRectFilled(ImVec2(x0, p.y + 1), ImVec2(x0 + 3.5f, p.y + size.y - 1), c);
        }
    }
    float h = norm(hold);
    if (h > 0.01f) {
        if (vertical) {
            float y = p.y + size.y - h * size.y;
            dl->AddLine(ImVec2(p.x, y), ImVec2(p.x + size.x, y), IM_COL32(255, 255, 255, 200), 1.5f);
        } else {
            float x = p.x + h * size.x;
            dl->AddLine(ImVec2(x, p.y), ImVec2(x, p.y + size.y), IM_COL32(255, 255, 255, 200), 1.5f);
        }
    }
}

void DrawWaveform(ImDrawList* dl, const Track& t, ImVec2 p0, ImVec2 p1, double frameStart, double framesPerPixel,
                  float alpha) {
    const int cols = int(p1.x - p0.x);
    const float mid = (p0.y + p1.y) * 0.5f;
    const float half = (p1.y - p0.y) * 0.5f;
    const long numBins = long(t.wave.size());
    const ImU32 cLow = withAlpha(theme.waveLow, alpha), cMid = withAlpha(theme.waveMid, alpha),
                cHigh = withAlpha(theme.waveHigh, alpha);
    for (int x = 0; x < cols; ++x) {
        double f0 = frameStart + x * framesPerPixel;
        double f1 = f0 + framesPerPixel;
        long b0 = long(std::floor(f0 / kWaveBinFrames));
        long b1 = long(std::floor(f1 / kWaveBinFrames));
        if (b1 < 0 || b0 >= numBins) continue;
        b0 = std::max(0L, b0);
        b1 = std::min(numBins - 1, std::max(b0, b1));
        long step = std::max(1L, (b1 - b0) / 48);  // cap the work for zoomed-out views
        float lo = 0, mi = 0, hi = 0;
        for (long b = b0; b <= b1; b += step) {
            const WaveBin& w = t.wave[size_t(b)];
            lo = std::max(lo, w.low);
            mi = std::max(mi, w.mid);
            hi = std::max(hi, w.high);
        }
        float fx = p0.x + float(x);
        float hl = lo * half * 0.98f, hm = mi * half * 0.72f, hh = hi * half * 0.45f;
        dl->AddRectFilled(ImVec2(fx, mid - hl), ImVec2(fx + 1.0f, mid + hl), cLow);
        dl->AddRectFilled(ImVec2(fx, mid - hm), ImVec2(fx + 1.0f, mid + hm), cMid);
        dl->AddRectFilled(ImVec2(fx, mid - hh), ImVec2(fx + 1.0f, mid + hh), cHigh);
    }
}

void BeatPhase(double beatPos, bool playing, ImU32 color, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    double b = std::floor(beatPos);
    int inBar = int(((long long)b % 4 + 4) % 4);
    float frac = float(beatPos - b);
    float w = (size.x - 6.0f) / 4.0f;
    for (int i = 0; i < 4; ++i) {
        ImVec2 a(p.x + i * (w + 2.0f), p.y), c(a.x + w, p.y + size.y);
        ImU32 col = IM_COL32(40, 40, 52, 255);
        if (i == inBar) col = playing ? withAlpha(color, 1.0f - frac * 0.6f) : withAlpha(color, 0.6f);
        dl->AddRectFilled(a, c, col, 2.0f);
    }
}

}  // namespace ui
