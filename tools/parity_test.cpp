// parity_test: run the C++ ChannelDetector over a WAV file, streaming it in
// chunks, with carrier spans supplied from a text file ("t0 t1" per line).
// Prints one JSON line per span with the final code state and timings.
//
//   parity_test <file.wav> <spans.txt> [--chunk N] [--fsd 2400] [--invert]
//
// Used by tests/run_parity.py to compare against dcsdecode.py.
#include "../src/detect/detector.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

struct Wav { int rate = 0; int channels = 0; std::vector<float> mono; };

bool readWav(const char* path, Wav& w) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { return false; }
    char id[4]; uint32_t sz;
    f.read(id, 4); f.read((char*)&sz, 4); f.read(id, 4);
    if (std::strncmp(id, "WAVE", 4) != 0) { return false; }
    uint16_t fmt = 0, ch = 0, bits = 0; uint32_t rate = 0;
    std::vector<char> data;
    while (f.read(id, 4) && f.read((char*)&sz, 4)) {
        if (std::strncmp(id, "fmt ", 4) == 0) {
            std::vector<char> b(sz); f.read(b.data(), sz);
            fmt = *(uint16_t*)&b[0]; ch = *(uint16_t*)&b[2]; rate = *(uint32_t*)&b[4]; bits = *(uint16_t*)&b[14];
            if (fmt == 0xFFFE && sz >= 26) { fmt = *(uint16_t*)&b[24]; }
        } else if (std::strncmp(id, "data", 4) == 0) {
            data.resize(sz); f.read(data.data(), sz); break;
        } else {
            f.seekg(sz + (sz & 1), std::ios::cur);
        }
    }
    if (!rate || !ch) { return false; }
    w.rate = rate; w.channels = ch;
    size_t frames = 0;
    if (fmt == 1 && bits == 16) {
        frames = data.size() / (2 * ch);
        w.mono.resize(frames);
        const int16_t* s = (const int16_t*)data.data();
        for (size_t i = 0; i < frames; i++) { double acc = 0; for (int c = 0; c < ch; c++) { acc += s[i * ch + c] / 32768.0; } w.mono[i] = (float)(acc / ch); }
    } else if (fmt == 1 && bits == 32) {
        frames = data.size() / (4 * ch);
        w.mono.resize(frames);
        const int32_t* s = (const int32_t*)data.data();
        for (size_t i = 0; i < frames; i++) { double acc = 0; for (int c = 0; c < ch; c++) { acc += s[i * ch + c] / 2147483648.0; } w.mono[i] = (float)(acc / ch); }
    } else if (fmt == 3 && bits == 32) {
        frames = data.size() / (4 * ch);
        w.mono.resize(frames);
        const float* s = (const float*)data.data();
        for (size_t i = 0; i < frames; i++) { double acc = 0; for (int c = 0; c < ch; c++) { acc += s[i * ch + c]; } w.mono[i] = (float)(acc / ch); }
    } else {
        std::fprintf(stderr, "unsupported wav format %d/%d bits\n", fmt, bits);
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: parity_test file.wav spans.txt [--chunk N] [--fsd N] [--invert]\n"); return 2; }
    int chunk = 1024, fsd = 2400; bool invert = false, debug = false;
    for (int i = 3; i < argc; i++) {
        if (!std::strcmp(argv[i], "--debug")) { debug = true; }
        else if (!std::strcmp(argv[i], "--chunk") && i + 1 < argc) { chunk = std::atoi(argv[++i]); }
        else if (!std::strcmp(argv[i], "--fsd") && i + 1 < argc) { fsd = std::atoi(argv[++i]); }
        else if (!std::strcmp(argv[i], "--invert")) { invert = true; }
    }
    Wav w;
    if (!readWav(argv[1], w)) { std::fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    if (w.rate % fsd != 0) { std::fprintf(stderr, "sample rate %d not a multiple of fsd %d\n", w.rate, fsd); return 1; }
    std::vector<std::pair<double, double>> spans;
    { std::ifstream s(argv[2]); double a, b; while (s >> a >> b) { spans.emplace_back(a, b); } }

    frsmon::ChannelDetector det((double)w.rate, fsd);
    det.setInvert(invert);
    det.setHoldSeconds(1e9);

    struct SpanOut { double t0, t1; std::string first, finalLabel; frsmon::CodeStage stage; double tTent = -1, tConf = -1; bool mixed = false; float conf = 0; std::string alt; int privacy = 0; };
    std::vector<SpanOut> outs;
    size_t si = 0; bool inSpan = false; SpanOut cur{};
    const double dt = 1.0 / w.rate;
    const double tickEvery = 0.05; double nextTick = 0;
    frsmon::CodeStage lastStage = frsmon::CodeStage::None;
    std::string lastLabel;

    for (size_t pos = 0; pos < w.mono.size(); pos += chunk) {
        size_t n = std::min((size_t)chunk, w.mono.size() - pos);
        double tStart = pos * dt, tEnd = (pos + n) * dt;
        // carrier edges inside this chunk (coarse: applied at chunk boundaries)
        if (!inSpan && si < spans.size() && spans[si].first <= tEnd) {
            det.setCarrier(true, spans[si].first);
            inSpan = true; cur = SpanOut{}; cur.t0 = spans[si].first; cur.t1 = spans[si].second;
            lastStage = frsmon::CodeStage::None; lastLabel.clear();
        }
        det.pushAudio(w.mono.data() + pos, (int)n);
        while (nextTick <= tEnd) {
            bool changed = det.tick(nextTick);
            auto st = det.state();
            if (debug && inSpan && st.lastRun == nextTick) {
                std::fprintf(stderr, "t=%.2f n=%d dcs %03o corr %.3f margin %.3f pf %.3f rate %.1f | ctcss %.2f prom %.1f dom %.1f comb %d | stage %d%s\n",
                             nextTick, st.lastWindowSamples, st.lastDcs.code, st.lastDcs.corr, st.lastDcs.margin, st.lastDcs.pf, st.lastDcs.rate,
                             st.lastCtcss.hz, st.lastCtcss.prominence, st.lastCtcss.dominance, (int)st.lastCtcss.comb, (int)st.stage, changed ? " *" : "");
            }
            if (inSpan) {
                if (st.stage == frsmon::CodeStage::Tentative && cur.tTent < 0) { cur.tTent = nextTick - cur.t0; cur.first = st.result.label(); }
                if (st.stage == frsmon::CodeStage::Confirmed && cur.tConf < 0) { cur.tConf = nextTick - cur.t0; }
                if (st.stage == frsmon::CodeStage::Confirmed) {
                    cur.finalLabel = st.result.label(); cur.conf = st.result.confidence; cur.mixed = st.mixed;
                    cur.privacy = st.result.kind == frsmon::CodeKind::DCS ? st.result.dcsNumber : st.result.ctcssNumber;
                    cur.alt = st.result.kind == frsmon::CodeKind::DCS ? frsmon::dcsOctal(st.result.dcsAltCode) : "";
                }
                cur.stage = st.stage;
            }
            nextTick += tickEvery;
        }
        auto closeSpan = [&](double tClose) {
            det.setCarrier(false, tClose);
            det.tick(tClose + 0.001);               // the end-of-transmission pass
            auto st = det.state();
            if (debug) {
                std::fprintf(stderr, "final t=%.2f n=%d dcs %03o corr %.3f margin %.3f pf %.3f | ctcss %.2f prom %.1f dom %.1f | stage %d thisKeydown %d\n",
                             tClose, st.lastWindowSamples, st.lastDcs.code, st.lastDcs.corr, st.lastDcs.margin, st.lastDcs.pf,
                             st.lastCtcss.hz, st.lastCtcss.prominence, st.lastCtcss.dominance, (int)st.stage, (int)st.confirmedThisKeydown);
            }
            if (st.confirmedThisKeydown && st.result.kind != frsmon::CodeKind::None) {
                if (cur.finalLabel.empty()) { cur.tConf = tClose - cur.t0; }
                cur.finalLabel = st.result.label(); cur.conf = st.result.confidence; cur.mixed = st.mixed;
                cur.privacy = st.result.kind == frsmon::CodeKind::DCS ? st.result.dcsNumber : st.result.ctcssNumber;
                cur.alt = st.result.kind == frsmon::CodeKind::DCS ? frsmon::dcsOctal(st.result.dcsAltCode) : "";
            }
            inSpan = false; outs.push_back(cur);
        };
        if (inSpan && spans[si].second <= tEnd) { closeSpan(spans[si].second); si++; }
    }
    if (inSpan) {
        det.setCarrier(false, w.mono.size() * dt);
        det.tick(w.mono.size() * dt + 0.001);
        auto st = det.state();
        if (st.confirmedThisKeydown && st.result.kind != frsmon::CodeKind::None) {
            cur.finalLabel = st.result.label(); cur.conf = st.result.confidence;
            cur.privacy = st.result.kind == frsmon::CodeKind::DCS ? st.result.dcsNumber : st.result.ctcssNumber;
        }
        outs.push_back(cur);
    }

    for (auto& o : outs) {
        std::printf("{\"t0\":%.2f,\"t1\":%.2f,\"label\":\"%s\",\"privacy\":%d,\"alt\":\"%s\",\"confidence\":%.3f,"
                    "\"first_tentative\":\"%s\",\"t_tentative\":%.2f,\"t_confirmed\":%.2f,\"mixed\":%s}\n",
                    o.t0, o.t1, o.finalLabel.c_str(), o.privacy, o.alt.c_str(), o.conf,
                    o.first.c_str(), o.tTent, o.tConf, o.mixed ? "true" : "false");
    }
    return 0;
}
