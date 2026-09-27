// Music visualisation: FFT analysis of the master output and several
// audio-reactive display modes drawn with ImGui draw lists.
#pragma once
#include <vector>

#include "imgui.h"
#include "sokol_gfx.h"

class Engine;

class Visualizer {
public:
    static constexpr int kFftSize = 2048;
    static constexpr int kBars = 64;
    static constexpr int kSpecW = 512, kSpecH = 160;

    enum Mode { Spectrum, Radial, Spectrogram, Vectorscope, Tunnel, Scope, NumModes };
    static const char* modeName(int m);

    void init();
    void shutdown();
    // Reads the master scope from the engine and analyses it. Caller holds engine.mutex.
    void update(Engine& engine, float dt);
    // Must be called once per frame before rendering (uploads the spectrogram).
    void uploadTextures();
    void draw(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int mode);

    int mode = Radial;
    bool autoCycle = false;
    float bass = 0, mids = 0, treble = 0, level = 0;
    float beatPulse = 0;       // 1 on each beat of the master deck, decays
    float kick = 0;            // bass onset detector output
    double masterBeat = 0;
    bool masterPlaying = false;

private:
    std::vector<float> scope_ = std::vector<float>(kFftSize * 2, 0.0f);
    float bars_[kBars] = {}, peaks_[kBars] = {}, peakVel_[kBars] = {};
    float bassAvg_ = 0;
    float time_ = 0, hueShift_ = 0, tunnelZ_ = 0;
    double lastBeat_ = -1;
    float cycleTimer_ = 0;

    struct Particle {
        float x, y, vx, vy, life, maxLife, hue, size;
    };
    std::vector<Particle> particles_;
    std::vector<std::vector<ImVec2>> trails_;  // vectorscope persistence

    std::vector<uint32_t> specPixels_ = std::vector<uint32_t>(size_t(kSpecW) * kSpecH, 0xFF000000u);
    int specCol_ = 0;
    sg_image specImg_{};
    sg_view specView_{};
    sg_sampler specSmp_{};

    void analyse(float dt);
    void spawnBurst(float cx, float cy, int count, float speed);
    void drawSpectrum(ImDrawList* dl, ImVec2 p0, ImVec2 p1);
    void drawRadial(ImDrawList* dl, ImVec2 p0, ImVec2 p1);
    void drawSpectrogram(ImDrawList* dl, ImVec2 p0, ImVec2 p1);
    void drawVectorscope(ImDrawList* dl, ImVec2 p0, ImVec2 p1);
    void drawTunnel(ImDrawList* dl, ImVec2 p0, ImVec2 p1);
    void drawScope(ImDrawList* dl, ImVec2 p0, ImVec2 p1);
    void drawParticles(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float dt);
};
