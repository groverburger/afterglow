// Audio file import/export and track analysis (waveform bins, loudness, BPM).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include "Track.h"
#include "dr_flac.h"
#include "dr_mp3.h"
#include "dr_wav.h"

namespace {

// One-pole low-pass coefficient for a cutoff frequency.
float onePoleCoef(float hz) { return 1.0f - std::exp(-2.0f * 3.14159265f * hz / kSampleRate); }

std::string lowerExt(const std::string& path) {
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string e = path.substr(dot + 1);
    for (auto& c : e) c = char(std::tolower((unsigned char)c));
    return e;
}

std::string fileStem(const std::string& path) {
    auto slash = path.find_last_of("/\\");
    std::string f = slash == std::string::npos ? path : path.substr(slash + 1);
    auto dot = f.find_last_of('.');
    return dot == std::string::npos ? f : f.substr(0, dot);
}

}  // namespace

void analyzeTrack(Track& t) {
    const size_t frames = t.frames();
    const size_t numBins = (frames + kWaveBinFrames - 1) / kWaveBinFrames;
    t.wave.assign(numBins, WaveBin{});
    if (frames == 0) return;

    const float cLow = onePoleCoef(200.0f), cMid = onePoleCoef(2500.0f);
    float lp1 = 0, lp2 = 0;
    double sumSq = 0;
    size_t loudFrames = 0;
    float maxPeak = 1e-6f, maxBand = 1e-6f;

    for (size_t b = 0; b < numBins; ++b) {
        size_t start = b * kWaveBinFrames, end = std::min(frames, start + kWaveBinFrames);
        float peak = 0, eL = 0, eM = 0, eH = 0;
        double binSq = 0;
        for (size_t i = start; i < end; ++i) {
            float x = 0.5f * (t.samples[i * 2] + t.samples[i * 2 + 1]);
            lp1 += cLow * (x - lp1);
            lp2 += cMid * (x - lp2);
            float lo = lp1, mi = lp2 - lp1, hi = x - lp2;
            eL += lo * lo;
            eM += mi * mi;
            eH += hi * hi;
            peak = std::max(peak, std::fabs(x));
            binSq += double(x) * x;
        }
        float n = float(end - start);
        WaveBin& w = t.wave[b];
        w.peak = peak;
        w.low = std::sqrt(eL / n);
        w.mid = std::sqrt(eM / n);
        w.high = std::sqrt(eH / n);
        maxPeak = std::max(maxPeak, peak);
        maxBand = std::max({maxBand, w.low, w.mid, w.high});
        // Loudness only over non-silent parts so long silences don't skew it.
        if (binSq / n > 1e-6) {
            sumSq += binSq;
            loudFrames += end - start;
        }
    }
    for (auto& w : t.wave) {
        w.peak /= maxPeak;
        w.low = std::pow(std::min(1.0f, w.low / maxBand), 0.75f);
        w.mid = std::pow(std::min(1.0f, w.mid / maxBand * 1.6f), 0.75f);
        w.high = std::pow(std::min(1.0f, w.high / maxBand * 2.5f), 0.75f);
    }

    // Normalise every track to roughly the same loudness (-11 dBFS RMS).
    double rms = loudFrames ? std::sqrt(sumSq / double(loudFrames)) : 0.0;
    const double target = std::pow(10.0, -11.0 / 20.0);
    t.autoGain = rms > 1e-5 ? float(std::clamp(target / rms, 0.25, 4.0)) : 1.0f;
}

double detectBpm(const Track& t, double* firstBeatSec) {
    const int hop = 441;  // 100 onset frames per second
    const double fps = double(kSampleRate) / hop;
    const size_t frames = t.frames();
    const size_t n = frames / hop;
    if (firstBeatSec) *firstBeatSec = 0.0;
    if (n < 400) return 120.0;

    // Onset strength: rectified energy rise, full band plus boosted low band.
    std::vector<float> onset(n, 0.0f), lowOnset(n, 0.0f);
    const float cLow = onePoleCoef(150.0f);
    float lp = 0, prevE = 0, prevL = 0;
    for (size_t h = 0; h < n; ++h) {
        float e = 0, el = 0;
        for (int i = 0; i < hop; ++i) {
            size_t f = h * hop + i;
            float x = 0.5f * (t.samples[f * 2] + t.samples[f * 2 + 1]);
            lp += cLow * (x - lp);
            e += x * x;
            el += lp * lp;
        }
        e = std::log1p(e * 100.0f);
        el = std::log1p(el * 100.0f);
        onset[h] = std::max(0.0f, e - prevE) + 1.5f * std::max(0.0f, el - prevL);
        lowOnset[h] = std::max(0.0f, el - prevL);
        prevE = e;
        prevL = el;
    }
    // Remove local mean so steady noise doesn't dominate.
    {
        std::vector<float> smooth(n);
        const int w = 16;
        double acc = 0;
        for (size_t i = 0; i < n; ++i) {
            acc += onset[i];
            if (i >= size_t(2 * w)) acc -= onset[i - 2 * w];
            smooth[i] = float(acc / std::min<size_t>(i + 1, 2 * w));
        }
        for (size_t i = 0; i < n; ++i) onset[i] = std::max(0.0f, onset[i] - smooth[std::min(n - 1, i + w)]);
    }

    auto sample = [&](const std::vector<float>& v, double pos) -> float {
        if (pos < 0) return 0;
        size_t i = size_t(pos);
        if (i + 1 >= v.size()) return 0;
        float f = float(pos - double(i));
        return v[i] * (1 - f) + v[i + 1] * f;
    };

    // Stage 1: coarse autocorrelation picks the tempo octave.
    double coarse = 120.0, bestScore = -1.0;
    for (double bpm = 70.0; bpm <= 180.0; bpm += 0.1) {
        double lag = 60.0 * fps / bpm;
        double score = 0;
        for (size_t i = 0; i < n; i += 2) {
            float o = onset[i];
            if (o <= 0) continue;
            score += o * (sample(onset, i + lag) + 0.5f * sample(onset, i + 2 * lag) + 0.25f * sample(onset, i + 4 * lag));
        }
        // Mild preference for common dance tempos (only matters between octaves).
        double pref = std::exp(-0.5 * std::pow(std::log2(bpm / 124.0) / 0.9, 2.0));
        score *= 0.8 + 0.2 * pref;
        if (score > bestScore) {
            bestScore = score;
            coarse = bpm;
        }
    }

    // Stage 2: fit a beat grid over the whole track. Small tempo errors add
    // up over hundreds of beats, so this is far more precise than stage 1.
    auto gridFit = [&](double bpm, double phaseStep, double* phaseOut) {
        const double period = 60.0 * fps / bpm;
        double best = -1, bestPh = 0;
        for (double ph = 0; ph < period; ph += phaseStep) {
            double sum = 0;
            for (double p = ph; p < double(n); p += period) sum += sample(onset, p);
            if (sum > best) {
                best = sum;
                bestPh = ph;
            }
        }
        if (phaseOut) *phaseOut = bestPh;
        return best;
    };
    auto refine = [&](double centre, double span, double* fitOut) {
        double best = centre, bestFit = -1;
        for (double bpm = centre - span; bpm <= centre + span; bpm += 0.01) {
            double fit = gridFit(bpm, 0.5, nullptr);
            if (fit > bestFit) {
                bestFit = fit;
                best = bpm;
            }
        }
        *fitOut = bestFit;
        return best;
    };
    double bestFit;
    double bestBpm = refine(coarse, 0.6, &bestFit);
    // Swung rhythms can fool stage 1 by a factor of 3:2. Compare the average
    // grid hit per beat and switch only for a clear win.
    double perBeat = bestFit / bestBpm;  // grid hits scale with the number of beats
    for (double ratio : {2.0 / 3.0, 1.5}) {
        double alt = coarse * ratio;
        if (alt < 70.0 || alt > 180.0) continue;
        double altFit;
        double altBpm = refine(alt, 1.5, &altFit);
        if (altFit / altBpm > perBeat * 1.15) {
            bestBpm = altBpm;
            bestFit = altFit;
            perBeat = altFit / altBpm;
        }
    }
    // Most music sits on a whole-number tempo; snap when the fit agrees.
    double rounded = std::round(bestBpm);
    if (std::fabs(rounded - bestBpm) < 0.06 && gridFit(rounded, 0.5, nullptr) > 0.97 * bestFit) bestBpm = rounded;
    else bestBpm = std::round(bestBpm * 100.0) / 100.0;

    const double period = 60.0 * fps / bestBpm;
    double bestPhase = 0;
    gridFit(bestBpm, 0.1, &bestPhase);
    // Downbeat: the beat of the bar with the strongest low-end hits.
    double bestDown = bestPhase, bestDownScore = -1;
    for (int j = 0; j < 4; ++j) {
        double start = bestPhase + j * period, s = 0;
        for (double p = start; p < double(n); p += 4 * period) s += sample(lowOnset, p);
        if (s > bestDownScore) {
            bestDownScore = s;
            bestDown = start;
        }
    }
    if (firstBeatSec) *firstBeatSec = std::fmod(bestDown, 4 * period) / fps;
    return bestBpm;
}

TrackPtr loadAudioFile(const std::string& path, std::string* error) {
    std::string ext = lowerExt(path);
    unsigned int channels = 0, rate = 0;
    drwav_uint64 count = 0;
    float* pcm = nullptr;
    void (*freeFn)(void*) = nullptr;

    if (ext == "wav") {
        pcm = drwav_open_file_and_read_pcm_frames_f32(path.c_str(), &channels, &rate, &count, nullptr);
        freeFn = [](void* p) { drwav_free(p, nullptr); };
    } else if (ext == "mp3") {
        drmp3_config cfg{};
        drmp3_uint64 c = 0;
        pcm = drmp3_open_file_and_read_pcm_frames_f32(path.c_str(), &cfg, &c, nullptr);
        channels = cfg.channels;
        rate = cfg.sampleRate;
        count = c;
        freeFn = [](void* p) { drmp3_free(p, nullptr); };
    } else if (ext == "flac") {
        drflac_uint64 c = 0;
        pcm = drflac_open_file_and_read_pcm_frames_f32(path.c_str(), &channels, &rate, &c, nullptr);
        count = c;
        freeFn = [](void* p) { drflac_free(p, nullptr); };
    } else {
        if (error) *error = "Unsupported file type '." + ext + "' (use WAV, MP3 or FLAC)";
        return nullptr;
    }
    if (!pcm || channels == 0 || rate == 0 || count == 0) {
        if (pcm) freeFn(pcm);
        if (error) *error = "Could not decode " + fileStem(path) + "." + ext;
        return nullptr;
    }
    if (double(count) / rate > 60.0 * 30.0) {
        freeFn(pcm);
        if (error) *error = "File is longer than 30 minutes; please trim it first";
        return nullptr;
    }

    auto t = std::make_shared<Track>();
    t->name = fileStem(path);
    t->artist = "Imported";
    t->genre = "Imported";
    t->key = "?";
    t->sourcePath = path;

    // Resample to kSampleRate stereo with linear interpolation.
    const double step = double(rate) / kSampleRate;
    const size_t outFrames = size_t(double(count) / step);
    t->samples.resize(outFrames * 2);
    for (size_t i = 0; i < outFrames; ++i) {
        double src = i * step;
        size_t i0 = size_t(src);
        size_t i1 = std::min<size_t>(i0 + 1, size_t(count) - 1);
        float f = float(src - double(i0));
        for (int ch = 0; ch < 2; ++ch) {
            unsigned sc = std::min<unsigned>(unsigned(ch), channels - 1);
            float a = pcm[i0 * channels + sc], b = pcm[i1 * channels + sc];
            t->samples[i * 2 + ch] = a + (b - a) * f;
        }
    }
    freeFn(pcm);

    t->bpm = detectBpm(*t, &t->firstBeatSec);
    analyzeTrack(*t);
    return t;
}

bool writeWav(const std::string& path, const float* interleaved, size_t frames) {
    drwav_data_format fmt{};
    fmt.container = drwav_container_riff;
    fmt.format = DR_WAVE_FORMAT_PCM;
    fmt.channels = 2;
    fmt.sampleRate = kSampleRate;
    fmt.bitsPerSample = 16;
    drwav wav;
    if (!drwav_init_file_write(&wav, path.c_str(), &fmt, nullptr)) return false;
    std::vector<int16_t> buf(frames * 2);
    for (size_t i = 0; i < frames * 2; ++i) {
        float s = std::clamp(interleaved[i], -1.0f, 1.0f);
        buf[i] = int16_t(std::lrint(s * 32767.0f));
    }
    drwav_uint64 written = drwav_write_pcm_frames(&wav, frames, buf.data());
    drwav_uninit(&wav);
    return written == frames;
}
