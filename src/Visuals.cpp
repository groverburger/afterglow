#include "Visuals.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>

#include "Engine.h"
#include "Widgets.h"
#include "sokol_app.h"
#include "sokol_imgui.h"

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kMinHz = 30.0f, kMaxHz = 16000.0f;

void fft(std::vector<std::complex<float>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * kPi / float(len);
        std::complex<float> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (size_t k = 0; k < len / 2; ++k) {
                auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// Inferno-like colour map, packed as RGBA8.
uint32_t heat(float t) {
    static const float stops[6][3] = {{0, 0, 4},       {40, 11, 84},    {120, 28, 109},
                                      {200, 60, 70},   {250, 150, 20},  {252, 255, 164}};
    t = std::clamp(t, 0.0f, 1.0f) * 5.0f;
    int i = std::min(4, int(t));
    float f = t - float(i);
    auto ch = [&](int c) { return uint32_t(stops[i][c] + (stops[i + 1][c] - stops[i][c]) * f); };
    return ch(0) | (ch(1) << 8) | (ch(2) << 16) | 0xFF000000u;
}

float binHz(int bin) { return float(bin) * float(kSampleRate) / Visualizer::kFftSize; }

std::vector<float> g_mags(Visualizer::kFftSize / 2, 0.0f);

float magAtHz(float hz) {
    float b = hz * Visualizer::kFftSize / float(kSampleRate);
    int i = std::clamp(int(b), 0, int(g_mags.size()) - 2);
    float f = b - float(i);
    return g_mags[size_t(i)] * (1 - f) + g_mags[size_t(i) + 1] * f;
}

float dbNorm(float mag) {
    float db = 20.0f * std::log10(std::max(mag, 1e-7f));
    return std::clamp((db + 72.0f) / 66.0f, 0.0f, 1.0f);
}

}  // namespace

const char* Visualizer::modeName(int m) {
    static const char* names[NumModes] = {"Spectrum", "Radial Burst", "Spectrogram", "Vectorscope", "Tunnel", "Oscilloscope"};
    return m >= 0 && m < NumModes ? names[m] : "?";
}

void Visualizer::init() {
    sg_image_desc id{};
    id.width = kSpecW;
    id.height = kSpecH;
    id.pixel_format = SG_PIXELFORMAT_RGBA8;
    id.usage.write_transient = true;
    id.label = "spectrogram";
    specImg_ = sg_make_image(&id);
    sg_view_desc vd{};
    vd.texture.image = specImg_;
    specView_ = sg_make_view(&vd);
    sg_sampler_desc sd{};
    sd.min_filter = SG_FILTER_LINEAR;
    sd.mag_filter = SG_FILTER_LINEAR;
    sd.wrap_u = SG_WRAP_REPEAT;
    sd.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    specSmp_ = sg_make_sampler(&sd);
    particles_.reserve(2048);
}

void Visualizer::shutdown() {
    sg_destroy_sampler(specSmp_);
    sg_destroy_view(specView_);
    sg_destroy_image(specImg_);
}

void Visualizer::update(Engine& engine, float dt) {
    engine.copyScope(scope_.data(), kFftSize);
    const Deck& md = engine.decks[engine.masterDeck];
    masterPlaying = md.loaded() && md.playing;
    masterBeat = md.beatPos();
    time_ += dt;
    analyse(dt);

    beatPulse *= std::exp(-dt * 7.0f);
    if (masterPlaying) {
        if (std::floor(masterBeat) != std::floor(lastBeat_)) beatPulse = 1.0f;
        lastBeat_ = masterBeat;
    }
    hueShift_ += dt * (0.02f + 0.08f * bass);
    if (autoCycle) {
        cycleTimer_ += dt;
        if (cycleTimer_ > 15.0f) {
            cycleTimer_ = 0;
            mode = (mode + 1) % NumModes;
        }
    }
}

void Visualizer::analyse(float dt) {
    std::vector<std::complex<float>> buf(kFftSize);
    float sumSq = 0;
    for (int i = 0; i < kFftSize; ++i) {
        float m = 0.5f * (scope_[size_t(i) * 2] + scope_[size_t(i) * 2 + 1]);
        sumSq += m * m;
        float w = 0.5f - 0.5f * std::cos(2.0f * kPi * i / (kFftSize - 1));
        buf[size_t(i)] = {m * w, 0.0f};
    }
    fft(buf);
    for (size_t i = 0; i < g_mags.size(); ++i) g_mags[i] = std::abs(buf[i]) / (kFftSize * 0.25f);
    level = std::sqrt(sumSq / kFftSize);

    const float fall = std::exp(-dt * 5.0f);
    float b = 0, m = 0, t = 0;
    int nb = 0, nm = 0, nt = 0;
    for (int i = 0; i < kBars; ++i) {
        float f0 = kMinHz * std::pow(kMaxHz / kMinHz, float(i) / kBars);
        float f1 = kMinHz * std::pow(kMaxHz / kMinHz, float(i + 1) / kBars);
        int b0 = int(f0 * kFftSize / kSampleRate), b1 = std::max(b0 + 1, int(f1 * kFftSize / kSampleRate));
        float mx = 0;
        for (int k = b0; k < b1 && k < int(g_mags.size()); ++k) mx = std::max(mx, g_mags[size_t(k)]);
        // Gentle tilt so highs are as visible as lows.
        float v = dbNorm(mx * (1.0f + float(i) / kBars * 3.0f));
        bars_[i] = v > bars_[i] ? bars_[i] + (v - bars_[i]) * 0.6f : bars_[i] * fall + v * (1 - fall);
        if (bars_[i] >= peaks_[i]) {
            peaks_[i] = bars_[i];
            peakVel_[i] = 0;
        } else {
            peakVel_[i] += dt * 1.5f;
            peaks_[i] = std::max(bars_[i], peaks_[i] - peakVel_[i] * dt);
        }
        float fc = std::sqrt(f0 * f1);
        if (fc < 150) { b += bars_[i]; ++nb; }
        else if (fc < 2500) { m += bars_[i]; ++nm; }
        else { t += bars_[i]; ++nt; }
    }
    bass = nb ? b / nb : 0;
    mids = nm ? m / nm : 0;
    treble = nt ? t / nt : 0;
    bassAvg_ += (bass - bassAvg_) * std::min(1.0f, dt * 2.0f);
    kick = std::max(0.0f, bass - bassAvg_) * 4.0f;

    // Spectrogram column (low frequencies at the bottom).
    for (int y = 0; y < kSpecH; ++y) {
        float fy = float(kSpecH - 1 - y) / float(kSpecH - 1);
        float hz = kMinHz * std::pow(kMaxHz / kMinHz, fy);
        float v = dbNorm(magAtHz(hz) * (1.0f + fy * 3.0f));
        specPixels_[size_t(y) * kSpecW + size_t(specCol_)] = heat(v * v * 1.1f);
    }
    specCol_ = (specCol_ + 1) % kSpecW;
    (void)binHz;
}

void Visualizer::uploadTextures() {
    sg_write_image_desc wd{};
    wd.src.data.ptr = specPixels_.data();
    wd.src.data.size = specPixels_.size() * sizeof(uint32_t);
    wd.dst.image = specImg_;
    sg_write_image_transient(&wd);
}

void Visualizer::spawnBurst(float cx, float cy, int count, float speed) {
    for (int i = 0; i < count && particles_.size() < 1500; ++i) {
        float a = float(std::rand()) / float(RAND_MAX) * 2.0f * kPi;
        float s = speed * (0.4f + float(std::rand()) / float(RAND_MAX));
        float life = 0.6f + float(std::rand()) / float(RAND_MAX) * 1.2f;
        particles_.push_back({cx, cy, std::cos(a) * s, std::sin(a) * s, life, life,
                              hueShift_ + float(std::rand()) / float(RAND_MAX) * 0.25f, 1.5f + float(std::rand() % 3)});
    }
}

void Visualizer::drawParticles(ImDrawList* dl, ImVec2, ImVec2, float dt) {
    for (auto& p : particles_) {
        p.x += p.vx * dt;
        p.y += p.vy * dt;
        p.vx *= 1.0f - dt * 0.6f;
        p.vy *= 1.0f - dt * 0.6f;
        p.life -= dt;
        float a = std::clamp(p.life / p.maxLife, 0.0f, 1.0f);
        dl->AddCircleFilled(ImVec2(p.x, p.y), p.size * (0.5f + a), ui::hsv(p.hue, 0.7f, 1.0f, a), 8);
    }
    particles_.erase(std::remove_if(particles_.begin(), particles_.end(), [](const Particle& p) { return p.life <= 0; }),
                     particles_.end());
}

void Visualizer::draw(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int m) {
    dl->PushClipRect(p0, p1, true);
    // Background: dark gradient that breathes with the bass.
    ImU32 top = ui::hsv(hueShift_ + 0.6f, 0.7f, 0.06f + 0.10f * bass);
    ImU32 bot = ui::hsv(hueShift_ + 0.8f, 0.8f, 0.03f + 0.18f * beatPulse * 0.5f + 0.06f * bass);
    dl->AddRectFilledMultiColor(p0, p1, top, top, bot, bot);
    switch (m) {
        case Spectrum: drawSpectrum(dl, p0, p1); break;
        case Radial: drawRadial(dl, p0, p1); break;
        case Spectrogram: drawSpectrogram(dl, p0, p1); break;
        case Vectorscope: drawVectorscope(dl, p0, p1); break;
        case Tunnel: drawTunnel(dl, p0, p1); break;
        case Scope: drawScope(dl, p0, p1); break;
        default: break;
    }
    // Beat flash around the edge.
    if (beatPulse > 0.05f)
        dl->AddRect(p0, p1, ui::hsv(hueShift_, 0.6f, 1.0f, beatPulse * 0.5f), 0.0f, 0, 2.0f + 4.0f * beatPulse);
    dl->PopClipRect();
}

void Visualizer::drawSpectrum(ImDrawList* dl, ImVec2 p0, ImVec2 p1) {
    const float w = p1.x - p0.x, h = p1.y - p0.y;
    const float floorY = p0.y + h * 0.78f;
    const float bw = w / kBars;
    for (int i = 0; i < kBars; ++i) {
        float x0 = p0.x + i * bw + 1.0f, x1 = x0 + bw - 2.0f;
        float bh = bars_[i] * h * 0.74f;
        ImU32 cTop = ui::hsv(hueShift_ + 0.55f - float(i) / kBars * 0.55f, 0.8f, 1.0f);
        ImU32 cBot = ui::hsv(hueShift_ + 0.62f - float(i) / kBars * 0.55f, 0.9f, 0.45f);
        dl->AddRectFilledMultiColor(ImVec2(x0, floorY - bh), ImVec2(x1, floorY), cTop, cTop, cBot, cBot);
        // Reflection.
        dl->AddRectFilledMultiColor(ImVec2(x0, floorY + 2), ImVec2(x1, floorY + 2 + bh * 0.3f), ui::withAlpha(cBot, 0.35f),
                                    ui::withAlpha(cBot, 0.35f), ui::withAlpha(cBot, 0.0f), ui::withAlpha(cBot, 0.0f));
        float py = floorY - peaks_[i] * h * 0.74f;
        dl->AddRectFilled(ImVec2(x0, py - 3), ImVec2(x1, py - 1), IM_COL32(255, 255, 255, 220));
    }
}

void Visualizer::drawRadial(ImDrawList* dl, ImVec2 p0, ImVec2 p1) {
    const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
    const float base = std::min(p1.x - p0.x, p1.y - p0.y) * 0.2f;
    const float R = base * (1.0f + bass * 0.25f + beatPulse * 0.06f);
    // Glow halo.
    for (int i = 0; i < 6; ++i)
        dl->AddCircleFilled(c, R * (2.1f - i * 0.18f), ui::hsv(hueShift_ + 0.1f * i, 0.8f, 1.0f, 0.02f + 0.04f * bass), 64);
    const int n = kBars * 2;
    const float rot = time_ * 0.15f;
    const float thick = 2.0f * kPi * R / n * 0.7f;
    for (int i = 0; i < n; ++i) {
        float v = bars_[i < kBars ? i : n - 1 - i];
        float a = rot - kPi * 0.5f + float(i) / n * 2.0f * kPi;
        float len = 4.0f + v * base * 1.5f;
        ImVec2 d(std::cos(a), std::sin(a));
        ImU32 col = ui::hsv(hueShift_ + float(i) / n * 0.6f, 0.85f, 1.0f);
        dl->AddLine(ImVec2(c.x + d.x * R, c.y + d.y * R), ImVec2(c.x + d.x * (R + len), c.y + d.y * (R + len)), col, thick);
        // Inner mirrored spikes.
        float il = v * R * 0.35f;
        dl->AddLine(ImVec2(c.x + d.x * (R - 3), c.y + d.y * (R - 3)), ImVec2(c.x + d.x * (R - 3 - il), c.y + d.y * (R - 3 - il)),
                    ui::withAlpha(col, 0.45f), thick * 0.6f);
    }
    // Core orb and a rotating polygon.
    dl->AddCircleFilled(c, R * 0.55f, ui::hsv(hueShift_ + 0.5f, 0.6f, 0.15f + 0.5f * beatPulse), 48);
    dl->AddCircle(c, R * 0.55f, ui::hsv(hueShift_ + 0.5f, 0.5f, 1.0f, 0.8f), 48, 2.0f);
    const int sides = 6;
    ImVec2 pts[sides];
    for (int k = 0; k < sides; ++k) {
        float a = -time_ * 0.6f + k * 2.0f * kPi / sides;
        float r = R * (0.3f + 0.15f * mids);
        pts[k] = ImVec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r);
    }
    dl->AddPolyline(pts, sides, ui::hsv(hueShift_ + 0.2f, 0.4f, 1.0f, 0.9f), ImDrawFlags_Closed, 2.0f);

    if (kick > 0.35f && beatPulse > 0.6f) spawnBurst(c.x, c.y, 6, base * 2.5f);
    drawParticles(dl, p0, p1, ImGui::GetIO().DeltaTime);
}

void Visualizer::drawSpectrogram(ImDrawList* dl, ImVec2 p0, ImVec2 p1) {
    float u0 = float(specCol_) / kSpecW;
    ImTextureID tex = ImTextureID(simgui_imtextureid_with_sampler(specView_, specSmp_));
    dl->AddImage(tex, p0, p1, ImVec2(u0, 0), ImVec2(u0 + 1.0f, 1));
    // Frequency labels.
    const float labels[] = {50, 100, 250, 500, 1000, 2500, 5000, 10000};
    for (float hz : labels) {
        float fy = std::log(hz / kMinHz) / std::log(kMaxHz / kMinHz);
        float y = p1.y - fy * (p1.y - p0.y);
        dl->AddLine(ImVec2(p0.x, y), ImVec2(p0.x + 8, y), IM_COL32(255, 255, 255, 120));
        char buf[16];
        if (hz >= 1000) std::snprintf(buf, sizeof(buf), "%gk", hz / 1000);
        else std::snprintf(buf, sizeof(buf), "%g", hz);
        dl->AddText(ImVec2(p0.x + 10, y - 7), IM_COL32(255, 255, 255, 150), buf);
    }
}

void Visualizer::drawVectorscope(ImDrawList* dl, ImVec2 p0, ImVec2 p1) {
    const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
    const float s = std::min(p1.x - p0.x, p1.y - p0.y) * 0.42f;
    dl->AddLine(ImVec2(c.x - s, c.y), ImVec2(c.x + s, c.y), IM_COL32(255, 255, 255, 30));
    dl->AddLine(ImVec2(c.x, c.y - s), ImVec2(c.x, c.y + s), IM_COL32(255, 255, 255, 30));
    dl->AddCircle(c, s, IM_COL32(255, 255, 255, 25), 64);
    std::vector<ImVec2> pts;
    const int n = 1024;
    pts.reserve(n);
    for (int i = kFftSize - n; i < kFftSize; ++i) {
        float l = scope_[size_t(i) * 2], r = scope_[size_t(i) * 2 + 1];
        float x = (l - r) * 0.7071f, y = (l + r) * 0.7071f;
        pts.emplace_back(c.x + x * s * 1.2f, c.y - y * s * 1.2f);
    }
    trails_.push_back(std::move(pts));
    if (trails_.size() > 6) trails_.erase(trails_.begin());
    for (size_t k = 0; k < trails_.size(); ++k) {
        float a = float(k + 1) / float(trails_.size());
        ImU32 col = ui::hsv(hueShift_ + 0.45f + a * 0.1f, 0.6f, 1.0f, a * a * 0.8f);
        dl->AddPolyline(trails_[k].data(), int(trails_[k].size()), col, 0, 1.0f + a);
    }
    dl->AddText(ImVec2(c.x - s, c.y - s), IM_COL32(255, 255, 255, 110), "L");
    dl->AddText(ImVec2(c.x + s - 8, c.y - s), IM_COL32(255, 255, 255, 110), "R");
}

void Visualizer::drawTunnel(ImDrawList* dl, ImVec2 p0, ImVec2 p1) {
    const ImVec2 c((p0.x + p1.x) * 0.5f + std::sin(time_ * 0.7f) * 20.0f, (p0.y + p1.y) * 0.5f + std::cos(time_ * 0.5f) * 14.0f);
    const float maxR = std::hypot(p1.x - p0.x, p1.y - p0.y) * 0.6f;
    const float dt = ImGui::GetIO().DeltaTime;
    tunnelZ_ += dt * (0.25f + bass * 1.2f + beatPulse * 0.3f);
    const int rings = 18, sides = 8;
    for (int k = 0; k < rings; ++k) {
        float s = std::fmod(float(k) / rings + tunnelZ_, 1.0f);  // 0 far .. 1 near
        float r = maxR * s * s * s;
        float rot = time_ * 0.25f + s * 1.5f;
        ImVec2 pts[sides];
        for (int j = 0; j < sides; ++j) {
            float v = bars_[(j * 7 + k * 3) % kBars];
            float a = rot + j * 2.0f * kPi / sides;
            float rr = r * (1.0f + v * 0.35f);
            pts[j] = ImVec2(c.x + std::cos(a) * rr, c.y + std::sin(a) * rr);
        }
        ImU32 col = ui::hsv(hueShift_ + s * 0.4f + k * 0.03f, 0.85f, 1.0f, s * 0.9f);
        dl->AddPolyline(pts, sides, col, ImDrawFlags_Closed, 1.0f + 4.0f * s);
    }
    // Star streaks flying out of the centre.
    if (particles_.size() < 600) {
        int spawn = 2 + int(bass * 10 + kick * 20);
        for (int i = 0; i < spawn; ++i) {
            float a = float(std::rand()) / float(RAND_MAX) * 2.0f * kPi;
            float sp = 60.0f + float(std::rand() % 200);
            particles_.push_back({c.x, c.y, std::cos(a) * sp, std::sin(a) * sp, 2.0f, 2.0f, hueShift_ + 0.5f, 1.0f});
        }
    }
    for (auto& p : particles_) {
        float ox = p.x, oy = p.y;
        p.vx *= 1.0f + dt * 2.2f;
        p.vy *= 1.0f + dt * 2.2f;
        p.x += p.vx * dt;
        p.y += p.vy * dt;
        p.life -= dt;
        dl->AddLine(ImVec2(ox, oy), ImVec2(p.x, p.y), ui::hsv(p.hue, 0.3f, 1.0f, std::min(1.0f, (2.0f - p.life))), 1.5f);
    }
    particles_.erase(std::remove_if(particles_.begin(), particles_.end(),
                                    [&](const Particle& p) {
                                        return p.life <= 0 || p.x < p0.x - 50 || p.x > p1.x + 50 || p.y < p0.y - 50 ||
                                               p.y > p1.y + 50;
                                    }),
                     particles_.end());
}

void Visualizer::drawScope(ImDrawList* dl, ImVec2 p0, ImVec2 p1) {
    const int n = 1024;
    // Trigger on a rising zero crossing so the trace stands still.
    int start = kFftSize - n;
    for (int i = kFftSize - n; i > 1; --i) {
        float a = scope_[size_t(i - 1) * 2] + scope_[size_t(i - 1) * 2 + 1];
        float b = scope_[size_t(i) * 2] + scope_[size_t(i) * 2 + 1];
        if (a < 0 && b >= 0) {
            start = i;
            break;
        }
    }
    const float midY = (p0.y + p1.y) * 0.5f, h = (p1.y - p0.y) * 0.45f;
    std::vector<ImVec2> pts(n);
    for (int i = 0; i < n; ++i) {
        size_t k = size_t(std::min(kFftSize - 1, start + i));
        float v = 0.5f * (scope_[k * 2] + scope_[k * 2 + 1]);
        pts[size_t(i)] = ImVec2(p0.x + (p1.x - p0.x) * i / (n - 1), midY - v * h * 1.6f);
    }
    ImU32 col = ui::hsv(hueShift_ + 0.35f, 0.7f, 1.0f);
    dl->AddPolyline(pts.data(), n, ui::withAlpha(col, 0.12f), 0, 9.0f);
    dl->AddPolyline(pts.data(), n, ui::withAlpha(col, 0.3f), 0, 4.0f);
    dl->AddPolyline(pts.data(), n, IM_COL32(255, 255, 255, 230), 0, 1.5f);
}
