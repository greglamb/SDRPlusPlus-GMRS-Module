#pragma once
// ChannelBank: one SDR++ sink block on the wide VFO that demodulates every
// FRS/GMRS channel of the block in-line (one thread), measures IF power per
// channel, decides carrier presence and feeds each channel's detector.
//
// The per-channel DSP is written directly on VOLK rather than with SDR++
// Processor blocks: each of those carries ~16 MB of stream buffers, and we
// need 45 of them. These cost a few kilobytes each.
#include <dsp/sink.h>
#include <dsp/taps/low_pass.h>
#include <dsp/math/hz_to_rads.h>
#include <dsp/math/normalize_phase.h>
#include <utils/flog.h>
#include <volk/volk.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "detect/detector.h"

namespace frsmon {

inline double steadySeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

struct BankConfig {
    double inputRate = 250000.0;   // VFO output rate
    int decimation = 10;           // -> 25 kS/s per channel
    double channelCutoffHz = 5500.0;
    double channelTransitionHz = 4000.0;
    double deviationHz = 2500.0;   // NFM (12.5 kHz channels)
    double frameSeconds = 0.05;    // power measurement frame
    float carrierThresholdDb = 8.0f;
    float hysteresisDb = 2.0f;
    int busyFrames = 2;            // 100 ms above threshold -> busy
    int idleFrames = 6;            // 300 ms below -> idle
    float floorRiseDbPerSec = 0.5f;
    bool afc = true;
    double afcMaxHz = 3000.0;
    int subAudibleRate = 2500;     // 25000 / 10
};

// ------------------------------------------------------------ light DSP ---

class Rotator {
public:
    void setOffset(double hz, double sampleRate) {
        double w = dsp::math::hzToRads(hz, sampleRate);
        _delta = lv_cmake((float)std::cos(w), (float)std::sin(w));
    }
    void reset() { _phase = lv_cmake(1.0f, 0.0f); }
    void process(const dsp::complex_t* in, dsp::complex_t* out, int n) {
#if VOLK_VERSION >= 030100
        volk_32fc_s32fc_x2_rotator2_32fc((lv_32fc_t*)out, (const lv_32fc_t*)in, &_delta, &_phase, n);
#else
        volk_32fc_s32fc_x2_rotator_32fc((lv_32fc_t*)out, (const lv_32fc_t*)in, _delta, &_phase, n);
#endif
    }
private:
    lv_32fc_t _phase = lv_cmake(1.0f, 0.0f);
    lv_32fc_t _delta = lv_cmake(1.0f, 0.0f);
};

class ComplexDecimFIR {
public:
    void init(const std::vector<float>& taps, int decim) {
        _taps = taps; _decim = decim;
        _hist.assign(taps.size() - 1, dsp::complex_t{0, 0});
        _phase = 0;
    }
    int process(const dsp::complex_t* in, int count, dsp::complex_t* out) {
        const int nt = (int)_taps.size();
        _work.resize(_hist.size() + count);
        std::memcpy(_work.data(), _hist.data(), _hist.size() * sizeof(dsp::complex_t));
        std::memcpy(_work.data() + _hist.size(), in, count * sizeof(dsp::complex_t));
        int produced = 0, i = _phase;
        for (; i + nt <= (int)_work.size(); i += _decim) {
            volk_32fc_32f_dot_prod_32fc((lv_32fc_t*)&out[produced++], (const lv_32fc_t*)&_work[i], _taps.data(), nt);
        }
        const int consumed = (int)_work.size() - (nt - 1);
        _phase = i - consumed;
        std::memcpy(_hist.data(), _work.data() + consumed, (nt - 1) * sizeof(dsp::complex_t));
        return produced;
    }
private:
    std::vector<float> _taps;
    std::vector<dsp::complex_t> _hist, _work;
    int _decim = 1, _phase = 0;
};

class QuadDemod {
public:
    void init(double deviationHz, double sampleRate) { _invDev = (float)(1.0 / dsp::math::hzToRads(deviationHz, sampleRate)); _phase = 0; }
    void process(const dsp::complex_t* in, float* out, int n) {
        for (int i = 0; i < n; i++) {
            float p = std::atan2(in[i].im, in[i].re);
            out[i] = dsp::math::normalizePhase(p - _phase) * _invDev;
            _phase = p;
        }
    }
private:
    float _invDev = 1.0f, _phase = 0.0f;
};

// -------------------------------------------------------------- the bank ---

class ChannelBank : public dsp::Sink<dsp::complex_t> {
    using base_type = dsp::Sink<dsp::complex_t>;
public:
    struct Channel {
        int number = 0;
        double freqHz = 0;
        double offsetHz = 0;          // relative to the VFO centre
        std::atomic<bool> active{false};
        Rotator rot;
        ComplexDecimFIR fir;
        QuadDemod demod;
        std::unique_ptr<ChannelDetector> det;
        std::vector<dsp::complex_t> bufA, bufB;
        std::vector<float> audio;
        double frameAcc = 0; int frameCount = 0;
        std::atomic<float> levelDb{-120.0f};
        std::atomic<float> floorDb{-120.0f};
        bool floorInit = false;
        int aboveCount = 0, belowCount = 0;
        std::atomic<bool> busy{false};
        std::atomic<double> busySince{0.0};
        std::atomic<double> lastBusy{0.0};
        std::atomic<float> peakDb{-120.0f};
        double afcHz = 0; int afcFrames = 0;
        std::atomic<float> afcHzOut{0.0f};
    };

    ChannelBank() {}
    ~ChannelBank() { if (base_type::_block_init) { base_type::stop(); } }

    void init(dsp::stream<dsp::complex_t>* in, const BankConfig& cfg) {
        _cfg = cfg;
        dsp::tap<float> t = dsp::taps::lowPass(cfg.channelCutoffHz, cfg.channelTransitionHz, cfg.inputRate);
        _taps.assign(t.taps, t.taps + t.size);
        dsp::taps::free(t);
        _frameSamples = (int)std::lround(cfg.frameSeconds * cfg.inputRate / cfg.decimation);
        base_type::init(in);
    }

    using ChannelSet = std::vector<std::unique_ptr<Channel>>;

    // Build a channel set (not yet placed on the VFO, nothing is fed to it).
    ChannelSet buildChannels(const std::vector<std::pair<int, double>>& chans) {
        ChannelSet set;
        for (auto& [num, freq] : chans) {
            auto ch = std::make_unique<Channel>();
            ch->number = num;
            ch->freqHz = freq;
            ch->fir.init(_taps, _cfg.decimation);
            ch->demod.init(_cfg.deviationHz, _cfg.inputRate / _cfg.decimation);
            ch->det = std::make_unique<ChannelDetector>(_cfg.inputRate / _cfg.decimation, _cfg.subAudibleRate);
            set.push_back(std::move(ch));
        }
        return set;
    }

    // Make `set` the live set and hand back the previous one (with its
    // carriers dropped) so the caller can park it and swap it back in later
    // with its held codes and last-heard times intact. vfoCenterHz is where
    // the VFO actually sits; channels outside the usable span are inactive.
    ChannelSet setChannels(ChannelSet&& set, double vfoCenterHz, double usableSpanHz) {
        std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
        base_type::tempStop();
        const double now = steadySeconds();
        ChannelSet old = std::move(_channels);
        _channels = std::move(set);
        for (auto& ch : old) {
            if (ch->busy.load()) { dropCarrier(*ch, now); }
            ch->active.store(false);
        }
        for (auto& ch : _channels) {
            ch->det->setInvert(_invert);
            ch->det->setHoldSeconds(_holdSeconds);
            ch->aboveCount = 0; ch->belowCount = 0;
            ch->frameAcc = 0; ch->frameCount = 0;
            place(*ch, vfoCenterHz, usableSpanHz, now);
        }
        base_type::tempStart();
        return old;
    }

    // Convenience: build + make live, discarding the previous set.
    void configureChannels(const std::vector<std::pair<int, double>>& chans, double vfoCenterHz, double usableSpanHz) {
        setChannels(buildChannels(chans), vfoCenterHz, usableSpanHz);
    }

    void setVfoCenter(double vfoCenterHz, double usableSpanHz) {
        std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
        const double now = steadySeconds();
        for (auto& ch : _channels) { place(*ch, vfoCenterHz, usableSpanHz, now); }
    }

    void setCarrierThreshold(float db) { _cfg.carrierThresholdDb = db; }
    void setAfcEnabled(bool on) { _cfg.afc = on; }
    void setInvert(bool inv) { _invert = inv; for (auto& ch : _channels) { ch->det->setInvert(inv); } }
    void setHoldSeconds(double s) { _holdSeconds = s; for (auto& ch : _channels) { ch->det->setHoldSeconds(s); } }
    const BankConfig& config() const { return _cfg; }

    // Hold this while iterating channels() from another thread; setChannels()
    // takes it (via the block's control mutex) before replacing the set.
    std::recursive_mutex& mutex() { return base_type::ctrlMtx; }
    ChannelSet& channels() { return _channels; }

    int run() {
        int count = base_type::_in->read();
        if (count < 0) { return -1; }
        const dsp::complex_t* in = base_type::_in->readBuf;
        const double now = steadySeconds();
        for (auto& chp : _channels) {
            Channel& ch = *chp;
            if (!ch.active.load()) { continue; }
            if ((int)ch.bufA.size() < count) {
                ch.bufA.resize(count);
                ch.bufB.resize(count / _cfg.decimation + 8);
                ch.audio.resize(count / _cfg.decimation + 8);
            }
            ch.rot.process(in, ch.bufA.data(), count);
            int m = ch.fir.process(ch.bufA.data(), count, ch.bufB.data());
            if (m <= 0) { continue; }
            for (int i = 0; i < m; i++) {
                const auto& s = ch.bufB[i];
                ch.frameAcc += (double)s.re * s.re + (double)s.im * s.im;
                if (++ch.frameCount >= _frameSamples) { endFrame(ch, now); }
            }
            ch.demod.process(ch.bufB.data(), ch.audio.data(), m);
            ch.det->pushAudio(ch.audio.data(), m);
        }
        base_type::_in->flush();
        return count;
    }

private:
    void place(Channel& ch, double vfoCenterHz, double usableSpanHz, double now) {
        ch.offsetHz = ch.freqHz - vfoCenterHz;
        bool act = std::fabs(ch.offsetHz) <= usableSpanHz / 2.0;
        if (!act && ch.active.load()) { dropCarrier(ch, now); }
        ch.active.store(act);
        ch.afcHz = 0; ch.afcHzOut.store(0.0f);
        ch.rot.setOffset(-ch.offsetHz, _cfg.inputRate);
    }

    void endFrame(Channel& ch, double now) {
        float p = 10.0f * std::log10((float)(ch.frameAcc / ch.frameCount) + 1e-20f);
        ch.frameAcc = 0; ch.frameCount = 0;
        ch.levelDb.store(p);
        if (!ch.floorInit) { ch.floorDb.store(p); ch.floorInit = true; }
        float fl = ch.floorDb.load();
        if (p < fl) { fl = p; } else { fl += _cfg.floorRiseDbPerSec * (float)_cfg.frameSeconds; }
        ch.floorDb.store(fl);
        const bool wasBusy = ch.busy.load();
        if (p > fl + _cfg.carrierThresholdDb) { ch.aboveCount++; ch.belowCount = 0; }
        else if (p < fl + _cfg.carrierThresholdDb - _cfg.hysteresisDb) { ch.belowCount++; ch.aboveCount = 0; }
        if (!wasBusy && ch.aboveCount >= _cfg.busyFrames) {
            ch.busy.store(true); ch.busySince.store(now); ch.peakDb.store(p);
            ch.afcHz = 0; ch.afcFrames = 0;
            ch.det->setCarrier(true, now);
        } else if (wasBusy && ch.belowCount >= _cfg.idleFrames) {
            dropCarrier(ch, now);
        }
        if (ch.busy.load()) {
            ch.lastBusy.store(now);
            if (p > ch.peakDb.load()) { ch.peakDb.store(p); }
            if (_cfg.afc && ++ch.afcFrames >= 20) {       // once a second
                ch.afcFrames = 0;
                double errHz = ch.det->frequencyErrorMean() * _cfg.deviationHz;
                double next = std::max(-_cfg.afcMaxHz, std::min(_cfg.afcMaxHz, ch.afcHz + 0.7 * errHz));
                if (std::fabs(next - ch.afcHz) > 20.0) {
                    ch.afcHz = next;
                    ch.rot.setOffset(-(ch.offsetHz + ch.afcHz), _cfg.inputRate);
                }
                ch.afcHzOut.store((float)ch.afcHz);
            }
        }
    }

    void dropCarrier(Channel& ch, double now) {
        ch.busy.store(false);
        ch.aboveCount = 0; ch.belowCount = 0;
        ch.det->setCarrier(false, now);
        if (ch.afcHz != 0) { ch.afcHz = 0; ch.afcHzOut.store(0.0f); ch.rot.setOffset(-ch.offsetHz, _cfg.inputRate); }
    }

    BankConfig _cfg;
    std::vector<float> _taps;
    int _frameSamples = 1250;
    bool _invert = false;
    double _holdSeconds = 600.0;
    std::vector<std::unique_ptr<Channel>> _channels;
};

} // namespace frsmon
