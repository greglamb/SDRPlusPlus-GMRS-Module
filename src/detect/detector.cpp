// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Greg Lamb
#include "detector.h"
#include "tables.h"

#include <fftw3.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <map>
#include <numeric>

namespace frsmon {

// ------------------------------------------------------------ utilities ---

std::string dcsOctal(int code) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%03o", code);
    return buf;
}

std::string DetectResult::label() const {
    char buf[32];
    switch (kind) {
    case CodeKind::DCS: std::snprintf(buf, sizeof(buf), "D%03oN", dcsCode); return buf;
    case CodeKind::CTCSS: std::snprintf(buf, sizeof(buf), "%.1f Hz", ctcssHz); return buf;
    default: return "";
    }
}

std::string DetectResult::privacyLabel() const {
    int n = (kind == CodeKind::DCS) ? dcsNumber : (kind == CodeKind::CTCSS ? ctcssNumber : 0);
    if (n <= 0) { return ""; }
    return "privacy " + std::to_string(n);
}

namespace {

std::mutex g_fftwPlanMutex;   // FFTW planning is not thread-safe

inline int pow2ceil(int n) {
    int p = 1;
    while (p < n) { p <<= 1; }
    return p;
}

// Real FFT of arbitrary size with its own aligned buffers.
struct RealFFT {
    int n = 0;
    float* in = nullptr;
    fftwf_complex* out = nullptr;
    fftwf_plan fwd = nullptr;
    fftwf_plan inv = nullptr;   // c2r, in-place pair out->in

    void init(int size) {
        n = size;
        in = fftwf_alloc_real(n);
        out = fftwf_alloc_complex(n / 2 + 1);
        std::lock_guard<std::mutex> lck(g_fftwPlanMutex);
        fwd = fftwf_plan_dft_r2c_1d(n, in, out, FFTW_ESTIMATE);
        inv = fftwf_plan_dft_c2r_1d(n, out, in, FFTW_ESTIMATE);
    }
    ~RealFFT() {
        if (fwd) { std::lock_guard<std::mutex> lck(g_fftwPlanMutex); fftwf_destroy_plan(fwd); fftwf_destroy_plan(inv); }
        if (in) { fftwf_free(in); }
        if (out) { fftwf_free(out); }
    }
    void forward() { fftwf_execute(fwd); }
    void inverse() { fftwf_execute(inv); }   // unnormalised: divide by n
};

std::vector<float> hann(int n) {
    std::vector<float> w(n);
    for (int i = 0; i < n; i++) { w[i] = (float)(0.5 - 0.5 * std::cos(2.0 * sa::kPi * i / n)); }  // numpy.hanning uses n-1; close enough
    return w;
}

// numpy-style linear percentile of a small vector (modifies v)
float percentile(std::vector<float>& v, float p) {
    if (v.empty()) { return 0.0f; }
    std::sort(v.begin(), v.end());
    double pos = p / 100.0 * (v.size() - 1);
    size_t lo = (size_t)std::floor(pos);
    size_t hi = std::min(v.size() - 1, lo + 1);
    double frac = pos - lo;
    return (float)(v[lo] * (1.0 - frac) + v[hi] * frac);
}

float median(std::vector<float> v) { return percentile(v, 50.0f); }

} // namespace

// ----------------------------------------------------- SubAudibleDetector ---

struct SubAudibleDetector::Impl {
    int fsd;
    std::map<int, std::unique_ptr<RealFFT>> ffts;       // by size
    std::map<int, std::vector<float>> hanns;            // by size
    RealFFT wordFFT;                                    // size DCS_WL, for folds
    struct Cpx { float re, im; };
    struct Template { std::vector<float> period; std::vector<Cpx> spec; float norm; };
    std::map<int, std::map<int, Template>> templates;   // [rateQ][code]

    explicit Impl(int fsd_) : fsd(fsd_) { wordFFT.init(DCS_WL); }

    RealFFT& fft(int n) {
        auto it = ffts.find(n);
        if (it == ffts.end()) {
            auto f = std::make_unique<RealFFT>();
            f->init(n);
            it = ffts.emplace(n, std::move(f)).first;
        }
        return *it->second;
    }
    const std::vector<float>& hannWin(int n) {
        auto it = hanns.find(n);
        if (it == hanns.end()) { it = hanns.emplace(n, hann(n)).first; }
        return it->second;
    }

    static int rateQ(double rate) { return (int)std::lround(rate / 0.15); }
    static double rateFromQ(int q) { return q * 0.15; }

    // One band-limited period of the code's NRZ word at DCS_SPB samples/bit.
    const Template& templ(int q, int code) {
        auto& byCode = templates[q];
        auto it = byCode.find(code);
        if (it != byCode.end()) { return it->second; }
        const double rate = rateFromQ(q);
        const double fsr = DCS_SPB * rate;
        uint32_t w = dcsWord(code);
        std::vector<float> nrz(7 * DCS_WL);
        for (int i = 0; i < 7 * DCS_WL; i++) {
            int bit = (w >> ((i / DCS_SPB) % 23)) & 1;
            nrz[i] = bit ? 1.0f : -1.0f;
        }
        // Same shaping as the signal path: 2 Hz one-pole DC block, then 150 Hz low-pass.
        // (The DC block shifts the lowest DCS harmonics by up to ~20 degrees; the
        // template must see the same shift or the correlation drops.)
        sa::DCBlocker dcb(2.0, fsr);
        dcb.process(nrz.data(), (int)nrz.size());
        sa::DecimatingFIR lp(sa::lowPassTaps(150.0, 60.0, fsr), 1);
        std::vector<float> filt(nrz.size() + 1);
        int m = lp.process(nrz.data(), (int)nrz.size(), filt.data());
        // the FIR delays by (taps-1)/2 samples; take a full period well past the transient
        int start = 3 * DCS_WL;
        Template t;
        t.period.assign(filt.begin() + start, filt.begin() + start + DCS_WL);
        (void)m;
        double ss = 0; for (float v : t.period) { ss += v * v; }
        t.norm = (float)std::sqrt(ss);
        for (auto& v : t.period) { v /= t.norm; }
        t.norm = 1.0f;
        // spectrum
        std::copy(t.period.begin(), t.period.end(), wordFFT.in);
        wordFFT.forward();
        t.spec.resize(DCS_WL / 2 + 1);
        for (int k = 0; k <= DCS_WL / 2; k++) { t.spec[k] = {wordFFT.out[k][0], wordFFT.out[k][1]}; }
        return byCode.emplace(code, std::move(t)).first->second;
    }
};

SubAudibleDetector::SubAudibleDetector(int fsd) : _impl(new Impl(fsd)), _fsd(fsd) {}
SubAudibleDetector::~SubAudibleDetector() = default;

SubAudibleDetector::DcsRaw SubAudibleDetector::detectDcs(const float* y150, int n) {
    DcsRaw out;
    if (n < _fsd / 2) { return out; }
    std::vector<float> seg(y150, y150 + n);
    double mean = std::accumulate(seg.begin(), seg.end(), 0.0) / n;
    for (auto& v : seg) { v = (float)(v - mean); }
    double energy = 0; for (float v : seg) { energy += v * v; }
    if (energy <= 0) { return out; }

    // --- bit-rate estimate from the comb (harmonic sum over the fundamental) ---
    double rate0 = DCS_NOMINAL_BPS;
    {
        int nfft = pow2ceil(8 * n);
        RealFFT& F = _impl->fft(nfft);
        const auto& w = _impl->hannWin(n);
        std::fill(F.in, F.in + nfft, 0.0f);
        for (int i = 0; i < n; i++) { F.in[i] = seg[i] * w[i]; }
        F.forward();
        const double df = (double)_fsd / nfft;
        const int nb = nfft / 2 + 1;
        std::vector<float> db(nb);
        for (int i = 0; i < nb; i++) {
            float re = F.out[i][0], im = F.out[i][1];
            db[i] = 20.0f * std::log10(std::sqrt(re * re + im * im) + 1e-12f);
        }
        double best = -1e30, bestF0 = DCS_NOMINAL_BPS / 23.0;
        for (double f0 = 5.70; f0 <= 5.96 + 1e-9; f0 += 0.005) {
            double s = 0;
            for (int k = 1; k <= 20; k++) {
                int idx = (int)std::lround(k * f0 / df);
                if (idx < nb) { s += db[idx]; }
            }
            if (s > best) { best = s; bestF0 = f0; }
        }
        rate0 = 23.0 * bestF0;
    }
    const int q0 = Impl::rateQ(rate0);

    // --- fold at candidate rates, correlate against all codewords ---
    struct Cand { int code; float corr; float margin; float pf; float rate; int words; };
    bool haveBest = false; Cand best{};
    std::vector<float> fold(DCS_WL), cc(DCS_WL);
    for (int dq = -2; dq <= 2; dq++) {
        const int q = q0 + dq;
        const double rate = Impl::rateFromQ(q);
        const double fsr = DCS_SPB * rate;
        const int nwords = (int)std::floor((double)n / _fsd * fsr / DCS_WL);
        if (nwords < 2) { continue; }
        // resample (linear) to fsr and fold
        std::fill(fold.begin(), fold.end(), 0.0f);
        double rsum = 0;
        for (int j = 0; j < nwords * DCS_WL; j++) {
            double ts = j / fsr * _fsd;      // position in input samples
            int i0 = (int)ts;
            if (i0 >= n - 1) { i0 = n - 2; }
            double fr = ts - i0;
            float v = (float)(seg[i0] * (1.0 - fr) + seg[i0 + 1] * fr);
            fold[j % DCS_WL] += v;
            rsum += v * v;
        }
        double fsum = 0;
        for (auto& v : fold) { v /= nwords; fsum += v * v; }
        if (fsum <= 0) { continue; }
        const float pf = (float)(nwords * fsum / (rsum + 1e-20));
        const float fnorm = (float)std::sqrt(fsum);
        // FFT of the fold
        RealFFT& W = _impl->wordFFT;
        std::copy(fold.begin(), fold.end(), W.in);
        W.forward();
        std::vector<Impl::Cpx> fspec(DCS_WL / 2 + 1);
        for (int k = 0; k <= DCS_WL / 2; k++) { fspec[k] = {W.out[k][0], W.out[k][1]}; }
        // correlate with every code
        struct Score { float a; float c; int code; };
        std::vector<Score> scored;
        scored.reserve(DCS_104.size());
        for (int code : DCS_104) {
            const auto& T = _impl->templ(q, code);
            for (int k = 0; k <= DCS_WL / 2; k++) {
                // F * conj(T)
                float ar = fspec[k].re, ai = fspec[k].im;
                float br = T.spec[k].re, bi = -T.spec[k].im;
                W.out[k][0] = ar * br - ai * bi;
                W.out[k][1] = ar * bi + ai * br;
            }
            W.inverse();
            float m = 0; int mi = 0;
            for (int i = 0; i < DCS_WL; i++) {
                float v = W.in[i] / DCS_WL;
                if (std::fabs(v) > std::fabs(m)) { m = v; mi = i; }
            }
            (void)mi;
            float c = m / fnorm;    // template already unit-norm
            scored.push_back({std::fabs(c), c, code});
        }
        std::sort(scored.begin(), scored.end(), [](const Score& a, const Score& b) { return a.a > b.a; });
        const Score& top = scored[0];
        const int inv = dcsInverse(top.code);
        float runnerA = 0.0f;
        for (size_t i = 1; i < scored.size(); i++) {
            if (scored[i].code != inv) { runnerA = scored[i].a; break; }
        }
        Cand cand{top.code, top.c, top.a - runnerA, pf, (float)rate, nwords};
        if (!haveBest || std::fabs(cand.corr) * std::sqrt(cand.pf) > std::fabs(best.corr) * std::sqrt(best.pf)) {
            best = cand; haveBest = true;
        }
    }
    if (!haveBest) { return out; }
    out.code = best.code; out.corr = best.corr; out.inverted = best.corr < 0;
    out.margin = best.margin; out.pf = best.pf; out.rate = best.rate; out.words = best.words;
    return out;
}

SubAudibleDetector::CtcssRaw SubAudibleDetector::detectCtcss(const float* y300, int n) {
    CtcssRaw out;
    if (n < _fsd / 2) { return out; }
    std::vector<float> seg(y300, y300 + n);
    double mean = std::accumulate(seg.begin(), seg.end(), 0.0) / n;
    for (auto& v : seg) { v = (float)(v - mean); }

    const int sub = std::min(n, _fsd / 2);     // 0.5 s sub-windows
    const int hop = std::max(1, sub / 2);
    const int nfft = pow2ceil(8 * sub);
    const double df = (double)_fsd / nfft;
    const int b0 = (int)std::ceil(55.0 / df), b1 = (int)std::floor(265.0 / df);
    const int nb = b1 - b0 + 1;
    RealFFT& F = _impl->fft(nfft);
    const auto& w = _impl->hannWin(sub);

    // spectra of sub-windows, band only
    std::vector<std::vector<float>> specs;
    for (int s = 0; s + sub <= n; s += hop) {
        std::fill(F.in, F.in + nfft, 0.0f);
        for (int i = 0; i < sub; i++) { F.in[i] = seg[s + i] * w[i]; }
        F.forward();
        std::vector<float> db(nb);
        for (int k = 0; k < nb; k++) {
            float re = F.out[b0 + k][0], im = F.out[b0 + k][1];
            db[k] = 20.0f * std::log10(std::sqrt(re * re + im * im) + 1e-12f);
        }
        specs.push_back(std::move(db));
    }
    if (specs.empty()) { return out; }
    std::vector<float> db(nb);
    if (specs.size() == 1) {
        db = specs[0];
    } else {
        std::vector<float> col(specs.size());
        for (int k = 0; k < nb; k++) {
            for (size_t j = 0; j < specs.size(); j++) { col[j] = specs[j][k]; }
            db[k] = percentile(col, 10.0f);
        }
    }
    const float floor = median(db);
    int pi = (int)(std::max_element(db.begin(), db.end()) - db.begin());
    const float peakDb = db[pi];
    const float peakHz = (float)((b0 + pi) * df);
    // second strongest outside the peak's own lobe and harmonics
    float second = floor;
    for (int k = 0; k < nb; k++) {
        double f = (b0 + k) * df;
        if (std::fabs(f - peakHz) <= 6.0) { continue; }
        if (std::fabs(f - 2.0 * peakHz) <= 3.0 || std::fabs(f - 3.0 * peakHz) <= 3.0) { continue; }
        second = std::max(second, db[k]);
    }
    // DCS comb guard
    bool comb = false;
    {
        int k0 = std::max(1, (int)std::lround(peakHz / (DCS_NOMINAL_BPS / 23.0)));
        double f0 = peakHz / k0;
        if (f0 >= 5.70 && f0 <= 5.96) {
            int lines = 0;
            for (int k = 10; k <= 45; k++) {
                if (std::abs(k - k0) <= 2 || k == 2 * k0 || k == 3 * k0) { continue; }
                double fk = k * f0;
                float mx = -1e30f; bool any = false;
                for (int b = 0; b < nb; b++) {
                    if (std::fabs((b0 + b) * df - fk) <= 0.4) { mx = std::max(mx, db[b]); any = true; }
                }
                if (any && mx >= peakDb - 20.0f && mx - floor >= 10.0f) { lines++; }
            }
            comb = lines >= 4;
        }
    }
    // refine on the full window
    float fineHz = peakHz;
    {
        const int nfull = pow2ceil(8 * n);
        RealFFT& G = _impl->fft(nfull);
        const auto& wf = _impl->hannWin(n);
        std::fill(G.in, G.in + nfull, 0.0f);
        for (int i = 0; i < n; i++) { G.in[i] = seg[i] * wf[i]; }
        G.forward();
        const double dff = (double)_fsd / nfull;
        int lo = std::max(1, (int)std::floor((peakHz - 2.0) / dff));
        int hi = std::min(nfull / 2, (int)std::ceil((peakHz + 2.0) / dff));
        float best = -1e30f;
        for (int k = lo; k <= hi; k++) {
            float re = G.out[k][0], im = G.out[k][1];
            float mag = re * re + im * im;
            if (mag > best) { best = mag; fineHz = (float)(k * dff); }
        }
    }
    // nearest standard tone
    float nearest = CTCSS_38[0]; float err = 1e9f;
    for (float t : CTCSS_38) { if (std::fabs(t - fineHz) < err) { err = std::fabs(t - fineHz); nearest = t; } }
    for (float t : CTCSS_EXTRA) { if (std::fabs(t - fineHz) < err) { err = std::fabs(t - fineHz); nearest = t; } }

    out.hz = fineHz; out.prominence = peakDb - floor; out.dominance = peakDb - second;
    out.nearest = nearest; out.err = err; out.comb = comb;
    return out;
}

DetectResult SubAudibleDetector::analyze(const float* y300, const float* y150, int n, bool invert,
                                         DcsRaw* dcsOut, CtcssRaw* ctcssOut) {
    DetectResult r;
    DcsRaw d = detectDcs(y150, n);
    if (dcsOut) { *dcsOut = d; }
    if (d.code && std::fabs(d.corr) >= DCS_MIN_CORR && d.margin >= DCS_MIN_MARGIN && d.pf >= DCS_MIN_PF) {
        bool flipped = d.inverted != invert;
        int code = d.code;
        int shown = flipped ? dcsInverse(code) : code;
        int other = flipped ? code : dcsInverse(code);
        r.kind = CodeKind::DCS;
        r.dcsCode = shown; r.dcsAltCode = other; r.dcsNumber = dcsPrivacyCode(shown);
        r.corr = std::fabs(d.corr); r.margin = d.margin; r.periodicFraction = d.pf; r.bitrate = d.rate;
        r.confidence = r.corr;
        return r;
    }
    CtcssRaw c = detectCtcss(y300, n);
    if (ctcssOut) { *ctcssOut = c; }
    if (c.prominence >= CTCSS_MIN_PROM_DB && c.dominance >= CTCSS_MIN_DOM_DB && c.err <= 1.0f && !c.comb) {
        r.kind = CodeKind::CTCSS;
        r.ctcssHz = c.nearest; r.ctcssMeasuredHz = c.hz; r.ctcssNumber = ctcssPrivacyCode(c.nearest);
        r.prominenceDb = c.prominence; r.dominanceDb = c.dominance;
        r.confidence = c.prominence;
        return r;
    }
    return r;
}

// -------------------------------------------------------- ChannelDetector ---

ChannelDetector::ChannelDetector(double inputRate, int fsd, double windowSeconds,
                                 double minDecodeSeconds, double runInterval)
    : _inputRate(inputRate), _fsd(fsd), _windowSeconds(windowSeconds),
      _minDecodeSeconds(minDecodeSeconds), _runInterval(runInterval), _det(fsd) {
    _decim = (int)std::lround(inputRate / fsd);
    assert(std::fabs(inputRate / fsd - _decim) < 1e-6 && "inputRate must be an integer multiple of fsd");
    _decimator.init(sa::lowPassTaps(300.0, 200.0, inputRate), _decim);
    _dcBlock.init(2.0, fsd);
    _lp150.init(sa::lowPassTaps(150.0, 60.0, fsd), 1);
    _dcMean.init(0.5, fsd);
    size_t cap = (size_t)std::ceil(windowSeconds * fsd) + fsd;   // a little slack
    _ring300.init(cap);
    _ring150.init(cap);
    _win300.resize(cap);
    _win150.resize(cap);
}

ChannelDetector::~ChannelDetector() = default;

void ChannelDetector::reset() {
    std::lock_guard<std::mutex> lck(_mtx);
    _decimator.reset(); _dcBlock.reset(); _lp150.reset(); _dcMean.reset();
    _ring300.clear(); _ring150.clear();
    _state = ChannelCodeState{};
    _lastRun = DetectResult{};
    _carrier = false; _nextRun = 0.0;
}

void ChannelDetector::pushAudio(const float* x, int n) {
    _tmp300.resize(n / _decim + 2);
    _tmp150.resize(n / _decim + 2);
    int m = _decimator.process(x, n, _tmp300.data());
    for (int i = 0; i < m; i++) { _dcMean.push(_tmp300[i]); }
    _dcBlock.process(_tmp300.data(), m);
    int m2 = _lp150.process(_tmp300.data(), m, _tmp150.data());
    std::lock_guard<std::mutex> lck(_mtx);
    int skip = std::min(_settleRemaining, std::min(m, m2));
    _settleRemaining -= skip;
    _ring300.push(_tmp300.data() + skip, m - skip);
    _ring150.push(_tmp150.data() + skip, m2 - skip);
}

float ChannelDetector::frequencyErrorMean() const { return _dcMean.value(); }

double ChannelDetector::bufferedSeconds() const {
    std::lock_guard<std::mutex> lck(_mtx);
    return (double)std::min(_ring300.size(), _ring150.size()) / _fsd;
}

void ChannelDetector::setCarrier(bool present, double tNow) {
    std::lock_guard<std::mutex> lck(_mtx);
    if (present == _carrier) { return; }
    _carrier = present;
    if (present) {
        _carrierSince = tNow;
        _ring300.clear(); _ring150.clear();   // start the key-down with fresh audio
        _settleRemaining = (int)std::lround(_settleSeconds * _fsd);
        _lastRun = DetectResult{};
        _state.agreeCount = 0;
        _state.mixed = false;
        _state.confirmedThisKeydown = false;
        _nextRun = tNow + _minDecodeSeconds;
    } else {
        _carrierLost = tNow;
        // Decide on the next tick (monitor thread): one last analysis of all
        // buffered audio, then Confirmed -> Held / Tentative -> None.
        _finalRunPending = true;
    }
}

void ChannelDetector::applyCarrierLoss(double tNow) {
    if (_state.stage == CodeStage::Confirmed) {
        _state.stage = CodeStage::Held;
        _state.stageSince = tNow;
    } else if (_state.stage == CodeStage::Tentative) {
        _state.stage = CodeStage::None;
    }
    _lastRun = DetectResult{};
    _state.agreeCount = 0;
    _finalRunPending = false;
}

ChannelCodeState ChannelDetector::state() const {
    std::lock_guard<std::mutex> lck(_mtx);
    return _state;
}

bool ChannelDetector::tick(double tNow) {
    enum class Action { Nothing, Run, FinalRun, Expire };
    Action action = Action::Nothing;
    CodeStage stageBefore; std::string labelBefore;
    {
        std::lock_guard<std::mutex> lck(_mtx);
        stageBefore = _state.stage;
        labelBefore = _state.result.label();
        const double buffered = (double)std::min(_ring300.size(), _ring150.size()) / _fsd;
        if (!_carrier) {
            if (_finalRunPending) {
                action = (buffered >= _minDecodeSeconds && _state.stage != CodeStage::Confirmed) ? Action::FinalRun : Action::Expire;
            } else if (_state.stage == CodeStage::Held && tNow - _state.stageSince > _holdSeconds) {
                _state.stage = CodeStage::None;
                return true;
            } else {
                return false;
            }
        } else {
            if (tNow < _nextRun || buffered < _minDecodeSeconds) { return false; }
            _nextRun = tNow + _runInterval;
            action = Action::Run;
        }
    }
    if (action == Action::Run) { runAnalysis(tNow, false); }
    if (action == Action::FinalRun) { runAnalysis(tNow, true); }
    std::lock_guard<std::mutex> lck(_mtx);
    if (action == Action::FinalRun || action == Action::Expire) { applyCarrierLoss(tNow); }
    return _state.stage != stageBefore || _state.result.label() != labelBefore;
}

void ChannelDetector::runAnalysis(double tNow, bool finalRun) {
    int n;
    {
        std::lock_guard<std::mutex> lck(_mtx);
        size_t avail = std::min(_ring300.size(), _ring150.size());
        n = (int)std::min(avail, (size_t)std::llround(_windowSeconds * _fsd));
        _ring300.readLast(n, _win300.data());
        _ring150.readLast(n, _win150.data());
    }
    SubAudibleDetector::DcsRaw draw; SubAudibleDetector::CtcssRaw craw;
    DetectResult res = _det.analyze(_win300.data(), _win150.data(), n, _invert, &draw, &craw);

    std::lock_guard<std::mutex> lck(_mtx);
    _state.lastRun = tNow;
    _state.lastWindowSamples = n;
    _state.lastDcs = draw;
    _state.lastCtcss = craw;
    const bool hasCode = res.kind != CodeKind::None;
    const bool agreesWithLast = hasCode && _lastRun.kind != CodeKind::None && _lastRun.sameCodeAs(res);

    if (finalRun) {
        // One shot at the end of a short key-down: accept agreement with what we
        // already saw, or a single strong result.
        bool strong = hasCode && ((res.kind == CodeKind::DCS && res.corr >= DCS_FINAL_MIN_CORR) ||
                                  (res.kind == CodeKind::CTCSS && res.prominenceDb >= CTCSS_FINAL_MIN_PROM_DB));
        bool agreesTentative = hasCode && _state.stage == CodeStage::Tentative && _state.result.sameCodeAs(res);
        if (agreesTentative || agreesWithLast || strong) {
            _state.stage = CodeStage::Confirmed; _state.stageSince = tNow; _state.agreeCount = 2; _state.confirmedThisKeydown = true;
            _state.result = res;
        }
        _lastRun = res;
        return;
    }

    switch (_state.stage) {
    case CodeStage::Confirmed:
        if (hasCode && _state.result.sameCodeAs(res)) {
            _state.agreeCount++;
            float conf = std::max(_state.result.confidence, res.confidence);
            _state.result = res; _state.result.confidence = conf;
        } else if (hasCode && agreesWithLast) {
            // a different code twice in a row: switch, flag as mixed
            _state.result = res; _state.stageSince = tNow; _state.agreeCount = 2; _state.mixed = true;
        } else if (hasCode) {
            _state.mixed = true;   // seen once; wait for a second agreeing run
        }
        break;
    case CodeStage::Tentative:
        if (hasCode && _state.result.sameCodeAs(res)) {
            _state.stage = CodeStage::Confirmed; _state.stageSince = tNow; _state.agreeCount = 2; _state.confirmedThisKeydown = true;
            float conf = std::max(_state.result.confidence, res.confidence);
            _state.result = res; _state.result.confidence = conf;
        } else {
            // disagreement or nothing: back to none, never straight to another tentative
            _state.stage = CodeStage::None; _state.agreeCount = 0;
        }
        break;
    case CodeStage::None:
    case CodeStage::Held:
        if (hasCode && agreesWithLast) {
            _state.stage = CodeStage::Confirmed; _state.stageSince = tNow; _state.agreeCount = 2; _state.confirmedThisKeydown = true;
            _state.result = res;
        } else if (hasCode) {
            _state.stage = CodeStage::Tentative; _state.stageSince = tNow; _state.agreeCount = 1;
            _state.result = res;
        }
        break;
    }
    _lastRun = res;
}

} // namespace frsmon
