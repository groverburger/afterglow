// Decks, mixer, scrolling waveforms and the transition bar.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "App.h"
#include "imgui.h"
#include "imgui_internal.h"  // BeginDragDropTargetCustom

using ui::theme;

namespace {

const char* kDeckPanel[2] = {"##deckA", "##deckB"};

// Accepts library rows dragged onto a rectangle; returns the entry index or -1.
int acceptLibraryDrop(ImVec2 p0, ImVec2 p1, ImGuiID id) {
    int result = -1;
    if (ImGui::BeginDragDropTargetCustom(ImRect(p0, p1), id)) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("LIB_ENTRY")) result = *(const int*)pl->Data;
        ImGui::EndDragDropTarget();
    }
    return result;
}

void drawBeatGrid(ImDrawList* dl, const Deck& dk, ImVec2 p0, ImVec2 p1, double frameStart, double fpp, bool labels) {
    const double f1 = frameStart + (p1.x - p0.x) * fpp;
    double b0 = std::ceil(dk.beatAt(frameStart)), b1 = dk.beatAt(f1);
    if (b1 - b0 > 400) return;
    for (double b = b0; b <= b1; b += 1.0) {
        float x = p0.x + float((dk.frameOfBeat(b) - frameStart) / fpp);
        bool bar = (long long)b % 4 == 0;
        dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), bar ? IM_COL32(255, 255, 255, 70) : IM_COL32(255, 255, 255, 22),
                    bar ? 1.5f : 1.0f);
        if (bar && labels && b >= 0) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%d", int(b / 4) + 1);
            dl->AddText(ImVec2(x + 3, p0.y + 1), IM_COL32(255, 255, 255, 110), buf);
        }
    }
}

void drawMarkers(ImDrawList* dl, const Deck& dk, ImVec2 p0, ImVec2 p1, double frameStart, double fpp) {
    auto xOf = [&](double f) { return p0.x + float((f - frameStart) / fpp); };
    if (dk.loopActive) {
        float a = std::max(p0.x, xOf(dk.loopIn)), b = std::min(p1.x, xOf(dk.loopOut));
        if (b > a) {
            dl->AddRectFilled(ImVec2(a, p0.y), ImVec2(b, p1.y), IM_COL32(60, 255, 120, 40));
            dl->AddLine(ImVec2(a, p0.y), ImVec2(a, p1.y), IM_COL32(60, 255, 120, 200), 2.0f);
            dl->AddLine(ImVec2(b, p0.y), ImVec2(b, p1.y), IM_COL32(60, 255, 120, 200), 2.0f);
        }
    }
    float cx = xOf(dk.cuePoint);
    if (cx >= p0.x && cx <= p1.x) {
        dl->AddTriangleFilled(ImVec2(cx - 5, p0.y), ImVec2(cx + 5, p0.y), ImVec2(cx, p0.y + 7), IM_COL32(255, 150, 30, 255));
        dl->AddLine(ImVec2(cx, p0.y), ImVec2(cx, p1.y), IM_COL32(255, 150, 30, 120));
    }
    static const ImU32 hc[kNumHotCues] = {IM_COL32(255, 80, 80, 255), IM_COL32(80, 200, 255, 255),
                                          IM_COL32(120, 255, 120, 255), IM_COL32(255, 220, 60, 255)};
    for (int i = 0; i < kNumHotCues; ++i) {
        if (!dk.hotCueSet[i]) continue;
        float x = xOf(dk.hotCues[i]);
        if (x < p0.x || x > p1.x) continue;
        dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), ui::withAlpha(hc[i], 0.7f), 1.5f);
        dl->AddRectFilled(ImVec2(x, p1.y - 13), ImVec2(x + 11, p1.y), hc[i], 2.0f);
        char buf[4];
        std::snprintf(buf, sizeof(buf), "%d", i + 1);
        dl->AddText(ImVec2(x + 2, p1.y - 14), IM_COL32(0, 0, 0, 255), buf);
    }
}

// Shows where a landing-mode blend will start and where it lands (the cue).
void drawLanding(ImDrawList* dl, const Deck& dk, ImVec2 p0, ImVec2 p1, double frameStart, double fpp, double startFrame,
                 bool labels) {
    auto xOf = [&](double f) { return p0.x + float((f - frameStart) / fpp); };
    float xs = xOf(startFrame), xe = xOf(dk.pos);
    float a = std::max(p0.x, xs), b = std::min(p1.x, xe);
    const ImU32 col = IM_COL32(255, 200, 60, 255);
    if (b > a) dl->AddRectFilledMultiColor(ImVec2(a, p0.y), ImVec2(b, p1.y), ui::withAlpha(col, 0.05f),
                                           ui::withAlpha(col, 0.28f), ui::withAlpha(col, 0.28f), ui::withAlpha(col, 0.05f));
    if (xs >= p0.x && xs <= p1.x) {
        dl->AddLine(ImVec2(xs, p0.y), ImVec2(xs, p1.y), ui::withAlpha(col, 0.8f), 1.5f);
        if (labels) dl->AddText(ImVec2(xs + 4, p1.y - 16), col, "blend starts");
    }
    if (xe >= p0.x && xe <= p1.x) {
        dl->AddLine(ImVec2(xe, p0.y), ImVec2(xe, p1.y), col, 2.5f);
        dl->AddTriangleFilled(ImVec2(xe, p0.y), ImVec2(xe + 12, p0.y + 6), ImVec2(xe, p0.y + 12), col);
        if (labels) dl->AddText(ImVec2(xe + 4, p0.y + 13), col, "lands here");
    }
}

}  // namespace

// --------------------------------------------------------- waveforms ----

void App::drawWaveforms(ImVec2 pos, ImVec2 size) {
    beginPanel("##waves", pos, size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c0 = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    const float gutter = 110.0f;
    const float laneH = (avail.y - 4.0f) * 0.5f;
    // Drag state per lane: 0 none, 1 scratch (hand on the record), 2 nudge / silent scrub (Shift).
    static int dragMode[2] = {0, 0};
    static float grabX[2] = {0, 0};
    static double grabPos[2] = {0, 0};

    for (int d = 0; d < 2; ++d) {
        Deck& dk = engine.decks[d];
        ImVec2 l0(c0.x, c0.y + d * (laneH + 4.0f)), l1(c0.x + gutter - 6, l0.y + laneH);
        ImVec2 w0(c0.x + gutter, l0.y), w1(c0.x + avail.x, l0.y + laneH);

        // Left gutter: deck letter, BPM, beat phase.
        dl->AddRectFilled(l0, l1, IM_COL32(22, 22, 32, 255), 4.0f);
        dl->AddRectFilled(l0, ImVec2(l0.x + 4, l1.y), theme.deck[d], 2.0f);
        ImFont* big = fontBig;
        char letter[2] = {char('A' + d), 0};
        dl->AddText(big, 26.0f, ImVec2(l0.x + 12, l0.y + 4), theme.deck[d], letter);
        if (dk.loaded()) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.1f", dk.effectiveBpm());
            dl->AddText(big, 18.0f, ImVec2(l0.x + 40, l0.y + 8), IM_COL32(255, 255, 255, 230), buf);
            ImGui::SetCursorScreenPos(ImVec2(l0.x + 12, l1.y - 18));
            ui::BeatPhase(dk.beatPos(), dk.playing, theme.deck[d], ImVec2(gutter - 30, 10));
        }

        // Waveform lane.
        ImGui::SetCursorScreenPos(w0);
        ImGui::PushID(d);
        ImGui::InvisibleButton("##lane", ImVec2(w1.x - w0.x, laneH));
        const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive(), activated = ImGui::IsItemActivated();
        ImGui::PopID();
        int dropped = acceptLibraryDrop(w0, w1, ImGui::GetID(d == 0 ? "dropWaveA" : "dropWaveB"));
        if (dropped >= 0) requestLoad(d, dropped);

        dl->AddRectFilled(w0, w1, IM_COL32(12, 12, 18, 255), 4.0f);
        if (!dk.loaded()) {
            const char* msg = pendingLoad[d] >= 0 ? "Loading..." : "Empty - drag a track here from the Library";
            ImVec2 ts = ImGui::CalcTextSize(msg);
            dl->AddText(ImVec2((w0.x + w1.x - ts.x) * 0.5f, (w0.y + w1.y - ts.y) * 0.5f), IM_COL32(150, 150, 170, 200), msg);
            continue;
        }
        const float width = w1.x - w0.x;
        const double rate = std::max(0.05, 1.0 + dk.tempo);
        const double fpp = waveSeconds * kSampleRate * rate / width;
        const double frameStart = dk.pos - width * 0.5 * fpp;

        dl->PushClipRect(w0, w1, true);
        drawBeatGrid(dl, dk, w0, w1, frameStart, fpp, true);
        ui::DrawWaveform(dl, *dk.track, ImVec2(w0.x, w0.y + 2), ImVec2(w1.x, w1.y - 2), frameStart, fpp);
        drawMarkers(dl, dk, w0, w1, frameStart, fpp);
        double landStart;
        if (landingPreview(d, &landStart)) drawLanding(dl, dk, w0, w1, frameStart, fpp, landStart, true);
        // Dim the part that has already played.
        dl->AddRectFilled(w0, ImVec2(w0.x + width * 0.5f, w1.y), IM_COL32(0, 0, 0, 70));
        float cx = w0.x + width * 0.5f;
        dl->AddLine(ImVec2(cx, w0.y), ImVec2(cx, w1.y), ui::withAlpha(theme.deck[d], 0.35f), 7.0f);
        dl->AddLine(ImVec2(cx, w0.y), ImVec2(cx, w1.y), IM_COL32(255, 255, 255, 255), 2.0f);
        dl->PopClipRect();

        // Drag = hand on the record: the track follows the mouse (scratch), playing or not,
        // and carries on from where you let go. Shift+drag = gentle nudge / silent scrub.
        ImGuiIO& io = ImGui::GetIO();
        if (activated) {
            grabX[d] = io.MousePos.x;
            grabPos[d] = dk.pos;
            dragMode[d] = io.KeyShift ? 2 : 1;
        }
        if (active && dragMode[d] == 1) {
            const double target = std::clamp(grabPos[d] - double(io.MousePos.x - grabX[d]) * fpp, 0.0, double(dk.track->frames()) - 1);
            engine.scratch(d, true, target);
        } else if (active && dragMode[d] == 2) {
            if (dk.playing) dk.nudge = std::clamp(-io.MouseDelta.x * 0.006, -0.2, 0.2);
            else dk.pos = std::clamp(dk.pos - io.MouseDelta.x * fpp, 0.0, double(dk.track->frames()) - 1);
        } else if (dragMode[d] != 0) {
            if (dragMode[d] == 1) engine.scratch(d, false, 0);
            else dk.nudge = 0;
            dragMode[d] = 0;
        }
        if (dk.scratching) {
            // Hand on the platter.
            dl->AddText(ImVec2(w0.x + 8, w0.y + 4), IM_COL32(255, 255, 255, 200), dk.slip ? "SCRATCH (slip)" : "SCRATCH");
        }
        if (hovered && io.MouseWheel != 0.0f) waveSeconds = std::clamp(waveSeconds * (io.MouseWheel > 0 ? 0.85f : 1.18f), 2.0f, 40.0f);
        if (hovered && !active)
            ui::Tip("Grab and drag the waveform to move the record: scratch, rewind or push it forward - it plays on from where you let go.\n"
                    "Hold still to stop the record. Shift+drag: gentle speed nudge instead. Scroll to zoom.");
    }
    ImGui::End();
}

// --------------------------------------------------------------- deck ----

void App::drawDeck(int d, ImVec2 pos, ImVec2 size) {
    Deck& dk = engine.decks[d];
    const ImU32 col = theme.deck[d];
    beginPanel(kDeckPanel[d], pos, size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    {
        ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        dl->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + 3), col);
        int dropped = acceptLibraryDrop(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), ImGui::GetID("deckdrop"));
        if (dropped >= 0) requestLoad(d, dropped);
    }

    // Header.
    ImGui::PushFont(fontBig, 30.0f);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col), "%c", 'A' + d);
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::BeginGroup();
    if (dk.loaded()) {
        ImGui::PushFont(fontBig, 19.0f);
        ImGui::TextUnformatted(dk.track->name.c_str());
        ImGui::PopFont();
        ImGui::TextDisabled("%s  |  %s  |  %s", dk.track->artist.c_str(), dk.track->genre.c_str(), dk.track->key.c_str());
    } else {
        ImGui::PushFont(fontBig, 19.0f);
        ImGui::TextDisabled("Empty deck");
        ImGui::PopFont();
        ImGui::TextDisabled("Double-click a track in the Library, or drag one here");
    }
    ImGui::EndGroup();
    if (dk.loaded()) {
        ImGui::SameLine(ImGui::GetWindowWidth() - 70);
        bool live = engine.deckAudible(d);
        ImGui::BeginDisabled(live || (engine.transitionBusy() && (engine.run.in == d || engine.run.out == d)));
        if (ImGui::Button("Eject")) {
            engine.ejectTrack(d);
            deckEntry[d] = -1;
        }
        ImGui::EndDisabled();
        ui::Tip(live ? "Can't eject while this deck is on air - fade it out first" : "Unload the track");
    }

    if (!dk.loaded()) {
        if (pendingLoad[d] >= 0) {
            LibraryEntry* e = library.entry(pendingLoad[d]);
            ImGui::Spacing();
            ImGui::Text("Preparing \"%s\"...", e ? e->name.c_str() : "?");
            ImGui::ProgressBar(e ? e->progress.load() : 0.0f, ImVec2(-1, 0));
        }
        ImGui::End();
        return;
    }

    // Tempo / time info line.
    const double rem = dk.remainingSec();
    ImGui::PushFont(fontBig, 24.0f);
    ImGui::Text("%.2f", dk.effectiveBpm());
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("BPM  (orig %.1f, %+.1f%%)", dk.track->bpm, dk.tempo * 100.0);
    if (!dk.track->isGenerated) {
        // Tempo detection can land on half or double time; let the user fix it.
        ImGui::SameLine();
        if (ImGui::SmallButton("x2")) dk.track->bpm *= 2.0;
        ui::Tip("Detected tempo is half of the real one? Double it.");
        ImGui::SameLine();
        if (ImGui::SmallButton("/2")) dk.track->bpm *= 0.5;
        ui::Tip("Detected tempo is double the real one? Halve it.");
    }
    ImGui::SameLine();
    char tbuf[64];
    std::snprintf(tbuf, sizeof(tbuf), "%s  /  -%s", formatTime(dk.timeSec()).c_str(), formatTime(rem).c_str());
    float tw = ImGui::CalcTextSize(tbuf).x;
    ImGui::SameLine(ImGui::GetWindowWidth() - tw - 14);
    bool ending = dk.playing && rem < 30.0;
    if (ending && std::fmod(time, 0.8f) < 0.4f) ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "%s", tbuf);
    else ImGui::TextUnformatted(tbuf);

    // Overview waveform with click-to-seek.
    {
        const float h = 46.0f;
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        ImGui::InvisibleButton("##overview", ImVec2(w, h));
        ImVec2 p1(p0.x + w, p0.y + h);
        const double frames = double(dk.track->frames());
        const double fpp = frames / w;
        dl->AddRectFilled(p0, p1, IM_COL32(12, 12, 18, 255), 3.0f);
        ui::DrawWaveform(dl, *dk.track, p0, p1, 0.0, fpp, 0.9f);
        drawMarkers(dl, dk, p0, p1, 0.0, fpp);
        double landStart;
        if (landingPreview(d, &landStart)) drawLanding(dl, dk, p0, p1, 0.0, fpp, landStart, false);
        float px = p0.x + float(dk.pos / fpp);
        dl->AddRectFilled(p0, ImVec2(px, p1.y), IM_COL32(0, 0, 0, 110));
        dl->AddLine(ImVec2(px, p0.y), ImVec2(px, p1.y), IM_COL32(255, 255, 255, 255), 2.0f);
        if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            float mx = std::clamp(ImGui::GetIO().MousePos.x - p0.x, 0.0f, w);
            if (ImGui::IsItemClicked() || ImGui::GetIO().MouseDelta.x != 0.0f) engine.seek(d, mx * fpp);
        }
        if (ImGui::IsItemHovered()) {
            float mx = ImGui::GetIO().MousePos.x - p0.x;
            dl->AddLine(ImVec2(p0.x + mx, p0.y), ImVec2(p0.x + mx, p1.y), IM_COL32(255, 255, 255, 90));
            ImGui::SetTooltip("Click to jump to %s%s", formatTime(mx * fpp / kSampleRate).c_str(),
                              engine.quantize && dk.playing ? " (stays on beat)" : "");
        }
    }

    // Beat position.
    {
        double b = dk.beatPos();
        ui::BeatPhase(b, dk.playing, col, ImVec2(120, 12));
        ImGui::SameLine();
        long long beat = (long long)std::floor(b);
        long long bar = beat >= 0 ? beat / 4 + 1 : 0;
        int inBar = int(((beat % 4) + 4) % 4) + 1;
        ImGui::TextDisabled("Bar %lld  Beat %d", bar, inBar);
        if (dk.sync && d != engine.masterDeck && engine.decks[engine.masterDeck].playing) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.6f, 1), "SYNCED to %c", 'A' + engine.masterDeck);
        } else if (d == engine.masterDeck && dk.playing) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "TEMPO MASTER");
        }
    }

    // Transport.
    const float bh = 42.0f;
    if (ui::ColoredButton(dk.playing ? "PAUSE" : "PLAY", ui::theme.good, ImVec2(92, bh), dk.playing)) engine.togglePlay(d);
    ui::Tip(dk.playing ? "Pause this deck" : "Start this deck. With SYNC on it lands exactly on the beat of the other deck.");
    ImGui::SameLine();
    if (ui::ColoredButton("CUE", IM_COL32(255, 150, 30, 255), ImVec2(62, bh))) engine.cueButton(d);
    ui::Tip("Paused: sets the cue point here (snapped to the beat).\nPlaying: jumps back to the cue point and stops.");
    ImGui::SameLine();
    if (ui::ColoredButton("SYNC", IM_COL32(80, 230, 140, 255), ImVec2(62, bh), dk.sync)) {
        dk.sync = !dk.sync;
        if (dk.sync) {
            std::string why;
            if (!engine.syncTempo(d, &why) && !why.empty()) toast(why, theme.warn);
            else if (!why.empty()) toast(why, IM_COL32(255, 210, 60, 255));
            if (dk.playing && engine.decks[1 - d].playing) engine.alignPhase(d, engine.decks[1 - d].beatPos(), dk.syncFold);
        }
    }
    ui::Tip("SYNC matches this deck's tempo and beats to the other deck automatically. Leave it on unless you want to beatmatch by ear.");
    ImGui::SameLine();

    // Tempo slider.
    ImGui::BeginGroup();
    const bool following = dk.sync && d != engine.masterDeck && engine.decks[engine.masterDeck].playing;
    float pct = float(dk.tempo * 100.0);
    float range = dk.tempoRange * 100.0f;
    ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - 110));
    ImGui::BeginDisabled(following);
    char fmt[32];
    std::snprintf(fmt, sizeof(fmt), "Tempo %%+.1f%%%%");
    if (ImGui::SliderFloat("##tempo", &pct, -range, range, fmt)) dk.tempo = pct / 100.0;
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) dk.tempo = 0;
    ImGui::EndDisabled();
    ui::Tip(following ? "Tempo is following the other deck because SYNC is on. Turn SYNC off to set it by hand."
                      : "Playback speed. Double-click to reset. (Changes pitch too, like a turntable.)");
    ImGui::SameLine();
    char rbuf[16];
    std::snprintf(rbuf, sizeof(rbuf), "+/-%d", int(dk.tempoRange * 100 + 0.5f));
    if (ImGui::Button(rbuf, ImVec2(52, 0))) dk.tempoRange = dk.tempoRange < 0.1f ? 0.16f : dk.tempoRange < 0.2f ? 0.5f : 0.08f;
    ui::Tip("Tempo slider range");
    ImGui::SameLine();
    if (ImGui::Button("0##t", ImVec2(28, 0))) dk.tempo = 0;
    ui::Tip("Reset tempo to the original speed");
    ImGui::TextDisabled("Nudge");
    ImGui::SameLine();
    ImGui::Button("<<##nudge", ImVec2(40, 0));
    bool slow = ImGui::IsItemActive();
    ui::Tip("Hold to slow down a little (fix drifting beats by ear)");
    ImGui::SameLine();
    ImGui::Button(">>##nudge", ImVec2(40, 0));
    bool fast = ImGui::IsItemActive();
    ui::Tip("Hold to speed up a little");
    if (slow || fast) dk.nudge = fast ? 0.04 : -0.04;
    else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && std::fabs(dk.nudge) == 0.04) dk.nudge = 0;
    ImGui::EndGroup();

    // Hot cues.
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("HOT CUES");
    static const ImU32 hc[kNumHotCues] = {IM_COL32(255, 80, 80, 255), IM_COL32(80, 200, 255, 255),
                                          IM_COL32(120, 255, 120, 255), IM_COL32(255, 220, 60, 255)};
    for (int i = 0; i < kNumHotCues; ++i) {
        ImGui::SameLine();
        char lbl[16];
        std::snprintf(lbl, sizeof(lbl), dk.hotCueSet[i] ? "%d##hc" : "+%d##hc", i + 1);
        if (ui::ColoredButton(lbl, hc[i], ImVec2(44, 0), dk.hotCueSet[i])) engine.jumpHotCue(d, i);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) dk.hotCueSet[i] = false;
        ui::Tip(dk.hotCueSet[i] ? "Click: jump here (stays on beat). Right-click: delete."
                                : "Click to save the current position as a hot cue");
    }

    // Loops.
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("LOOP    ");
    static const float sizes[] = {0.5f, 1, 2, 4, 8, 16};
    static const char* names[] = {"1/2", "1", "2", "4", "8", "16"};
    for (int i = 0; i < 6; ++i) {
        ImGui::SameLine();
        char lbl[16];
        std::snprintf(lbl, sizeof(lbl), "%s##lp", names[i]);
        bool on = dk.loopActive && std::fabs(dk.loopBeats - sizes[i]) < 1e-3f;
        if (ui::ColoredButton(lbl, IM_COL32(60, 255, 120, 255), ImVec2(36, 0), on)) engine.setLoop(d, sizes[i]);
        ui::Tip("Loop this many beats (starts on the beat). Click again to release.");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!dk.loopActive);
    if (ImGui::Button("Exit##loop")) engine.exitLoop(d);
    ImGui::SameLine();
    if (ImGui::Button("Save as clip")) {
        char name[128];
        std::snprintf(name, sizeof(name), "%s loop %d", dk.track->name.c_str(), clip.counter++);
        saveClip(makeClip(*dk.track, dk.loopIn, dk.loopOut, name, 4.0f));
    }
    ui::Tip("Saves the looping section as a new clip in your Library (and as a WAV file)");
    ImGui::EndDisabled();

    // Turntable FX.
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("TURNTABLE");
    ImGui::SameLine();
    ImGui::BeginDisabled(!dk.playing || dk.motion != Motion::Normal);
    if (ImGui::Button("Power down")) engine.startMotion(d, Motion::Brake);
    ui::Tip("Slows the track to a halt like a turntable losing power");
    ImGui::SameLine();
    if (ImGui::Button("Backspin")) engine.startMotion(d, Motion::Backspin);
    ui::Tip("Spins the record backwards to a stop");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ui::ColoredButton("SLIP", IM_COL32(120, 200, 255, 255), ImVec2(48, 0), dk.slip)) dk.slip = !dk.slip;
    ui::Tip("Slip mode: while you scratch (drag the waveform), the track keeps running silently underneath.\n"
            "Let go and it continues exactly where it would have been, so your mix stays on the beat.");

    if (ending) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.35f, 0.35f, 1), "  Track ending - mix into the next one!");
    }
    ImGui::End();
}

// -------------------------------------------------------------- mixer ----

void App::drawMixer(ImVec2 pos, ImVec2 size) {
    beginPanel("##mixer", pos, size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorPos();
    const float W = ImGui::GetContentRegionAvail().x;
    const float H = size.y - origin.y - 8;

    const char* title = "MIXER";
    ImGui::SetCursorPos(ImVec2(origin.x + (W - ImGui::CalcTextSize(title).x) * 0.5f, origin.y));
    ImGui::TextDisabled("%s", title);

    const float knobR = 16.0f, cellW = 46.0f, cellH = knobR * 2 + ImGui::GetTextLineHeight() + 8;
    const float stripW = cellW * 2;
    const float masterW = W - stripW * 2;
    const float top = origin.y + 22;
    const float xfH = 58.0f;

    static const char* kTips[6] = {
        "HIGH: cymbals and hi-hats. Turn fully left to remove them.",
        "GAIN TRIM: tracks are auto-levelled already, so you rarely need this.",
        "MID: vocals, synths and snares.",
        "FILTER: left = muffled (low-pass), right = thin (high-pass), centre = off. Great for build-ups.",
        "LOW: kick and bass. Pro trick: kill the incoming track's bass, then swap basses on the drop.",
        "ECHO: tempo-synced echo. Tails keep ringing even after you pull the fader down."};

    for (int d = 0; d < 2; ++d) {
        Deck& dk = engine.decks[d];
        const float x0 = origin.x + (d == 0 ? 0.0f : stripW + masterW);
        ImU32 col = theme.deck[d];
        float* vals[6] = {&dk.eq[2], &dk.trim, &dk.eq[1], &dk.filter, &dk.eq[0], &dk.echo};
        const char* labels[6] = {"HIGH", "TRIM", "MID", "FILTER", "LOW", "ECHO"};
        const int autoIdx[6] = {3, -1, 2, 4, 1, 5};
        const float defs[6] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.0f};
        for (int k = 0; k < 6; ++k) {
            int row = k / 2, c = k % 2;
            ImGui::SetCursorPos(ImVec2(x0 + c * cellW + (cellW - knobR * 2) * 0.5f - 4, top + row * cellH));
            ImGui::PushID(d * 10 + k);
            bool locked = autoIdx[k] >= 0 && engine.paramAutomated(d, autoIdx[k]);
            char id[32];
            std::snprintf(id, sizeof(id), "%s##k", labels[k]);
            ui::Knob(id, vals[k], defs[k], 0.0f, 1.0f, k == 3 ? IM_COL32(180, 120, 255, 255) : col, knobR, kTips[k], locked,
                     k != 5);
            ImGui::PopID();
        }
        // Fader + meter.
        const float fy = top + 3 * cellH + 2;
        const float fh = std::max(60.0f, H - (fy - origin.y) - xfH);
        ImGui::SetCursorPos(ImVec2(x0 + 14, fy));
        ImGui::PushID(d);
        ui::VFader("##vol", &dk.volume, 1.0f, ImVec2(38, fh), col, engine.paramAutomated(d, 0));
        ImGui::PopID();
        ImGui::SetCursorPos(ImVec2(x0 + 60, fy + 6));
        deckMeters[d].draw(ImVec2(10, fh - 12));
    }

    // Master column.
    {
        const float mx = origin.x + stripW + 6, mw = masterW - 12;
        ImGui::SetCursorPos(ImVec2(mx + (mw - 44) * 0.5f, top));
        ui::Knob("MASTER", &engine.masterVolume, 0.8f, 0.0f, 1.0f, IM_COL32(230, 230, 240, 255), 20.0f,
                 "Main output volume. The built-in limiter stops it from ever distorting.");
        const float my = top + 70;
        const float mh = std::max(60.0f, H - (my - origin.y) - xfH - 56);
        ImGui::SetCursorPos(ImVec2(mx + mw * 0.5f - 13, my));
        masterMeters[0].draw(ImVec2(11, mh));
        ImGui::SetCursorPos(ImVec2(mx + mw * 0.5f + 2, my));
        masterMeters[1].draw(ImVec2(11, mh));
        ImGui::SetCursorPos(ImVec2(mx + (mw - 50) * 0.5f, my + mh + 4));
        ImVec2 lp = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(50, 16));
        dl->AddRectFilled(lp, ImVec2(lp.x + 50, lp.y + 16), ui::lerpColor(IM_COL32(50, 30, 30, 255), IM_COL32(255, 60, 60, 255), limiterLight), 3);
        dl->AddText(ImVec2(lp.x + 9, lp.y + 1), IM_COL32(255, 255, 255, 200), "LIMIT");
        ui::Tip("Lights up when the safety limiter is catching peaks. Occasional flashes are fine.");
        ImGui::SetCursorPos(ImVec2(mx, my + mh + 24));
        if (ImGui::Button("Reset FX", ImVec2(mw, 0))) engine.resetAllFx();
        ui::Tip("Panic button: puts all EQs, filters and echoes back to normal");
    }

    // Crossfader.
    ImGui::SetCursorPos(ImVec2(origin.x, origin.y + H - xfH + 6));
    ui::Crossfader("##xf", &engine.crossfader, ImVec2(W, 26), engine.crossfaderAutomated());
    const char* curves[] = {"Smooth", "Dipless", "Cut"};
    ImGui::SetNextItemWidth(W * 0.5f);
    ImGui::SetCursorPosX(origin.x + W * 0.25f);
    ImGui::Combo("##curve", &engine.xfCurve, curves, 3);
    ui::Tip("Crossfader curve.\nSmooth: gentle blend (default).\nDipless: both decks full in the middle.\nCut: hard switch for scratch-style cuts.");
    ImGui::End();
}

// ---------------------------------------------------------- transition ----

// Transition picker: a grid of cards, each with a preview of its automation curves.
void App::drawTransitionPicker() {
    // Centred over the whole app, like an overlay.
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(18, 17, 26, 250));
    const bool open = ImGui::BeginPopup("##trpicker");
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (!open) return;

    const int out = transitionOutDeck(), in = 1 - out;
    const Deck& inDk = engine.decks[in];
    double ratio = 1, fold = 1;
    const bool bigGap = engine.decks[out].playing && inDk.loaded() && !inDk.playing && engine.computeSync(in, &ratio, &fold) &&
                        std::fabs(ratio - 1.0) > 0.16 && std::fabs((1.0 + inDk.tempo) / ratio - 1.0) > 0.002;
    auto anyTempo = [](const TransitionDef& d) { return !d.overlaps() || d.lane(Param::Tempo).enabled; };
    auto hasFx = [](const TransitionDef& d) { return d.outEffect != OutEffect::None || d.inEffect != InEffect::None; };

    ImGui::PushFont(fontBig, 19.0f);
    ImGui::TextUnformatted("Choose a transition");
    ImGui::PopFont();
    ImGui::SameLine(0, 24);
    bool anyCustom = false;
    for (const auto& t : transitions) anyCustom |= !t.stock;
    const char* filters[] = {"All", "Blends", "Any tempo", "Effects", "Custom"};
    const ImU32 filterCol[] = {IM_COL32(255, 140, 60, 255), IM_COL32(90, 170, 255, 255), IM_COL32(80, 230, 140, 255),
                               IM_COL32(255, 90, 170, 255), IM_COL32(180, 130, 255, 255)};
    for (int f = 0; f < 5; ++f) {
        if (f == 4 && !anyCustom) continue;
        ImGui::SameLine(0, 4);
        if (ui::ColoredButton(filters[f], filterCol[f], ImVec2(0, 0), pickerFilter == f)) pickerFilter = f;
    }
    if (pickerFilter == 4 && !anyCustom) pickerFilter = 0;
    if (bigGap) {
        ImGui::TextColored(ImVec4(0.45f, 1, 0.6f, 1), "Your decks are %.0f%% apart in tempo: the transitions with a green dot handle that best.",
                           std::fabs(ratio - 1.0) * 100.0);
    } else {
        ImGui::TextDisabled("Blends overlap both tracks and beat-match them. Any-tempo transitions cut or stop the old track, so they work across any tempo gap.");
    }
    ImGui::Spacing();

    std::vector<int> shown;
    for (int i = 0; i < int(transitions.size()); ++i) {
        const TransitionDef& t = transitions[size_t(i)];
        bool ok = pickerFilter == 0 || (pickerFilter == 1 && t.overlaps()) || (pickerFilter == 2 && anyTempo(t)) ||
                  (pickerFilter == 3 && hasFx(t)) || (pickerFilter == 4 && !t.stock);
        if (ok) shown.push_back(i);
    }

    const int cols = 6;
    const ImVec2 card(158, 116);
    const float gap = 8.0f;
    const int rows = (int(shown.size()) + cols - 1) / cols;
    const float gridW = cols * card.x + (cols - 1) * gap;
    // Fit the window: header and footer take about 190 px.
    const float maxGrid = std::max(card.y + gap, ds.y * 0.92f - 190.0f);
    const float gridH = std::min(float(rows) * (card.y + gap), maxGrid);
    int hovered = -1;
    ImGui::BeginChild("##trgrid", ImVec2(gridW + 6, gridH), ImGuiChildFlags_None);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (size_t k = 0; k < shown.size(); ++k) {
        const int i = shown[k];
        const TransitionDef& t = transitions[size_t(i)];
        if (k % cols) ImGui::SameLine(0, gap);
        ImGui::PushID(i);
        const ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + card.x, p0.y + card.y);
        const bool pick = ImGui::InvisibleButton("##card", card);
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hov) hovered = i;
        const bool sel = i == selectedTransition;
        const ImU32 accent = IM_COL32(255, 140, 60, 255);
        if (sel) {
            dl->AddRectFilledMultiColor(p0, p1, IM_COL32(70, 38, 30, 255), IM_COL32(70, 30, 60, 255), IM_COL32(40, 22, 40, 255),
                                        IM_COL32(40, 26, 26, 255));
        } else {
            dl->AddRectFilled(p0, p1, hov ? IM_COL32(40, 38, 56, 255) : IM_COL32(26, 25, 36, 255), 7.0f);
        }
        dl->AddRect(p0, p1, sel ? accent : hov ? IM_COL32(255, 255, 255, 120) : IM_COL32(255, 255, 255, 25), 7.0f, 0, sel ? 2.0f : 1.0f);
        drawTransitionThumb(dl, t, ImVec2(p0.x + 8, p0.y + 8), ImVec2(p1.x - 8, p0.y + 54), hov || sel ? 1.0f : 0.8f);
        dl->PushClipRect(p0, ImVec2(p1.x - 4, p1.y), true);
        dl->AddText(fontBig, 15.0f, ImVec2(p0.x + 9, p0.y + 60), IM_COL32(255, 255, 255, 240), t.name.c_str());
        dl->PopClipRect();
        char meta[48];
        if (t.beats < 4) std::snprintf(meta, sizeof(meta), "%d beat%s", t.beats, t.beats > 1 ? "s" : "");
        else std::snprintf(meta, sizeof(meta), "%d beats  -  %d bar%s", t.beats, t.beats / 4, t.beats / 4 > 1 ? "s" : "");
        dl->AddText(ImVec2(p0.x + 9, p0.y + 78), IM_COL32(255, 255, 255, 110), meta);
        // Tags.
        float tx = p0.x + 9;
        auto tag = [&](const char* text, ImU32 col) {
            ImVec2 ts = ImGui::CalcTextSize(text);
            const float sc = 0.8f, tw = ts.x * sc + 8, th = ts.y * sc + 2;
            ImVec2 a(tx, p1.y - th - 7), b(tx + tw, p1.y - 7);
            if (b.x > p1.x - 6) return;
            dl->AddRectFilled(a, b, ui::withAlpha(col, 0.18f), 4.0f);
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * sc, ImVec2(a.x + 4, a.y + 1), col, text);
            tx = b.x + 4;
        };
        if (t.overlaps() && !t.lane(Param::Tempo).enabled) tag("BLEND", filterCol[1]);
        if (anyTempo(t)) tag("ANY TEMPO", filterCol[2]);
        if (hasFx(t)) tag("FX", filterCol[3]);
        if (!t.stock) tag("CUSTOM", filterCol[4]);
        if (bigGap && anyTempo(t)) {
            dl->AddCircleFilled(ImVec2(p1.x - 12, p0.y + 12), 7.0f, ui::withAlpha(filterCol[2], 0.3f));
            dl->AddCircleFilled(ImVec2(p1.x - 12, p0.y + 12), 4.0f, filterCol[2]);
        }
        if (pick) {
            selectedTransition = i;
            if (!scaleAllowed(transitionScale)) transitionScale = 1.0f;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndChild();

    // Details of the hovered (or selected) card.
    ImGui::Separator();
    const TransitionDef& info = transitions[size_t(hovered >= 0 ? hovered : selectedTransition)];
    ImGui::PushTextWrapPos(gridW);
    ImGui::TextColored(ImVec4(1, 0.7f, 0.45f, 1), "%s", info.name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%d beats%s%s", info.beats, info.outEffect != OutEffect::None ? "  -  out effect" : "",
                        info.inEffect == InEffect::SpinUp ? "  -  spin-up start" : "");
    ImGui::TextWrapped("%s", info.description.empty() ? "Custom transition." : info.description.c_str());
    ImGui::PopTextWrapPos();
    if (ImGui::SmallButton("Open in Transition Editor")) {
        trEdit.selected = hovered >= 0 ? hovered : selectedTransition;
        requestTab = 2;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("  Lines: white = crossfader, colours = EQ / filter / echo / tempo. Green tick = new track starts, red = stop effect.");
    ImGui::EndPopup();
}

void App::drawTransitionBar(ImVec2 pos, ImVec2 size) {
    beginPanel("##transition", pos, size);
    const bool busy = engine.transitionBusy();
    const int out = busy ? engine.run.out : transitionOutDeck();
    const int in = 1 - out;
    const bool outPlaying = engine.decks[out].playing;

    // Transition card: opens the picker.
    selectedTransition = std::clamp(selectedTransition, 0, int(transitions.size()) - 1);
    const TransitionDef def = activeTransition();
    {
        const TransitionDef& base = transitions[size_t(selectedTransition)];
        const ImVec2 c0 = ImGui::GetCursorScreenPos(), cs(260, size.y - 16);
        const bool clicked = ImGui::InvisibleButton("##trcard", cs);
        const bool hov = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 c1(c0.x + cs.x, c0.y + cs.y);
        const ImU32 accent = IM_COL32(255, 140, 60, 255);
        dl->AddRectFilled(c0, c1, hov ? IM_COL32(40, 34, 52, 255) : IM_COL32(28, 26, 38, 255), 6.0f);
        dl->AddRect(c0, c1, ui::withAlpha(accent, hov ? 0.9f : 0.45f), 6.0f, 0, 1.5f);
        drawTransitionThumb(dl, base, ImVec2(c0.x + 6, c0.y + 6), ImVec2(c0.x + 78, c1.y - 6));
        dl->AddText(ImVec2(c0.x + 86, c0.y + 4), IM_COL32(255, 255, 255, 110), "TRANSITION");
        dl->AddText(fontBig, 17.0f, ImVec2(c0.x + 86, c0.y + 20), IM_COL32(255, 255, 255, 240), base.name.c_str());
        // Chevron.
        const float ax = c1.x - 16, ay = (c0.y + c1.y) * 0.5f;
        dl->AddTriangleFilled(ImVec2(ax - 5, ay - 3), ImVec2(ax + 5, ay - 3), ImVec2(ax, ay + 3), ui::withAlpha(accent, 0.9f));
        if (clicked) ImGui::OpenPopup("##trpicker");
        if (hov) ui::Tip((base.description + "\n\nClick to browse all transitions.").c_str());
        drawTransitionPicker();
    }
    ImGui::SameLine(0, 14);

    // Length: the transition's own length times a multiplier.
    {
        ImGui::BeginGroup();
        const Deck& o = engine.decks[out];
        double spb = o.loaded() ? o.secPerBeat() / std::max(0.25, o.rate()) : 0.5;
        ImGui::TextDisabled("LENGTH");
        ImGui::SameLine();
        if (transitionScale != 1.0f && scaleAllowed(transitionScale))
            ImGui::TextColored(ImVec4(1, 0.75f, 0.35f, 1), "%d beats  %.1fs", def.beats, def.beats * spb);
        else
            ImGui::Text("%d beats  %.1fs", def.beats, def.beats * spb);
        static const float kScales[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
        static const char* kLabels[] = {"1/4", "1/2", "x1", "x2", "x4"};
        for (int k = 0; k < 5; ++k) {
            if (k) ImGui::SameLine(0, 2);
            const bool ok = scaleAllowed(kScales[k]);
            ImGui::BeginDisabled(!ok || busy);
            char lbl[16];
            std::snprintf(lbl, sizeof(lbl), "%s##len%d", kLabels[k], k);
            if (ui::ColoredButton(lbl, IM_COL32(255, 140, 60, 255), ImVec2(36, 0), transitionScale == kScales[k] && ok))
                transitionScale = kScales[k];
            ImGui::EndDisabled();
            if (ok) {
                char tip[96];
                std::snprintf(tip, sizeof(tip), "%d beats (%d bars). Shortcut: [ and ] halve / double.",
                              int(std::lround(transitions[size_t(selectedTransition)].beats * kScales[k])),
                              std::max(1, int(std::lround(transitions[size_t(selectedTransition)].beats * kScales[k])) / 4));
                ui::Tip(tip);
            } else {
                ui::Tip("Too short or too long for this transition");
            }
        }
        ImGui::EndGroup();
    }
    ImGui::SameLine(0, 18);

    // What the incoming deck's playhead means: where the blend starts, or where it lands.
    {
        ImGui::BeginGroup();
        ImGui::TextDisabled("PLAYHEAD OF %c MARKS", 'A' + in);
        const ImU32 c = IM_COL32(255, 200, 60, 255);
        if (ui::ColoredButton("Start##cue", c, ImVec2(62, 0), !landOnCue)) landOnCue = false;
        ui::Tip("The incoming track starts playing from its playhead when the blend begins.");
        ImGui::SameLine(0, 2);
        if (ui::ColoredButton("Landing##cue", c, ImVec2(72, 0), landOnCue)) landOnCue = true;
        ui::Tip("The incoming track ARRIVES at its playhead when the blend finishes.\n"
                "Put the playhead on the drop and it lands right as the old track is gone - "
                "the yellow zone on its waveform shows where the blend will begin.");
        ImGui::EndGroup();
    }
    ImGui::SameLine(0, 18);

    // The big button.
    {
        char lbl[64];
        if (!outPlaying && !busy) std::snprintf(lbl, sizeof(lbl), "START  %c", 'A' + in);
        else std::snprintf(lbl, sizeof(lbl), "MIX   %c  >>  %c", 'A' + out, 'A' + in);
        const bool canMix = !busy && engine.decks[in].loaded();
        ImGui::BeginDisabled(!canMix);
        ImGui::PushFont(fontBig, 20.0f);
        if (ui::ColoredButton(lbl, theme.deck[in], ImVec2(220, size.y - 16), canMix)) triggerTransition();
        ImGui::PopFont();
        ImGui::EndDisabled();
        if (!engine.decks[in].loaded()) ui::Tip("Load a track on the other deck first (double-click one in the Library)");
        else if (busy) ui::Tip("A transition is already running");
        else ui::Tip("Runs the selected transition: syncs the tempo, waits for the next bar, then automates the mixer for you. Shortcut: T");
    }
    ImGui::SameLine(0, 18);

    // Progress / status.
    ImGui::BeginGroup();
    const float progW = std::max(140.0f, ImGui::GetContentRegionAvail().x - 430);
    if (engine.run.state == TransitionRun::State::Armed) {
        ImGui::TextColored(ImVec4(1, 0.85f, 0.3f, 1), "Waiting for the next bar to start \"%s\"...", engine.run.def.name.c_str());
        float pulse = 0.5f + 0.5f * std::sin(time * 8.0f);
        ImGui::ProgressBar(pulse * 0.05f, ImVec2(progW, 0), "armed");
    } else if (engine.run.state == TransitionRun::State::Running) {
        ImGui::TextColored(ImVec4(0.5f, 1, 0.7f, 1), "%s: deck %c >> deck %c%s", engine.run.def.name.c_str(),
                           'A' + engine.run.out, 'A' + engine.run.in, engine.run.synced ? "  (beat-matched)" : "");
        char ov[32];
        std::snprintf(ov, sizeof(ov), "%d%%", int(engine.run.progress * 100));
        ImGui::ProgressBar(engine.run.progress, ImVec2(progW, 0), ov);
    } else {
        // Blends need matching tempos; warn before a big stretch and point at transitions that don't.
        double ratio = 1, fold = 1;
        const Deck& inDk = engine.decks[in];
        const bool bigGap = outPlaying && inDk.loaded() && !inDk.playing && engine.computeSync(in, &ratio, &fold) &&
                            std::fabs(ratio - 1.0) > 0.16 && std::fabs((1.0 + inDk.tempo) / ratio - 1.0) > 0.002;
        if (bigGap && def.overlaps() && !def.lane(Param::Tempo).enabled)
            ImGui::TextColored(ImVec4(1, 0.82f, 0.3f, 1), "Tempo gap %.0f%%: this blend will stretch deck %c. Tempo Ramp, Echo Out or Power Swap sound more natural.",
                               std::fabs(ratio - 1.0) * 100.0, 'A' + in);
        else
            ImGui::TextDisabled("%s", def.description.c_str());
        ImGui::ProgressBar(0.0f, ImVec2(progW, 0), "ready");
    }
    ImGui::EndGroup();
    if (busy) {
        ImGui::SameLine();
        if (ImGui::Button("Stop\nautomation", ImVec2(90, size.y - 16))) engine.cancelTransition();
        ui::Tip("Stops the automation and leaves every control where it is right now");
    }

    // Right side toggles.
    ImGui::SameLine(ImGui::GetWindowWidth() - 330);
    ImGui::BeginGroup();
    ImGui::Checkbox("Quantize", &engine.quantize);
    ui::Tip("Keeps everything on the beat: play, cues, loops and transitions snap to the beat grid. Leave this on!");
    ImGui::Checkbox("Auto DJ", &autoDj);
    ui::Tip("Let the computer DJ: it loads the next track and mixes it in automatically near the end of each song.");
    ImGui::EndGroup();
    ImGui::SameLine();
    if (ui::ColoredButton("PARTY\nMODE", IM_COL32(255, 80, 170, 255), ImVec2(80, size.y - 16))) partyMode = true;
    ui::Tip("Fullscreen visuals (F). Press Esc to come back.");
    ImGui::SameLine();
    if (ImGui::Button("Help", ImVec2(60, size.y - 16))) showHelp = true;
    ImGui::End();
}
