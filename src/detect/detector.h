#pragma once
// CTCSS / DCS detection on demodulated NFM audio.
//
// Two layers:
//   SubAudibleDetector  - stateless analysis of one window (the maths validated
//                          in dcsdecode.py: fold-and-correlate DCS, percentile
//                          spectrum CTCSS). FFTW3f inside.
//   ChannelDetector     - streaming wrapper for one radio channel: front end
//                          (decimation to the sub-audible rate), ring buffers,
//                          periodic analysis while a carrier is present, and the
//                          tentative -> confirmed -> held state machine the board
//                          displays.
//
// No SDR++ dependency; the same code is used by the module and by the parity
// test binary.
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "dsp_util.h"

namespace frsmon {

// ---------------------------------------------------------- thresholds ---
// Set from measurements (see design doc): real transmissions, even with a child
// shouting over the code, score corr>=0.76 / margin>=0.19 / pf>=0.14; noise and
// voice-only carriers never exceeded 0.58 / 0.08 / 0.09.
constexpr float DCS_MIN_CORR = 0.75f;
constexpr float DCS_MIN_MARGIN = 0.15f;
constexpr float DCS_MIN_PF = 0.10f;
// Steady tone under a voice 12 dB louder: prominence >= 23 dB, dominance >= 13 dB;
// voice-only / noise windows never exceeded 14 / 2.5.
constexpr float CTCSS_MIN_PROM_DB = 18.0f;
constexpr float CTCSS_MIN_DOM_DB = 8.0f;
// A key-down that ends before a second confirming run gets one last analysis of
// everything buffered; a single result is accepted as confirmed above these.
constexpr float DCS_FINAL_MIN_CORR = 0.80f;
constexpr float CTCSS_FINAL_MIN_PROM_DB = 22.0f;

constexpr double DCS_NOMINAL_BPS = 134.4;
constexpr int DCS_SPB = 10;               // samples per bit in the folded domain
constexpr int DCS_WL = 23 * DCS_SPB;      // samples per word

enum class CodeKind { None, CTCSS, DCS };

struct DetectResult {
    CodeKind kind = CodeKind::None;
    // CTCSS
    float ctcssHz = 0.0f;        // nearest standard tone
    float ctcssMeasuredHz = 0.0f;
    int ctcssNumber = 0;         // 1..38, 0 if not in the consumer list
    float prominenceDb = 0.0f;
    float dominanceDb = 0.0f;
    // DCS
    int dcsCode = 0;             // octal value, as the demodulator polarity reads it
    int dcsAltCode = 0;          // the inverse-polarity reading
    int dcsNumber = 0;           // 39..121, 0 if not in the consumer list
    float corr = 0.0f;           // folded correlation, 0..1
    float margin = 0.0f;         // gap to best different code
    float periodicFraction = 0.0f;
    float bitrate = 0.0f;
    // common
    float confidence = 0.0f;     // DCS: corr; CTCSS: prominence dB

    bool sameCodeAs(const DetectResult& o) const {
        if (kind != o.kind) { return false; }
        if (kind == CodeKind::CTCSS) { return ctcssHz == o.ctcssHz; }
        if (kind == CodeKind::DCS) { return dcsCode == o.dcsCode; }
        return true;
    }
    std::string label() const;   // "D261N" / "141.3 Hz" / ""
    std::string privacyLabel() const; // "privacy 75" / ""
};

// ------------------------------------------------------- window analysis ---
class SubAudibleDetector {
public:
    // fsd: sub-audible sample rate the windows arrive at (2400 or 2500 work; any 2000..4000).
    explicit SubAudibleDetector(int fsd);
    ~SubAudibleDetector();
    SubAudibleDetector(const SubAudibleDetector&) = delete;
    SubAudibleDetector& operator=(const SubAudibleDetector&) = delete;

    int sampleRate() const { return _fsd; }

    // y300: window band-limited to ~2-300 Hz; y150: the same window band-limited
    // to ~2-150 Hz (for DCS). Both length n (>= 1 s of samples). invert flips the
    // reported DCS polarity.
    // Exposed for tests/tuning/UI
    struct DcsRaw { int code = 0; float corr = 0; bool inverted = false; float margin = 0; float pf = 0; float rate = 0; int words = 0; };
    struct CtcssRaw { float hz = 0; float prominence = 0; float dominance = 0; float nearest = 0; float err = 99; bool comb = false; };
    DetectResult analyze(const float* y300, const float* y150, int n, bool invert,
                         DcsRaw* dcsOut = nullptr, CtcssRaw* ctcssOut = nullptr);
    DcsRaw detectDcs(const float* y150, int n);
    CtcssRaw detectCtcss(const float* y300, int n);

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
    int _fsd;
};

// ------------------------------------------------------------- streaming ---
enum class CodeStage { None, Tentative, Confirmed, Held };

struct ChannelCodeState {
    CodeStage stage = CodeStage::None;
    DetectResult result;        // valid when stage != None
    double stageSince = 0.0;    // seconds (caller's clock)
    double lastRun = 0.0;
    int agreeCount = 0;
    bool mixed = false;         // a different code also showed up during this key-down
    bool confirmedThisKeydown = false;  // false while a Held code is only carried over for display
    // raw numbers from the most recent analysis run (diagnostics / hover)
    int lastWindowSamples = 0;
    SubAudibleDetector::DcsRaw lastDcs;
    SubAudibleDetector::CtcssRaw lastCtcss;
};

class ChannelDetector {
public:
    // inputRate: rate of the discriminator audio pushed in (e.g. 25000 from the
    // module, 48000 from a WAV). fsd: internal sub-audible rate; inputRate must be
    // an integer multiple of it.
    ChannelDetector(double inputRate, int fsd, double windowSeconds = 3.0,
                    double minDecodeSeconds = 1.0, double runInterval = 0.25);
    ~ChannelDetector();

    // DSP-thread side ------------------------------------------------------
    void pushAudio(const float* x, int n);          // raw discriminator samples
    void setCarrier(bool present, double tNow);     // from the channel's power detector
    float frequencyErrorMean() const;               // mean discriminator level (AFC input)

    // Monitor-thread side --------------------------------------------------
    // Call periodically (every ~50-250 ms). Runs analysis when due. Returns true
    // if the displayed state changed.
    bool tick(double tNow);

    ChannelCodeState state() const;                 // thread-safe copy
    bool carrier() const { return _carrier; }
    double bufferedSeconds() const;

    void setInvert(bool inv) { _invert = inv; }
    void setHoldSeconds(double s) { _holdSeconds = s; }
    // audio in the first settleSeconds after key-down is not analysed (filter
    // transients, squelch tail, radio TX ramp). Default 0.15 s.
    void setSettleSeconds(double s) { _settleSeconds = s; }
    void reset();

private:
    void runAnalysis(double tNow, bool finalRun);
    void applyCarrierLoss(double tNow);   // Confirmed -> Held, Tentative -> None

    double _inputRate;
    int _fsd;
    int _decim;
    double _windowSeconds, _minDecodeSeconds, _runInterval, _holdSeconds = 600.0, _settleSeconds = 0.15;
    int _settleRemaining = 0;   // samples (at fsd) still to discard after key-down
    bool _invert = false;

    sa::DecimatingFIR _decimator;    // input -> fsd, cutoff 300 Hz
    sa::DCBlocker _dcBlock;           // 2 Hz
    sa::DecimatingFIR _lp150;        // fsd -> fsd, cutoff 150 Hz (decim 1)
    sa::SlowMean _dcMean;             // discriminator DC (frequency error)
    std::vector<float> _tmp300, _tmp150;

    mutable std::mutex _mtx;
    sa::RingBuffer _ring300, _ring150;
    std::vector<float> _win300, _win150;

    SubAudibleDetector _det;
    ChannelCodeState _state;
    DetectResult _lastRun;           // previous run's result during this key-down
    bool _carrier = false;
    bool _finalRunPending = false;
    double _carrierSince = 0.0;
    double _carrierLost = 0.0;
    double _nextRun = 0.0;
};

std::string dcsOctal(int code);      // "261"

} // namespace frsmon
