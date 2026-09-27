// Bottom panels: library, clip editor, transition editor, Auto DJ, visualiser, help.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "App.h"
#include "imgui.h"
#include "sokol_app.h"

using ui::theme;

namespace {

double beatOf(const Track& t, double frame) { return (frame / kSampleRate - t.firstBeatSec) / t.secPerBeat(); }
double frameOfBeat(const Track& t, double beat) { return (t.firstBeatSec + beat * t.secPerBeat()) * kSampleRate; }

std::string barBeat(const Track& t, double frame) {
    double b = beatOf(t, frame);
    long long whole = (long long)std::floor(b + 1e-6);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "bar %lld.%lld", whole >= 0 ? whole / 4 + 1 : 0, ((whole % 4) + 4) % 4 + 1);
    return buf;
}

bool tempoCompatible(double a, double b) {
    if (a <= 0 || b <= 0) return false;
    for (double f : {0.5, 1.0, 2.0})
        if (std::fabs(a * f / b - 1.0) < 0.06) return true;
    return false;
}

ImU32 laneColor(int p) {
    static const ImU32 c[kNumParams] = {
        IM_COL32(255, 255, 255, 255),                                // crossfader
        IM_COL32(255, 120, 120, 255), IM_COL32(120, 255, 160, 255),  // volumes
        IM_COL32(80, 120, 255, 255),  IM_COL32(80, 200, 255, 255),   // lows
        IM_COL32(255, 170, 60, 255),  IM_COL32(255, 220, 90, 255),   // mids
        IM_COL32(230, 230, 255, 255), IM_COL32(200, 255, 255, 255),  // highs
        IM_COL32(190, 120, 255, 255), IM_COL32(230, 150, 255, 255),  // filters
        IM_COL32(255, 90, 170, 255),  IM_COL32(255, 140, 200, 255),  // echoes
    };
    return c[p];
}

void valueLabels(Param p, const char** lo, const char** mid, const char** hi) {
    switch (p) {
        case Param::Crossfader: *lo = "Out deck"; *mid = "middle"; *hi = "In deck"; break;
        case Param::OutFilter:
        case Param::InFilter: *lo = "low-pass"; *mid = "off"; *hi = "high-pass"; break;
        case Param::OutEcho:
        case Param::InEcho:
        case Param::OutVolume:
        case Param::InVolume: *lo = "0%"; *mid = "50%"; *hi = "100%"; break;
        default: *lo = "kill"; *mid = "normal"; *hi = "boost"; break;
    }
}

}  // namespace

// ---------------------------------------------------------------- clips ----

TrackPtr makeClip(const Track& src, double a, double b, const std::string& name, float fadeMs) {
    const double last = double(src.frames());
    a = std::clamp(std::floor(a), 0.0, last);
    b = std::clamp(std::floor(b), 0.0, last);
    if (b - a < 16) return nullptr;
    auto t = std::make_shared<Track>();
    const size_t n = size_t(b - a);
    t->samples.assign(src.samples.begin() + ptrdiff_t(a) * 2, src.samples.begin() + ptrdiff_t(a) * 2 + ptrdiff_t(n) * 2);
    // Equal-power fades so the clip never clicks.
    size_t fade = std::min(n / 4, size_t(fadeMs * kSampleRate / 1000.0f));
    for (size_t i = 0; i < fade; ++i) {
        float g = std::sin(float(i) / float(fade) * 1.5707963f);
        for (int ch = 0; ch < 2; ++ch) {
            t->samples[i * 2 + size_t(ch)] *= g;
            t->samples[(n - 1 - i) * 2 + size_t(ch)] *= g;
        }
    }
    t->name = name;
    t->artist = "Clip of " + src.name;
    t->genre = "Clip";
    t->key = src.key;
    t->bpm = src.bpm;
    double ba = beatOf(src, a);
    double next = std::ceil(ba - 1e-4);
    t->firstBeatSec = std::max(0.0, (next - ba) * src.secPerBeat());
    t->isClip = true;
    analyzeTrack(*t);
    return t;
}

void App::saveClip(TrackPtr c) {
    if (!c) {
        toast("That selection is too short to be a clip", theme.warn);
        return;
    }
    std::string err;
    bool ok = library.persistClip(*c, &err);
    library.addClip(c);
    if (ok) toast("Saved clip \"" + c->name + "\" to the Library", theme.good);
    else toast("Clip added, but saving the WAV failed: " + err, theme.warn);
}

// ------------------------------------------------------------ bottom tabs ----

void App::drawBottomTabs(ImVec2 pos, ImVec2 size) {
    beginPanel("##bottom", pos, size);
    if (ImGui::BeginTabBar("##tabs")) {
        const char* names[4] = {"Library", "Clip Editor", "Transition Editor", "Auto DJ"};
        for (int i = 0; i < 4; ++i) {
            ImGuiTabItemFlags f = requestTab == i ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(names[i], nullptr, f)) {
                bottomTab = i;
                switch (i) {
                    case 0: drawLibrary(); break;
                    case 1: drawClipEditor(); break;
                    case 2: drawTransitionEditor(); break;
                    case 3: drawAutoDj(); break;
                }
                ImGui::EndTabItem();
            }
        }
        requestTab = -1;
        ImGui::EndTabBar();
    }
    ImGui::End();
}

// ---------------------------------------------------------------- library ----

void App::drawLibrary() {
    ImGui::SetNextItemWidth(260);
    ImGui::InputTextWithHint("##search", "Search title, artist, genre...", search, sizeof(search));
    ImGui::SameLine();
    ImGui::TextDisabled("Double-click a track to load it on the free deck. Drop WAV/MP3/FLAC files on the window to import.");

    const int n = library.size();
    const int live = liveDeck();
    const Track* liveTrack = live >= 0 ? engine.decks[live].track.get() : nullptr;

    std::vector<int> rows;
    std::string q = search;
    for (auto& c : q) c = char(std::tolower((unsigned char)c));
    for (int i = 0; i < n; ++i) {
        LibraryEntry* e = library.entry(i);
        if (!q.empty()) {
            std::string hay = e->name + " " + e->artist + " " + e->genre;
            for (auto& c : hay) c = char(std::tolower((unsigned char)c));
            if (hay.find(q) == std::string::npos) continue;
        }
        rows.push_back(i);
    }

    ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV |
                         ImGuiTableFlags_Sortable | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##lib", 8, tf)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Load", ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed, 64);
    ImGui::TableSetupColumn("Title", ImGuiTableColumnFlags_DefaultSort, 3.0f);
    ImGui::TableSetupColumn("Artist", 0, 2.0f);
    ImGui::TableSetupColumn("Genre", 0, 1.6f);
    ImGui::TableSetupColumn("BPM", ImGuiTableColumnFlags_WidthFixed, 56);
    ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 56);
    ImGui::TableSetupColumn("Length", ImGuiTableColumnFlags_WidthFixed, 70);
    ImGui::TableSetupColumn("Match", ImGuiTableColumnFlags_WidthFixed, 90);
    ImGui::TableHeadersRow();

    if (ImGuiTableSortSpecs* ss = ImGui::TableGetSortSpecs(); ss && ss->SpecsCount > 0) {
        const ImGuiTableColumnSortSpecs& s = ss->Specs[0];
        std::stable_sort(rows.begin(), rows.end(), [&](int a, int b) {
            LibraryEntry *x = library.entry(a), *y = library.entry(b);
            int c = 0;
            switch (s.ColumnIndex) {
                case 1: c = x->name.compare(y->name); break;
                case 2: c = x->artist.compare(y->artist); break;
                case 3: c = x->genre.compare(y->genre); break;
                case 4: c = x->bpm < y->bpm ? -1 : x->bpm > y->bpm ? 1 : 0; break;
                case 5: c = x->key.compare(y->key); break;
                case 6: c = x->lengthSec < y->lengthSec ? -1 : x->lengthSec > y->lengthSec ? 1 : 0; break;
                default: c = a - b; break;
            }
            // Title sort keeps the original library order (the stock set list).
            if (s.ColumnIndex == 1 && s.SortDirection == ImGuiSortDirection_Ascending) c = a - b;
            return s.SortDirection == ImGuiSortDirection_Ascending ? c < 0 : c > 0;
        });
    }

    for (int i : rows) {
        LibraryEntry* e = library.entry(i);
        ImGui::PushID(i);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        for (int d = 0; d < 2; ++d) {
            if (d) ImGui::SameLine(0, 4);
            char lbl[8];
            std::snprintf(lbl, sizeof(lbl), "%c", 'A' + d);
            if (ui::ColoredButton(lbl, theme.deck[d], ImVec2(26, 0), deckEntry[d] == i)) requestLoad(d, i);
            ui::Tip(d == 0 ? "Load on deck A" : "Load on deck B");
        }
        ImGui::TableSetColumnIndex(1);
        bool onDeck = deckEntry[0] == i || deckEntry[1] == i;
        if (onDeck) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme.deck[deckEntry[0] == i ? 0 : 1]));
        if (ImGui::Selectable(e->name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick |
                                                          ImGuiSelectableFlags_AllowOverlap)) {
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) requestLoad(smartTargetDeck(), i);
        }
        if (onDeck) ImGui::PopStyleColor();
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("LIB_ENTRY", &i, sizeof(int));
            ImGui::Text("Drop on a deck to load \"%s\"", e->name.c_str());
            ImGui::EndDragDropSource();
        }
        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(e->artist.c_str());
        ImGui::TableSetColumnIndex(3);
        ImGui::TextDisabled("%s", e->genre.c_str());
        ImGui::TableSetColumnIndex(4);
        if (e->bpm > 0) ImGui::Text("%.1f", e->bpm);
        else ImGui::TextDisabled("...");
        ImGui::TableSetColumnIndex(5);
        ImGui::TextUnformatted(e->key.c_str());
        ImGui::TableSetColumnIndex(6);
        EntryState st = e->state;
        if (st == EntryState::Loading) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%d%%", int(e->progress * 100));
        else if (st == EntryState::Queued) ImGui::TextDisabled("queued");
        else if (st == EntryState::Failed) {
            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "error");
            ui::Tip(e->error.c_str());
        } else if (e->lengthSec > 0) ImGui::TextUnformatted(formatTime(e->lengthSec).c_str());
        else ImGui::TextDisabled("-");
        ImGui::TableSetColumnIndex(7);
        if (liveTrack && !onDeck) {
            bool t = tempoCompatible(e->bpm, liveTrack->bpm), k = keysCompatible(e->key, liveTrack->key);
            if (t && k) ImGui::TextColored(ImVec4(0.4f, 1, 0.5f, 1), "great");
            else if (t) ImGui::TextColored(ImVec4(0.7f, 0.9f, 0.5f, 1), "tempo ok");
            else if (k) ImGui::TextColored(ImVec4(0.7f, 0.8f, 1, 1), "key ok");
            else ImGui::TextDisabled("-");
            ui::Tip("How well this track fits what's playing now: similar tempo and a harmonically compatible key (Camelot wheel) mix best.");
        } else if (onDeck) {
            ImGui::TextDisabled("on deck %c", deckEntry[0] == i ? 'A' : 'B');
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

// ------------------------------------------------------------ clip editor ----

void App::drawClipEditor() {
    ClipEditorState& c = clip;
    const int n = library.size();
    if (c.entry < 0 || c.entry >= n) c.entry = deckEntry[0] >= 0 ? deckEntry[0] : 0;

    LibraryEntry* e = library.entry(c.entry);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Source");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(260);
    if (ImGui::BeginCombo("##clipsrc", e ? e->name.c_str() : "-")) {
        for (int i = 0; i < n; ++i) {
            LibraryEntry* x = library.entry(i);
            if (ImGui::Selectable(x->name.c_str(), i == c.entry)) {
                c.entry = i;
                c.viewStart = c.viewEnd = 0;
                c.selA = c.selB = -1;
            }
        }
        ImGui::EndCombo();
    }
    for (int d = 0; d < 2; ++d) {
        ImGui::SameLine();
        char lbl[32];
        std::snprintf(lbl, sizeof(lbl), "Use deck %c", 'A' + d);
        ImGui::BeginDisabled(deckEntry[d] < 0);
        if (ImGui::Button(lbl) && deckEntry[d] != c.entry) {
            c.entry = deckEntry[d];
            c.viewStart = c.viewEnd = 0;
            c.selA = c.selB = -1;
        }
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    const char* snaps[] = {"Snap: off", "Snap: beat", "Snap: bar"};
    ImGui::SetNextItemWidth(120);
    ImGui::Combo("##snap", &c.snap, snaps, 3);
    ui::Tip("Snapping keeps clip edges exactly on the beat so clips loop and mix cleanly. Keep it on.");

    TrackPtr tp = library.acquire(c.entry);
    if (!tp) {
        ImGui::Spacing();
        if (e && e->state == EntryState::Failed) ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "Could not load: %s", e->error.c_str());
        else {
            ImGui::Text("Preparing track...");
            ImGui::ProgressBar(e ? e->progress.load() : 0.0f, ImVec2(300, 0));
        }
        return;
    }
    const Track& t = *tp;
    const double total = double(t.frames());
    if (c.viewEnd <= c.viewStart) {
        c.viewStart = 0;
        c.viewEnd = total;
    }
    if (c.name[0] == 0 || c.selA < 0) {
        if (c.name[0] == 0) std::snprintf(c.name, sizeof(c.name), "%s clip %d", t.name.c_str(), c.counter);
    }

    auto snapFrame = [&](double f) {
        if (c.snap == 0) return std::clamp(f, 0.0, total);
        double unit = c.snap == 1 ? 1.0 : 4.0;
        double b = std::round(beatOf(t, f) / unit) * unit;
        double s = frameOfBeat(t, b);
        if (s < 0) s += unit * t.secPerBeat() * kSampleRate;
        return std::clamp(s, 0.0, total);
    };

    // Main canvas.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiIO& io = ImGui::GetIO();
    const float canvasH = std::clamp(ImGui::GetContentRegionAvail().y - 110.0f, 80.0f, 220.0f);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::InvisibleButton("##clipcanvas", ImVec2(w, canvasH),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    ImVec2 p1(p0.x + w, p0.y + canvasH);
    const bool hovered = ImGui::IsItemHovered();
    double viewLen = c.viewEnd - c.viewStart;
    double fpp = viewLen / w;
    auto xOf = [&](double f) { return p0.x + float((f - c.viewStart) / fpp); };
    auto fOf = [&](float x) { return c.viewStart + double(x - p0.x) * fpp; };

    if (hovered && io.MouseWheel != 0.0f) {
        double mf = fOf(io.MousePos.x);
        double newLen = std::clamp(viewLen * (io.MouseWheel > 0 ? 0.8 : 1.25), 2.0 * kSampleRate, total);
        double frac = (mf - c.viewStart) / viewLen;
        c.viewStart = std::clamp(mf - frac * newLen, 0.0, total - newLen);
        c.viewEnd = c.viewStart + newLen;
        viewLen = newLen;
        fpp = viewLen / w;
    }
    if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle))) {
        double shift = -io.MouseDelta.x * fpp;
        c.viewStart = std::clamp(c.viewStart + shift, 0.0, total - viewLen);
        c.viewEnd = c.viewStart + viewLen;
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        float mx = io.MousePos.x;
        bool hasSel = c.selA >= 0 && c.selB > c.selA;
        if (hasSel && std::fabs(mx - xOf(c.selA)) < 7) c.dragMode = 1;
        else if (hasSel && std::fabs(mx - xOf(c.selB)) < 7) c.dragMode = 2;
        else {
            c.dragMode = 0;
            c.selA = c.selB = snapFrame(fOf(mx));
        }
        c.dragging = true;
    }
    if (c.dragging) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            c.dragging = false;
            if (c.selB < c.selA) std::swap(c.selA, c.selB);
        } else {
            double f = snapFrame(fOf(std::clamp(io.MousePos.x, p0.x, p1.x)));
            if (c.dragMode == 1) c.selA = f;
            else c.selB = f;
        }
    }

    dl->AddRectFilled(p0, p1, IM_COL32(12, 12, 18, 255), 4.0f);
    dl->PushClipRect(p0, p1, true);
    {
        // Beat grid with bar numbers.
        double b0 = std::ceil(beatOf(t, c.viewStart)), b1 = beatOf(t, c.viewEnd);
        double step = 1.0;
        while ((b1 - b0) / step > 200) step *= 4;
        b0 = std::ceil(b0 / step) * step;
        for (double b = b0; b <= b1; b += step) {
            float x = xOf(frameOfBeat(t, b));
            bool bar = (long long)b % 4 == 0;
            dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), bar ? IM_COL32(255, 255, 255, 60) : IM_COL32(255, 255, 255, 20));
            if (bar && (step > 1 || (long long)b % 16 == 0 || (b1 - b0) < 64) && b >= 0) {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%d", int(b / 4) + 1);
                dl->AddText(ImVec2(x + 3, p0.y + 2), IM_COL32(255, 255, 255, 120), buf);
            }
        }
    }
    ui::DrawWaveform(dl, t, ImVec2(p0.x, p0.y + 14), p1, c.viewStart, fpp);
    if (c.selA >= 0 && c.selB != c.selA) {
        double a = std::min(c.selA, c.selB), b = std::max(c.selA, c.selB);
        float xa = xOf(a), xb = xOf(b);
        dl->AddRectFilled(ImVec2(p0.x, p0.y), ImVec2(xa, p1.y), IM_COL32(0, 0, 0, 120));
        dl->AddRectFilled(ImVec2(xb, p0.y), ImVec2(p1.x, p1.y), IM_COL32(0, 0, 0, 120));
        dl->AddRectFilled(ImVec2(xa, p0.y), ImVec2(xb, p1.y), IM_COL32(255, 200, 60, 30));
        for (float x : {xa, xb}) {
            dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), IM_COL32(255, 200, 60, 255), 2.0f);
            dl->AddRectFilled(ImVec2(x - 4, p0.y), ImVec2(x + 4, p0.y + 12), IM_COL32(255, 200, 60, 255), 2.0f);
        }
    }
    if (engine.previewing && engine.previewTrack == tp) {
        float x = xOf(engine.previewPos);
        dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), IM_COL32(255, 255, 255, 255), 2.0f);
    }
    dl->PopClipRect();
    if (hovered && !c.dragging)
        ui::Tip("Left-drag: select a region (drag the yellow edges to adjust)\nScroll: zoom    Right-drag: pan");

    // Overview strip showing where the zoomed view is.
    {
        ImVec2 o0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##clipov", ImVec2(w, 16));
        ImVec2 o1(o0.x + w, o0.y + 16);
        ui::DrawWaveform(dl, t, o0, o1, 0, total / w, 0.5f);
        float va = o0.x + float(c.viewStart / total) * w, vb = o0.x + float(c.viewEnd / total) * w;
        dl->AddRect(ImVec2(va, o0.y), ImVec2(std::max(va + 3, vb), o1.y), IM_COL32(255, 255, 255, 200), 2.0f);
        if (ImGui::IsItemActive()) {
            double centre = double(io.MousePos.x - o0.x) / w * total;
            c.viewStart = std::clamp(centre - viewLen * 0.5, 0.0, total - viewLen);
            c.viewEnd = c.viewStart + viewLen;
        }
        ui::Tip("Click or drag to move the zoomed view");
    }

    const bool hasSel = c.selA >= 0 && std::fabs(c.selB - c.selA) > 0.05 * kSampleRate;
    const double a = std::min(c.selA, c.selB), b = std::max(c.selA, c.selB);
    if (hasSel) {
        double beats = (b - a) / (t.secPerBeat() * kSampleRate);
        ImGui::Text("Selection: %s  to  %s   (%.2f beats = %.1f bars, %.2f s)", barBeat(t, a).c_str(), barBeat(t, b).c_str(),
                    beats, beats / 4.0, (b - a) / kSampleRate);
    } else {
        ImGui::TextDisabled("Drag across the waveform to select the part you want to cut out.");
    }

    // Actions.
    ImGui::BeginDisabled(!hasSel);
    if (ui::ColoredButton("Preview", theme.good, ImVec2(80, 0), engine.previewing)) engine.startPreview(tp, a, b);
    ImGui::EndDisabled();
    ui::Tip(liveDeck() >= 0 ? "Plays the selection through the main output - your audience will hear it too!"
                            : "Plays the selection once");
    ImGui::SameLine();
    if (ImGui::Button("Stop")) engine.stopPreview();
    ImGui::SameLine();
    ImGui::TextDisabled("  Quick select (bars):");
    for (int bars : {1, 2, 4, 8, 16}) {
        ImGui::SameLine();
        char lbl[8];
        std::snprintf(lbl, sizeof(lbl), "%d##qs", bars);
        if (ImGui::Button(lbl)) {
            double start = c.selA >= 0 ? a : c.viewStart;
            double sb = std::round(beatOf(t, start) / 4.0) * 4.0;
            if (frameOfBeat(t, sb) < 0) sb += 4;
            c.selA = frameOfBeat(t, sb);
            c.selB = std::min(total, frameOfBeat(t, sb + bars * 4));
        }
    }

    ImGui::SetNextItemWidth(260);
    ImGui::InputText("Name##clip", c.name, sizeof(c.name));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::SliderFloat("Fade (ms)", &c.fadeMs, 0.0f, 200.0f, "%.0f");
    ui::Tip("A tiny fade at each end prevents clicks. 5-10 ms is inaudible.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!hasSel);
    if (ui::ColoredButton("Save clip to Library", IM_COL32(255, 200, 60, 255), ImVec2(0, 0), hasSel)) {
        std::string nm = c.name[0] ? c.name : "Clip";
        saveClip(makeClip(t, a, b, nm, c.fadeMs));
        ++c.counter;
        std::snprintf(c.name, sizeof(c.name), "%s clip %d", t.name.c_str(), c.counter);
    }
    ui::Tip("Creates a new track from the selection (saved as a WAV in user_data/clips)");
    for (int d = 0; d < 2; ++d) {
        ImGui::SameLine();
        char lbl[32];
        std::snprintf(lbl, sizeof(lbl), "Loop on deck %c", 'A' + d);
        if (ImGui::Button(lbl)) {
            if (deckEntry[d] == c.entry && engine.decks[d].loaded()) {
                engine.setLoopRange(d, a, b);
            } else if (engine.deckAudible(d) || (engine.transitionBusy() && (engine.run.in == d || engine.run.out == d))) {
                toast(std::string("Deck ") + char('A' + d) + " is on air - use the other deck", theme.warn);
            } else {
                engine.loadTrack(d, tp);
                deckEntry[d] = c.entry;
                engine.setLoopRange(d, a, b);
            }
        }
        ui::Tip("Loads this track on the deck and loops the selection, ready to play");
    }
    ImGui::EndDisabled();
}

// ------------------------------------------------------ transition editor ----

void App::drawTransitionEditor() {
    TransitionEditorState& s = trEdit;
    s.selected = std::clamp(s.selected, 0, int(transitions.size()) - 1);

    // List.
    ImGui::BeginChild("##trlist", ImVec2(210, 0), ImGuiChildFlags_Borders);
    for (int i = 0; i < int(transitions.size()); ++i) {
        const TransitionDef& t = transitions[size_t(i)];
        char lbl[160];
        std::snprintf(lbl, sizeof(lbl), "%s%s##tr%d", t.name.c_str(), t.stock ? "" : "  *", i);
        if (ImGui::Selectable(lbl, s.selected == i)) {
            s.selected = i;
            s.dragKey = -1;
        }
        ui::Tip(t.stock ? "Stock transition (read-only)" : "Your custom transition");
    }
    ImGui::Separator();
    if (ImGui::Button("New", ImVec2(60, 0))) {
        TransitionDef d;
        d.name = "My Transition " + std::to_string(transitions.size() - makeStockTransitions().size() + 1);
        d.description = "A custom transition.";
        d.beats = 16;
        Lane& xf = d.lane(Param::Crossfader);
        xf.enabled = true;
        xf.keys = {{0, 0, CurveShape::Smooth}, {1, 1, CurveShape::Smooth}};
        transitions.push_back(d);
        s.selected = int(transitions.size()) - 1;
        s.lane = int(Param::Crossfader);
        s.dirty = true;
    }
    ui::Tip("Start a new transition from a simple crossfade");
    ImGui::SameLine();
    if (ImGui::Button("Duplicate", ImVec2(80, 0))) {
        TransitionDef d = transitions[size_t(s.selected)];
        d.stock = false;
        d.name += " (copy)";
        transitions.push_back(d);
        s.selected = int(transitions.size()) - 1;
        s.dirty = true;
    }
    ui::Tip("Make an editable copy of the selected transition");
    ImGui::BeginDisabled(transitions[size_t(s.selected)].stock);
    if (ImGui::Button("Delete", ImVec2(-1, 0))) {
        transitions.erase(transitions.begin() + s.selected);
        if (selectedTransition >= s.selected) selectedTransition = std::max(0, selectedTransition - 1);
        s.selected = std::max(0, s.selected - 1);
        s.dirty = true;
    }
    ImGui::EndDisabled();
    ImGui::EndChild();
    ImGui::SameLine();

    TransitionDef& def = transitions[size_t(s.selected)];
    const bool ro = def.stock;
    ImGui::BeginChild("##tredit", ImVec2(0, 0));
    if (ro) ImGui::TextColored(ImVec4(1, 0.82f, 0.3f, 1), "Stock transitions are read-only - press Duplicate to make your own editable copy.");

    ImGui::BeginDisabled(ro);
    char nameBuf[96];
    std::snprintf(nameBuf, sizeof(nameBuf), "%s", def.name.c_str());
    ImGui::SetNextItemWidth(220);
    if (ImGui::InputText("Name##tr", nameBuf, sizeof(nameBuf))) {
        def.name = nameBuf;
        s.dirty = true;
    }
    ImGui::SameLine();
    char descBuf[256];
    std::snprintf(descBuf, sizeof(descBuf), "%s", def.description.c_str());
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##desc", "Description", descBuf, sizeof(descBuf))) {
        def.description = descBuf;
        s.dirty = true;
    }
    ImGui::SetNextItemWidth(150);
    if (ImGui::SliderInt("Length (beats)", &def.beats, 1, 128)) s.dirty = true;
    for (int b : {4, 8, 16, 32, 64}) {
        ImGui::SameLine();
        char lbl[8];
        std::snprintf(lbl, sizeof(lbl), "%d##len", b);
        if (ImGui::SmallButton(lbl)) {
            def.beats = b;
            s.dirty = true;
        }
    }
    ImGui::SameLine(0, 20);
    float inBeat = def.inStartAt * def.beats;
    ImGui::SetNextItemWidth(130);
    if (ImGui::SliderFloat("In deck starts at beat", &inBeat, 0.0f, float(def.beats), "%.0f")) {
        def.inStartAt = std::round(inBeat) / float(def.beats);
        s.dirty = true;
    }
    const char* fx[] = {"No stop effect", "Power down", "Backspin"};
    int fxi = int(def.outEffect);
    ImGui::SetNextItemWidth(150);
    if (ImGui::Combo("Out deck effect", &fxi, fx, 3)) {
        def.outEffect = OutEffect(fxi);
        s.dirty = true;
    }
    if (def.outEffect != OutEffect::None) {
        ImGui::SameLine();
        float fxBeat = def.outEffectAt * def.beats;
        ImGui::SetNextItemWidth(130);
        if (ImGui::SliderFloat("at beat", &fxBeat, 0.0f, float(def.beats), "%.0f")) {
            def.outEffectAt = std::round(fxBeat) / float(def.beats);
            s.dirty = true;
        }
    }
    ImGui::EndDisabled();

    // Lane list.
    ImGui::BeginChild("##lanes", ImVec2(170, 0), ImGuiChildFlags_Borders);
    for (int p = 0; p < kNumParams; ++p) {
        Lane& l = def.lanes[size_t(p)];
        ImGui::PushID(p);
        ImGui::BeginDisabled(ro);
        bool en = l.enabled;
        if (ImGui::Checkbox("##en", &en)) {
            l.enabled = en;
            if (en && l.keys.empty()) {
                float nv = paramNeutral(Param(p));
                l.keys = {{0, nv, CurveShape::Smooth}, {1, nv, CurveShape::Smooth}};
            }
            s.lane = p;
            s.dirty = true;
        }
        ImGui::EndDisabled();
        ui::Tip("Enable to let this transition control it");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ui::withAlpha(laneColor(p), l.enabled ? 1.0f : 0.45f)));
        if (ImGui::Selectable(paramName(Param(p)), s.lane == p)) s.lane = p;
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // Curve editor.
    ImGui::BeginGroup();
    Lane& lane = def.lanes[size_t(s.lane)];
    const Param lp = Param(s.lane);
    ImGui::BeginDisabled(ro);
    auto preset = [&](const char* label, std::vector<Keyframe> keys, const char* tip) {
        ImGui::SameLine();
        if (ImGui::SmallButton(label)) {
            lane.enabled = true;
            lane.keys = std::move(keys);
            s.dirty = true;
        }
        ui::Tip(tip);
    };
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(laneColor(s.lane)), "%s", paramName(lp));
    float nv = paramNeutral(lp);
    preset("Ramp up", {{0, 0, CurveShape::Smooth}, {1, 1, CurveShape::Smooth}}, "Smoothly from 0 to full");
    preset("Ramp down", {{0, 1, CurveShape::Smooth}, {1, 0, CurveShape::Smooth}}, "Smoothly from full to 0");
    preset("Swap at middle", {{0, lp == Param::InLow || lp == Param::InMid || lp == Param::InHigh ? 0.0f : 0.5f, CurveShape::Step},
                              {0.5f, lp == Param::InLow || lp == Param::InMid || lp == Param::InHigh ? 0.5f : 0.0f, CurveShape::Linear},
                              {1, lp == Param::InLow || lp == Param::InMid || lp == Param::InHigh ? 0.5f : 0.0f, CurveShape::Linear}},
           "Instant jump halfway through (classic bass swap)");
    preset("Neutral", {{0, nv, CurveShape::Smooth}, {1, nv, CurveShape::Smooth}}, "Flat at the normal position");
    ImGui::SameLine();
    if (ImGui::SmallButton("Disable")) {
        lane.enabled = false;
        s.dirty = true;
    }
    ImGui::EndDisabled();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    const float cw = std::max(200.0f, avail.x), ch = std::max(90.0f, avail.y - 30.0f);
    ImVec2 c0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##curve", ImVec2(cw, ch), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const float padY = 10.0f;
    ImVec2 c1(c0.x + cw, c0.y + ch);
    auto toScreen = [&](float t, float v) { return ImVec2(c0.x + t * cw, c1.y - padY - v * (ch - 2 * padY)); };
    auto fromScreen = [&](ImVec2 p, float* t, float* v) {
        *t = std::clamp((p.x - c0.x) / cw, 0.0f, 1.0f);
        *v = std::clamp((c1.y - padY - p.y) / (ch - 2 * padY), 0.0f, 1.0f);
    };

    dl->AddRectFilled(c0, c1, IM_COL32(12, 12, 18, 255), 4.0f);
    for (int b = 0; b <= def.beats; ++b) {
        if (def.beats > 64 && b % 4) continue;
        float x = c0.x + cw * b / float(def.beats);
        bool bar = b % 4 == 0;
        dl->AddLine(ImVec2(x, c0.y), ImVec2(x, c1.y), bar ? IM_COL32(255, 255, 255, 45) : IM_COL32(255, 255, 255, 15));
        if (bar && b < def.beats) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "bar %d", b / 4 + 1);
            dl->AddText(ImVec2(x + 3, c1.y - 16), IM_COL32(255, 255, 255, 70), buf);
        }
    }
    const char *lo, *mid, *hi;
    valueLabels(lp, &lo, &mid, &hi);
    for (float v : {0.0f, 0.5f, 1.0f}) {
        ImVec2 a = toScreen(0, v), b = toScreen(1, v);
        dl->AddLine(a, b, IM_COL32(255, 255, 255, v == 0.5f ? 30 : 18));
        dl->AddText(ImVec2(a.x + 4, a.y - (v > 0.9f ? 0 : 15)), IM_COL32(255, 255, 255, 90), v == 0 ? lo : v == 0.5f ? mid : hi);
    }
    // Event markers.
    {
        float x = c0.x + def.inStartAt * cw;
        dl->AddLine(ImVec2(x, c0.y), ImVec2(x, c1.y), IM_COL32(80, 255, 140, 160), 2.0f);
        dl->AddText(ImVec2(x + 4, c0.y + 2), IM_COL32(80, 255, 140, 220), "In starts");
        if (def.outEffect != OutEffect::None) {
            float fx2 = c0.x + def.outEffectAt * cw;
            dl->AddLine(ImVec2(fx2, c0.y), ImVec2(fx2, c1.y), IM_COL32(255, 90, 90, 160), 2.0f);
            dl->AddText(ImVec2(fx2 + 4, c0.y + 16), IM_COL32(255, 90, 90, 220),
                        def.outEffect == OutEffect::Brake ? "Power down" : "Backspin");
        }
    }
    auto drawLane = [&](const Lane& l, ImU32 col, float thick) {
        const int steps = 160;
        std::vector<ImVec2> pts(steps + 1);
        for (int i = 0; i <= steps; ++i) {
            float tt = float(i) / steps;
            pts[size_t(i)] = toScreen(tt, l.eval(tt));
        }
        dl->AddPolyline(pts.data(), steps + 1, col, 0, thick);
    };
    for (int p = 0; p < kNumParams; ++p)
        if (p != s.lane && def.lanes[size_t(p)].enabled && !def.lanes[size_t(p)].keys.empty())
            drawLane(def.lanes[size_t(p)], ui::withAlpha(laneColor(p), 0.25f), 1.5f);
    if (lane.enabled && !lane.keys.empty()) {
        drawLane(lane, laneColor(s.lane), 3.0f);
        int hoverKey = -1;
        for (int k = 0; k < int(lane.keys.size()); ++k) {
            ImVec2 kp = toScreen(lane.keys[size_t(k)].t, lane.keys[size_t(k)].v);
            float d2 = (kp.x - io.MousePos.x) * (kp.x - io.MousePos.x) + (kp.y - io.MousePos.y) * (kp.y - io.MousePos.y);
            if (hovered && d2 < 64) hoverKey = k;
            bool hot = k == hoverKey || k == s.dragKey;
            dl->AddCircleFilled(kp, hot ? 7.0f : 5.0f, hot ? IM_COL32(255, 255, 255, 255) : laneColor(s.lane));
            dl->AddCircle(kp, hot ? 7.0f : 5.0f, IM_COL32(0, 0, 0, 255));
        }
        if (!ro) {
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && hoverKey >= 0) s.dragKey = hoverKey;
            if (s.dragKey >= 0 && s.dragKey < int(lane.keys.size())) {
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    float t, v;
                    fromScreen(io.MousePos, &t, &v);
                    // Snap time to the beat grid unless Shift is held.
                    if (!io.KeyShift) t = std::round(t * def.beats * 2) / (def.beats * 2.0f);
                    float tmin = s.dragKey > 0 ? lane.keys[size_t(s.dragKey) - 1].t : 0.0f;
                    float tmax = s.dragKey + 1 < int(lane.keys.size()) ? lane.keys[size_t(s.dragKey) + 1].t : 1.0f;
                    lane.keys[size_t(s.dragKey)].t = std::clamp(t, tmin, tmax);
                    lane.keys[size_t(s.dragKey)].v = v;
                    s.dirty = true;
                } else {
                    s.dragKey = -1;
                }
            }
            if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hoverKey < 0) {
                float t, v;
                fromScreen(io.MousePos, &t, &v);
                if (!io.KeyShift) t = std::round(t * def.beats * 2) / (def.beats * 2.0f);
                lane.keys.push_back({t, v, CurveShape::Smooth});
                lane.sortKeys();
                s.dirty = true;
            }
            if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && hoverKey >= 0) {
                s.dragKey = hoverKey;
                ImGui::OpenPopup("##keymenu");
            }
        }
    } else if (!ro) {
        const char* msg = "This control isn't automated. Tick its box (or double-click here) to add a curve.";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(c0.x + (cw - ts.x) * 0.5f, c0.y + ch * 0.5f - ts.y), IM_COL32(200, 200, 220, 160), msg);
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            lane.enabled = true;
            lane.keys = {{0, nv, CurveShape::Smooth}, {1, nv, CurveShape::Smooth}};
            s.dirty = true;
        }
    }
    if (ImGui::BeginPopup("##keymenu")) {
        if (s.dragKey >= 0 && s.dragKey < int(lane.keys.size())) {
            Keyframe& k = lane.keys[size_t(s.dragKey)];
            ImGui::TextDisabled("Curve after this point");
            if (ImGui::RadioButton("Smooth", k.shape == CurveShape::Smooth)) { k.shape = CurveShape::Smooth; s.dirty = true; }
            if (ImGui::RadioButton("Linear", k.shape == CurveShape::Linear)) { k.shape = CurveShape::Linear; s.dirty = true; }
            if (ImGui::RadioButton("Step (jump)", k.shape == CurveShape::Step)) { k.shape = CurveShape::Step; s.dirty = true; }
            ImGui::Separator();
            ImGui::BeginDisabled(lane.keys.size() <= 1);
            if (ImGui::MenuItem("Delete point")) {
                lane.keys.erase(lane.keys.begin() + s.dragKey);
                s.dirty = true;
            }
            ImGui::EndDisabled();
        }
        ImGui::EndPopup();
    } else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        s.dragKey = -1;
    }
    ImGui::TextDisabled(ro ? "Read-only preview." : "Drag points to move (Shift = off-grid)  |  Double-click: add point  |  Right-click point: curve shape / delete");
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 150);
    if (ImGui::Button("Use in transition bar", ImVec2(150, 0))) selectedTransition = s.selected;
    ImGui::EndGroup();
    ImGui::EndChild();

    // Auto-save custom transitions once an edit gesture is finished.
    if (s.dirty && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsAnyItemActive()) {
        saveCustomTransitions();
        s.dirty = false;
    }
}

// ----------------------------------------------------------------- auto dj ----

void App::drawAutoDj() {
    ImGui::PushFont(fontBig, 20.0f);
    if (ui::ColoredButton(autoDj ? "AUTO DJ IS ON" : "AUTO DJ IS OFF", theme.good, ImVec2(220, 44), autoDj)) autoDj = !autoDj;
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextWrapped("Auto DJ plays through your library for you: it loads the next track on the free deck, "
                       "beat-matches it and runs a transition near the end of every song. Take over any time "
                       "by switching it off.");
    ImGui::Separator();
    ImGui::Text("Track order");
    int ord = int(autoDjOrder);
    ImGui::RadioButton("Library order", &ord, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Shuffle", &ord, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Best match (tempo & key)", &ord, 2);
    autoDjOrder = AutoDjOrder(ord);
    ImGui::Text("Transitions");
    int tr = autoDjRandomTransition ? 1 : 0;
    char lbl[128];
    std::snprintf(lbl, sizeof(lbl), "Always \"%s\"", transitions[size_t(selectedTransition)].name.c_str());
    ImGui::RadioButton(lbl, &tr, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Surprise me (random)", &tr, 1);
    autoDjRandomTransition = tr == 1;
    ImGui::Separator();

    int live = liveDeck();
    if (live >= 0 && engine.decks[live].track) {
        const Deck& l = engine.decks[live];
        ImGui::Text("Now playing on %c: ", 'A' + live);
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme.deck[live]), "%s", l.track->name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%s left)", formatTime(l.remainingSec()).c_str());
        int other = 1 - live;
        if (engine.decks[other].track) {
            ImGui::Text("Up next on %c:    ", 'A' + other);
            ImGui::SameLine();
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme.deck[other]), "%s", engine.decks[other].track->name.c_str());
            if (autoDj && !engine.transitionBusy()) {
                const TransitionDef& def = transitions[size_t(selectedTransition)];
                double spb = l.secPerBeat() / std::max(0.25, l.rate());
                double in = l.remainingSec() - (def.beats + 6) * spb;
                ImGui::SameLine();
                ImGui::TextDisabled("  - mixing in about %s", formatTime(std::max(0.0, in)).c_str());
            }
        }
    } else {
        ImGui::TextDisabled("Nothing is playing. Switch Auto DJ on and it will start the music.");
    }
    ImGui::BeginDisabled(engine.transitionBusy());
    if (ImGui::Button("Mix to the next track now")) triggerTransition();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("Tracks mixed by Auto DJ this session: %d", autoDjPlayed);
}

// ------------------------------------------------------------- visualizer ----

void App::drawVisualizerPanel(ImVec2 pos, ImVec2 size) {
    beginPanel("##vis", pos, size);
    for (int m = 0; m < Visualizer::NumModes; ++m) {
        if (m) ImGui::SameLine(0, 3);
        if (ui::ColoredButton(Visualizer::modeName(m), IM_COL32(150, 110, 255, 255), ImVec2(0, 0), vis.mode == m)) vis.mode = m;
    }
    ImGui::SameLine();
    ImGui::Checkbox("Cycle", &vis.autoCycle);
    ui::Tip("Switch visual every 15 seconds");
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 p1(p0.x + avail.x, p0.y + avail.y);
    ImGui::InvisibleButton("##viscanvas", avail);
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) partyMode = true;
    ui::Tip("Double-click for fullscreen party mode");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    vis.draw(dl, p0, p1, vis.mode);
    // Band meters.
    const char* names[3] = {"LOW", "MID", "HIGH"};
    float vals[3] = {vis.bass, vis.mids, vis.treble};
    for (int i = 0; i < 3; ++i) {
        ImVec2 b0(p0.x + 8 + i * 40, p1.y - 60), b1(b0.x + 10, p1.y - 22);
        dl->AddRectFilled(b0, b1, IM_COL32(0, 0, 0, 120), 2);
        float h = (b1.y - b0.y) * std::clamp(vals[i], 0.0f, 1.0f);
        dl->AddRectFilled(ImVec2(b0.x, b1.y - h), b1, ui::hsv(0.6f - i * 0.25f, 0.7f, 1.0f), 2);
        dl->AddText(ImVec2(b0.x - 4, p1.y - 18), IM_COL32(255, 255, 255, 140), names[i]);
    }
    ImGui::End();
}

// ------------------------------------------------------------------- help ----

void App::drawHelpWindow() {
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(700.0f, ds.x - 40), std::min(600.0f, ds.y - 40)), ImGuiCond_Appearing);
    bool open = true;
    if (ImGui::Begin("Quick Start - Dummy DJ", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PushFont(fontBig, 24.0f);
        ImGui::TextColored(ImVec4(0.3f, 0.85f, 1, 1), "Welcome to Dummy DJ!");
        ImGui::PopFont();
        ImGui::TextWrapped("You can't break anything here. Every knob resets with a double-click, the limiter "
                           "stops the sound from distorting, and SYNC keeps the beats together.");
        ImGui::SeparatorText("Your first mix in 3 steps");
        ImGui::BulletText("Press PLAY on deck A (or press Q). Two tracks are already loaded for you.");
        ImGui::BulletText("Pick a transition in the bar under the decks - try \"Bass Swap\".");
        ImGui::BulletText("Press MIX (or T). It waits for the next bar, matches tempos and mixes into deck B.");
        ImGui::TextWrapped("Then load another track onto the free deck (double-click it in the Library) and repeat. "
                           "Or turn on Auto DJ and just enjoy the visuals.");
        ImGui::SeparatorText("Going further");
        ImGui::BulletText("EQ knobs (HIGH / MID / LOW): shape each track. Killing the LOW on one deck avoids muddy bass.");
        ImGui::BulletText("FILTER sweeps and ECHO throws are instant crowd-pleasers.");
        ImGui::BulletText("LOOP buttons repeat a few beats on the beat. HOT CUES remember spots to jump back to.");
        ImGui::BulletText("Clip Editor: cut your favourite part of any track into a new clip (saved as WAV).");
        ImGui::BulletText("Transition Editor: duplicate a stock transition and draw your own automation curves.");
        ImGui::BulletText("Drag your own WAV / MP3 / FLAC files onto the window. Files in ./music load at startup.");
        ImGui::SeparatorText("Keyboard");
        ImGui::TextUnformatted("Q / P  play-pause deck A / B      T  run transition      F  party mode\n"
                               "V  next visual      H  this help      Esc  leave party mode");
        ImGui::Spacing();
        if (ui::ColoredButton("Let's go!", theme.good, ImVec2(140, 36), true)) open = false;
    }
    ImGui::End();
    if (!open) {
        showHelp = false;
        std::error_code ec;
        std::filesystem::create_directories(userDir, ec);
        std::ofstream(userDir + "/seen_help") << "1\n";
    }
}
