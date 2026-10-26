#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Greg Lamb
// Small, dependency-free DSP helpers for the sub-audible path.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace frsmon::sa {

constexpr double kPi = 3.14159265358979323846;

// Windowed-sinc (Blackman) low-pass FIR. Odd tap count, unity DC gain.
inline std::vector<float> lowPassTaps(double cutoffHz, double transitionHz, double sampleRate) {
    int n = (int)std::ceil(5.5 * sampleRate / transitionHz);  // Blackman: ~5.5/transition
    if (n % 2 == 0) { n++; }
    n = std::max(n, 11);
    std::vector<float> taps(n);
    const int m = n - 1;
    const double fc = cutoffHz / sampleRate;
    double sum = 0.0;
    for (int i = 0; i < n; i++) {
        const double k = i - m / 2.0;
        double sinc = (k == 0.0) ? 2.0 * fc : std::sin(2.0 * kPi * fc * k) / (kPi * k);
        double w = 0.42 - 0.5 * std::cos(2.0 * kPi * i / m) + 0.08 * std::cos(4.0 * kPi * i / m);
        taps[i] = (float)(sinc * w);
        sum += taps[i];
    }
    for (auto& t : taps) { t = (float)(t / sum); }
    return taps;
}

// Streaming FIR with integer decimation (real input).
class DecimatingFIR {
public:
    DecimatingFIR() = default;
    DecimatingFIR(std::vector<float> taps, int decim) { init(std::move(taps), decim); }

    void init(std::vector<float> taps, int decim) {
        _taps = std::move(taps);
        _decim = std::max(1, decim);
        _hist.assign(_taps.size() - 1, 0.0f);
        _phase = 0;
    }

    void reset() {
        std::fill(_hist.begin(), _hist.end(), 0.0f);
        _phase = 0;
    }

    // Returns number of output samples written to out (out needs count/decim + 1 room).
    int process(const float* in, int count, float* out) {
        const int nt = (int)_taps.size();
        // Build a contiguous buffer: history + new samples
        _work.resize(_hist.size() + count);
        std::memcpy(_work.data(), _hist.data(), _hist.size() * sizeof(float));
        std::memcpy(_work.data() + _hist.size(), in, count * sizeof(float));
        int produced = 0;
        // Output sample k uses work[k*decim + phase .. +nt-1]
        int i = _phase;
        for (; i + nt <= (int)_work.size(); i += _decim) {
            float acc = 0.0f;
            const float* w = _work.data() + i;
            for (int t = 0; t < nt; t++) { acc += w[t] * _taps[t]; }
            out[produced++] = acc;
        }
        // Keep the last nt-1 samples as history; carry the decimation phase.
        const int consumed = (int)_work.size() - (nt - 1);
        _phase = i - consumed;  // position of the next output relative to new history start
        std::memcpy(_hist.data(), _work.data() + consumed, (nt - 1) * sizeof(float));
        return produced;
    }

    int decimation() const { return _decim; }
    int tapCount() const { return (int)_taps.size(); }

private:
    std::vector<float> _taps;
    std::vector<float> _hist;
    std::vector<float> _work;
    int _decim = 1;
    int _phase = 0;
};

// First-order DC blocker (high-pass), corner frequency in Hz.
class DCBlocker {
public:
    DCBlocker() = default;
    DCBlocker(double cornerHz, double sampleRate) { init(cornerHz, sampleRate); }
    void init(double cornerHz, double sampleRate) {
        _a = (float)std::exp(-2.0 * kPi * cornerHz / sampleRate);
        reset();
    }
    void reset() { _x1 = 0.0f; _y1 = 0.0f; }
    inline float process(float x) {
        float y = x - _x1 + _a * _y1;
        _x1 = x;
        _y1 = y;
        return y;
    }
    void process(float* data, int count) {
        for (int i = 0; i < count; i++) { data[i] = process(data[i]); }
    }
private:
    float _a = 0.999f, _x1 = 0.0f, _y1 = 0.0f;
};

// Slow running mean (for the discriminator DC = frequency error).
class SlowMean {
public:
    void init(double tauSeconds, double sampleRate) {
        _a = (float)std::exp(-1.0 / (tauSeconds * sampleRate));
        _v = 0.0f;
    }
    inline void push(float x) { _v = _a * _v + (1.0f - _a) * x; }
    float value() const { return _v; }
    void reset() { _v = 0.0f; }
private:
    float _a = 0.999f, _v = 0.0f;
};

// Fixed-capacity ring buffer of floats; readLast() copies the newest n samples in order.
class RingBuffer {
public:
    void init(size_t capacity) {
        _buf.assign(capacity, 0.0f);
        _cap = capacity;
        _head = 0;
        _count = 0;
    }
    void clear() { _head = 0; _count = 0; }
    void push(const float* x, int n) {
        for (int i = 0; i < n; i++) {
            _buf[_head] = x[i];
            _head = (_head + 1) % _cap;
        }
        _count = std::min(_cap, _count + (size_t)n);
    }
    size_t size() const { return _count; }
    size_t capacity() const { return _cap; }
    // Copy the last n samples (n <= size()) into out, oldest first.
    void readLast(size_t n, float* out) const {
        n = std::min(n, _count);
        size_t start = (_head + _cap - n) % _cap;
        for (size_t i = 0; i < n; i++) { out[i] = _buf[(start + i) % _cap]; }
    }
private:
    std::vector<float> _buf;
    size_t _cap = 0, _head = 0, _count = 0;
};

} // namespace frsmon::sa
