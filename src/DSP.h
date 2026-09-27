// Small real-time DSP building blocks used by the mixer.
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

#include "Track.h"

namespace dsp {

constexpr float kPi = 3.14159265358979f;

inline float dbToGain(float db) { return std::pow(10.0f, db / 20.0f); }

// EQ knob position (0..1, 0.5 = flat) to decibels. 0 is a full kill.
inline float eqKnobToDb(float v) {
    if (v >= 0.5f) return (v - 0.5f) * 2.0f * 6.0f;  // up to +6 dB
    float g = v / 0.5f;
    return 20.0f * std::log10(std::max(g * g, 0.001f));  // down to -60 dB
}

// Stereo RBJ biquad, transposed direct form II.
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1[2] = {0, 0}, z2[2] = {0, 0};

    void setNormalized(float nb0, float nb1, float nb2, float a0, float na1, float na2) {
        b0 = nb0 / a0; b1 = nb1 / a0; b2 = nb2 / a0; a1 = na1 / a0; a2 = na2 / a0;
    }
    void lowShelf(float hz, float db) {
        float A = std::pow(10.0f, db / 40.0f), w = 2 * kPi * hz / kSampleRate;
        float cs = std::cos(w), sn = std::sin(w), alpha = sn / 2 * std::sqrt(2.0f), sa = 2 * std::sqrt(A) * alpha;
        setNormalized(A * ((A + 1) - (A - 1) * cs + sa), 2 * A * ((A - 1) - (A + 1) * cs), A * ((A + 1) - (A - 1) * cs - sa),
                      (A + 1) + (A - 1) * cs + sa, -2 * ((A - 1) + (A + 1) * cs), (A + 1) + (A - 1) * cs - sa);
    }
    void highShelf(float hz, float db) {
        float A = std::pow(10.0f, db / 40.0f), w = 2 * kPi * hz / kSampleRate;
        float cs = std::cos(w), sn = std::sin(w), alpha = sn / 2 * std::sqrt(2.0f), sa = 2 * std::sqrt(A) * alpha;
        setNormalized(A * ((A + 1) + (A - 1) * cs + sa), -2 * A * ((A - 1) + (A + 1) * cs), A * ((A + 1) + (A - 1) * cs - sa),
                      (A + 1) - (A - 1) * cs + sa, 2 * ((A - 1) - (A + 1) * cs), (A + 1) - (A - 1) * cs - sa);
    }
    void peak(float hz, float q, float db) {
        float A = std::pow(10.0f, db / 40.0f), w = 2 * kPi * hz / kSampleRate;
        float cs = std::cos(w), alpha = std::sin(w) / (2 * q);
        setNormalized(1 + alpha * A, -2 * cs, 1 - alpha * A, 1 + alpha / A, -2 * cs, 1 - alpha / A);
    }
    float process(float x, int ch) {
        float y = b0 * x + z1[ch];
        z1[ch] = b1 * x - a1 * y + z2[ch];
        z2[ch] = b2 * x - a2 * y;
        return y;
    }
    void reset() { z1[0] = z1[1] = z2[0] = z2[1] = 0; }
};

// Three-band DJ EQ. Coefficients only recomputed when a knob moves.
struct ThreeBandEq {
    Biquad low, mid, high;
    float cur[3] = {-1, -1, -1};

    void set(float lowKnob, float midKnob, float highKnob) {
        if (lowKnob != cur[0]) low.lowShelf(220.0f, eqKnobToDb(cur[0] = lowKnob));
        if (midKnob != cur[1]) mid.peak(1000.0f, 0.6f, eqKnobToDb(cur[1] = midKnob));
        if (highKnob != cur[2]) high.highShelf(4000.0f, eqKnobToDb(cur[2] = highKnob));
    }
    bool flat() const { return cur[0] == 0.5f && cur[1] == 0.5f && cur[2] == 0.5f; }
    void process(float& l, float& r) {
        l = high.process(mid.process(low.process(l, 0), 0), 0);
        r = high.process(mid.process(low.process(r, 1), 1), 1);
    }
};

// Zavalishin TPT state-variable filter. One knob: <0.5 low-pass, >0.5 high-pass.
struct DjFilter {
    float ic1[2] = {0, 0}, ic2[2] = {0, 0};
    float g = 0, k = 1.0f, a1 = 0, a2 = 0, a3 = 0;
    float knob = 0.5f;
    bool lowpass = true;

    void set(float v) {
        knob = v;
        float hz;
        if (v < 0.5f) {  // 0.5 -> 20 kHz, 0 -> 60 Hz
            lowpass = true;
            hz = 60.0f * std::pow(20000.0f / 60.0f, v / 0.5f);
        } else {  // 0.5 -> 20 Hz, 1 -> 9 kHz
            lowpass = false;
            hz = 20.0f * std::pow(9000.0f / 20.0f, (v - 0.5f) / 0.5f);
        }
        hz = std::min(hz, 20000.0f);
        g = std::tan(kPi * hz / kSampleRate);
        k = 1.0f / 1.3f;  // slight resonance for character
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    bool bypassed() const { return std::fabs(knob - 0.5f) < 0.015f; }
    float process(float x, int ch) {
        float v3 = x - ic2[ch];
        float v1 = a1 * ic1[ch] + a2 * v3;
        float v2 = ic2[ch] + a2 * ic1[ch] + a3 * v3;
        ic1[ch] = 2 * v1 - ic1[ch];
        ic2[ch] = 2 * v2 - ic2[ch];
        if (lowpass) return v2;
        return x - k * v1 - v2;
    }
    void reset() { ic1[0] = ic1[1] = ic2[0] = ic2[1] = 0; }
};

// Tempo-synced stereo ping-pong echo.
struct Echo {
    std::vector<float> buf;  // interleaved stereo
    size_t write = 0;
    float delayFrames = 10000;
    float feedback = 0.55f;
    float dampL = 0, dampR = 0;

    Echo() : buf(size_t(kSampleRate) * 3 * 2, 0.0f) {}

    void process(float inL, float inR, float& outL, float& outR) {
        const size_t len = buf.size() / 2;
        float d = std::clamp(delayFrames, 1.0f, float(len - 2));
        double rp = double(write) - d;
        if (rp < 0) rp += double(len);
        size_t i0 = size_t(rp) % len, i1 = (i0 + 1) % len;
        float f = float(rp - std::floor(rp));
        float dl = buf[i0 * 2] * (1 - f) + buf[i1 * 2] * f;
        float dr = buf[i0 * 2 + 1] * (1 - f) + buf[i1 * 2 + 1] * f;
        // Darken repeats slightly so the tail sounds like tape.
        dampL += 0.35f * (dl - dampL);
        dampR += 0.35f * (dr - dampR);
        // Ping-pong: feed each side's repeats into the other side.
        buf[write * 2] = inL + dampR * feedback;
        buf[write * 2 + 1] = inR + dampL * feedback;
        write = (write + 1) % len;
        outL = dampL;
        outR = dampR;
    }
    void clear() {
        std::fill(buf.begin(), buf.end(), 0.0f);
        dampL = dampR = 0;
    }
};

// Look-ahead-free peak limiter with a soft-clip safety stage.
struct Limiter {
    float env = 0;
    float gainReduction = 1.0f;  // exposed for the UI
    void process(float& l, float& r) {
        const float threshold = 0.89f;  // about -1 dBFS
        float peak = std::max(std::fabs(l), std::fabs(r));
        float attack = 0.9f, release = 0.9998f;
        env = peak > env ? attack * env + (1 - attack) * peak : release * env + (1 - release) * peak;
        env = std::max(env, peak * 0.9f);
        float g = env > threshold ? threshold / env : 1.0f;
        gainReduction = g;
        l = softClip(l * g);
        r = softClip(r * g);
    }
    static float softClip(float x) {
        const float t = 0.95f;
        if (std::fabs(x) <= t) return x;
        float s = x > 0 ? 1.0f : -1.0f;
        float over = std::fabs(x) - t;
        return s * (t + (1 - t) * std::tanh(over / (1 - t)));
    }
};

// One-pole parameter smoother to avoid zipper noise.
struct Smoother {
    float value = 0;
    void snap(float v) { value = v; }
    float next(float target, float coef = 0.0025f) {
        value += (target - value) * coef;
        return value;
    }
};

// 4-point cubic Hermite interpolation.
inline float hermite(float xm1, float x0, float x1, float x2, float t) {
    float c = (x1 - xm1) * 0.5f;
    float v = x0 - x1;
    float w = c + v;
    float a = w + v + (x2 - x0) * 0.5f;
    float b = w + a;
    return ((((a * t) - b) * t + c) * t + x0);
}

}  // namespace dsp
