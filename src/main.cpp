// FRS/GMRS Monitor — SDR++ module
//
// Watches every FRS/GMRS channel of a block at once on its own wide VFO and
// shows, live, which channels are busy and which CTCSS tone / DCS code each
// transmission carries (with the consumer privacy-code number). Logs each
// key-down, names channel+code pairs via presets, has one-click channel
// shortcuts (all 22 channels + the 8 repeater inputs) that tune the stock
// Radio module, and a scanner that parks the Radio on whatever is talking.
#include <imgui.h>
#include <config.h>
#include <core.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <gui/tuner.h>
#include <signal_path/signal_path.h>
#include <module.h>
#include <utils/flog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "channel_bank.h"
#include "detect/tables.h"
#include "frs_tables.h"
#include "radio_iface_compat.h"

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "frsgmrs_monitor",
    /* Description:     */ "FRS/GMRS channel board with live CTCSS/DCS privacy-code detection, shortcuts and scanner",
    /* Author:          */ "Greg Lamb",
    /* Version:         */ 0, 2, 0,
    /* Max instances    */ -1
};

ConfigManager config;

namespace {

constexpr double VFO_BANDWIDTH = 200000.0;
constexpr double VFO_SAMPLERATE = 250000.0;
constexpr double VFO_USABLE_SPAN = 186000.0;   // keep channels off the VFO filter skirt
constexpr double SCAN_BLANK_AFTER_RETUNE = 0.5; // s: ignore activity right after the SDR hops

struct Preset {
    int channel = 1;
    int kind = 1;            // 0 = CTCSS, 1 = DCS
    float hz = 100.0f;
    int dcs = 0023;          // octal
    char name[32] = "";
    bool watched = false;
    bool matches(int ch, const frsmon::DetectResult& r) const {
        if (ch != channel || r.kind == frsmon::CodeKind::None) { return false; }
        if (kind == 0) { return r.kind == frsmon::CodeKind::CTCSS && std::fabs(r.ctcssHz - hz) < 0.05f; }
        return r.kind == frsmon::CodeKind::DCS && r.dcsCode == dcs;
    }
};

struct LogEntry {
    std::time_t wallStart = 0;
    double monoStart = 0, monoEnd = 0;   // steady clock, for duration
    int channel = 0;
    std::string code;        // "" while no code confirmed
    int privacy = 0;
    float peakDb = -120.0f;
    std::string who;
    bool whoWatched = false;
    bool mixed = false;
    bool open = true;
};

std::string timeString(std::time_t t) {
    char buf[16];
    std::tm tm = *std::localtime(&t);
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return buf;
}

std::string ago(double seconds) {
    char buf[16];
    if (seconds < 1.0) { return "now"; }
    if (seconds < 60) { std::snprintf(buf, sizeof(buf), "%ds", (int)seconds); }
    else if (seconds < 3600) { std::snprintf(buf, sizeof(buf), "%dm", (int)(seconds / 60)); }
    else { std::snprintf(buf, sizeof(buf), "%dh", (int)(seconds / 3600)); }
    return buf;
}

std::string codeText(const frsmon::ChannelCodeState& st) {
    std::string txt = st.result.label();
    int priv = st.result.kind == frsmon::CodeKind::DCS ? st.result.dcsNumber : st.result.ctcssNumber;
    if (priv) { txt += " (" + std::to_string(priv) + ")"; }
    return txt;
}

// Shortcut grid: 4 rows x 8 columns of channel numbers (0 = empty cell).
constexpr int GRID_ROWS = 4, GRID_COLS = 8;
constexpr int GRID[GRID_ROWS][GRID_COLS] = {
    {1, 2, 3, 4, 5, 6, 7, 0},
    {8, 9, 10, 11, 12, 13, 14, 0},
    {15, 16, 17, 18, 19, 20, 21, 22},
    {23, 24, 25, 26, 27, 28, 29, 30},
};

} // namespace

class FrsGmrsMonitorModule : public ModuleManager::Instance {
public:
    FrsGmrsMonitorModule(std::string name) : name(name) {
        loadConfig();

        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 0, VFO_BANDWIDTH, VFO_SAMPLERATE,
                                            VFO_BANDWIDTH, VFO_BANDWIDTH, true);
        vfo->setSnapInterval(12500);

        frsmon::BankConfig bc;
        bc.inputRate = VFO_SAMPLERATE;
        bc.carrierThresholdDb = carrierThresholdDb;
        bc.afc = afcEnabled;
        bank.init(vfo->output, bc);
        bank.setInvert(dcsInvert);
        bank.setHoldSeconds(holdMinutes * 60.0);
        applyBlock(false);
        bank.start();

        running = true;
        monitorThread = std::thread(&FrsGmrsMonitorModule::monitorLoop, this);

        gui::menu.registerEntry(name, menuHandler, this, this);
    }

    ~FrsGmrsMonitorModule() {
        gui::menu.removeEntry(name);
        running = false;
        if (monitorThread.joinable()) { monitorThread.join(); }
        if (enabled) {
            bank.stop();
            sigpath::vfoManager.deleteVFO(vfo);
        }
    }

    // All modules (sources included) are loaded by now: arm the start-up automation.
    void postInit() {
        autoArmedAt = frsmon::steadySeconds();
        autoPlayPending = autoPlay;
        autoScanPending = autoScan;
        if (autoPlayPending || autoScanPending) {
            flog::info("FRS/GMRS Monitor: start-up automation armed (play={}, scan={})", autoPlay, autoScan);
        }
    }

    void enable() {
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 0, VFO_BANDWIDTH, VFO_SAMPLERATE,
                                            VFO_BANDWIDTH, VFO_BANDWIDTH, true);
        vfo->setSnapInterval(12500);
        bank.setInput(vfo->output);
        applyBlock(false);
        bank.start();
        enabled = true;
    }

    void disable() {
        scanOn = false;
        bank.stop();
        sigpath::vfoManager.deleteVFO(vfo);
        enabled = false;
    }

    bool isEnabled() { return enabled; }

private:
    using ChannelSet = frsmon::ChannelBank::ChannelSet;
    using Channel = frsmon::ChannelBank::Channel;

    // ------------------------------------------------------------ config ---
    void loadConfig() {
        config.acquire();
        json& c = config.conf[name];
        // First run: the entry doesn't exist yet and operator[] leaves a null,
        // on which json::value() throws. Make it an object.
        if (!c.is_object()) { c = json::object(); }
        blockIdx = c.value("block", 0);
        if (blockIdx < 0 || blockIdx >= (int)frsmon::frsBlocks().size()) { blockIdx = 0; }
        carrierThresholdDb = c.value("carrierThresholdDb", 8.0f);
        dcsInvert = c.value("dcsInvert", false);
        afcEnabled = c.value("afc", true);
        holdMinutes = c.value("holdMinutes", 10);
        showLog = c.value("showLog", false);
        showPresets = c.value("showPresets", false);
        showGrid = c.value("showChannels", true);
        alertEnabled = c.value("alerts", true);
        autoPlay = c.value("autoPlay", false);
        autoScan = c.value("autoScan", false);
        scanMode = c.value("scanMode", 0);
        scanHang = c.value("scanHangSeconds", 3.0f);
        scanDwell = c.value("scanDwellSeconds", 2.0f);
        scanSkip.clear();
        if (c.contains("scanSkip")) {
            for (auto& n : c["scanSkip"]) { int v = n.get<int>(); if (frsmon::frsValidChannel(v)) { scanSkip.push_back(v); } }
        }
        presets.clear();
        if (c.contains("presets")) {
            for (auto& p : c["presets"]) {
                Preset pr;
                pr.channel = p.value("channel", 1);
                if (!frsmon::frsValidChannel(pr.channel)) { pr.channel = 1; }
                std::string kind = p.value("kind", "DCS");
                pr.kind = (kind == "CTCSS") ? 0 : 1;
                if (pr.kind == 0) { pr.hz = p.value("hz", 100.0f); }
                else { pr.dcs = (int)std::strtol(p.value("dcs", "023").c_str(), nullptr, 8); }
                std::string nm = p.value("name", "");
                std::snprintf(pr.name, sizeof(pr.name), "%s", nm.c_str());
                pr.watched = p.value("watched", false);
                presets.push_back(pr);
            }
        }
        config.release();
        applyEnvOverrides();
    }

    // Environment overrides, so a launcher/shortcut can decide without editing the config:
    //   FRSGMRS_AUTOPLAY=1   press Play once SDR++ is up (same as sdrpp --autostart)
    //   FRSGMRS_SCAN=1       start scanning as soon as the SDR is running (0 = don't)
    //   FRSGMRS_BLOCK=462|467
    //   FRSGMRS_SCAN_MODE=this|both
    void applyEnvOverrides() {
        auto flag = [](const char* name, bool& out) {
            const char* v = std::getenv(name);
            if (!v || !*v) { return; }
            out = !(v[0] == '0' || v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F');
        };
        flag("FRSGMRS_AUTOPLAY", autoPlay);
        flag("FRSGMRS_SCAN", autoScan);
        if (const char* v = std::getenv("FRSGMRS_BLOCK")) {
            if (std::string(v) == "467") { blockIdx = 1; } else if (std::string(v) == "462") { blockIdx = 0; }
        }
        if (const char* v = std::getenv("FRSGMRS_SCAN_MODE")) {
            std::string m = v;
            if (m == "both") { scanMode = 1; } else if (m == "this" || m == "block") { scanMode = 0; }
        }
    }

    void saveConfig() {
        config.acquire();
        json& c = config.conf[name];
        if (!c.is_object()) { c = json::object(); }
        c["block"] = blockIdx;
        c["carrierThresholdDb"] = carrierThresholdDb;
        c["dcsInvert"] = dcsInvert;
        c["afc"] = afcEnabled;
        c["holdMinutes"] = holdMinutes;
        c["showLog"] = showLog;
        c["showPresets"] = showPresets;
        c["showChannels"] = showGrid;
        c["alerts"] = alertEnabled;
        c["autoPlay"] = autoPlay;
        c["autoScan"] = autoScan;
        c["scanMode"] = scanMode;
        c["scanHangSeconds"] = scanHang;
        c["scanDwellSeconds"] = scanDwell;
        c["scanSkip"] = scanSkip;
        json arr = json::array();
        for (auto& p : presets) {
            json j;
            j["channel"] = p.channel;
            j["kind"] = p.kind == 0 ? "CTCSS" : "DCS";
            if (p.kind == 0) { j["hz"] = p.hz; } else { j["dcs"] = frsmon::dcsOctal(p.dcs); }
            j["name"] = std::string(p.name);
            j["watched"] = p.watched;
            arr.push_back(j);
        }
        c["presets"] = arr;
        config.release(true);
    }

    // ------------------------------------------------------------- block ---
    double vfoCenterHz() {
        return gui::waterfall.getCenterFrequency() + vfo->wtfVFO->generalOffset;
    }

    static std::vector<std::pair<int, double>> chansFor(int b) {
        std::vector<std::pair<int, double>> chans;
        for (int n : frsmon::frsBlocks()[b].channels) { chans.emplace_back(n, frsmon::frsChannel(n).freqHz); }
        return chans;
    }

    // Make blockIdx the live block: swap its channel set into the bank (the
    // other block's set is parked, keeping held codes / last-heard), reset the
    // per-row bookkeeping and the scanner.
    void applyBlock(bool persist) {
        std::lock_guard<std::recursive_mutex> blck(bank.mutex());
        std::lock_guard<std::recursive_mutex> lck(logMtx);
        const double now = frsmon::steadySeconds();
        lastVfoCenter = vfoCenterHz();
        if (liveBlock != blockIdx) {
            ChannelSet incoming = std::move(parkedSets[blockIdx]);
            if (incoming.empty()) { incoming = bank.buildChannels(chansFor(blockIdx)); }
            ChannelSet outgoing = bank.setChannels(std::move(incoming), lastVfoCenter, VFO_USABLE_SPAN);
            if (liveBlock >= 0) { parkedSets[liveBlock] = std::move(outgoing); }
            liveBlock = blockIdx;
        } else {
            bank.setVfoCenter(lastVfoCenter, VFO_USABLE_SPAN);
        }
        const size_t n = bank.channels().size();
        closeOpenLogs(now);
        openLog.assign(n, -1);
        wasBusy.assign(n, false);
        alertUntil.assign(n, 0.0);
        selected = -1;
        scanParked = -1;
        scanBlankUntil = now + SCAN_BLANK_AFTER_RETUNE;
        scanIdleSince = now;
        if (persist) { saveConfig(); }
    }

    void switchBlock(int idx, bool persist) {
        if (idx < 0 || idx >= (int)frsmon::frsBlocks().size()) { return; }
        blockIdx = idx;
        centerOnBlock();
        applyBlock(persist);
    }

    void centerOnBlock() {
        const auto& blk = frsmon::frsBlocks()[blockIdx];
        tuner::tune(tuner::TUNER_MODE_CENTER, name, blk.centerHz);
    }

    bool blockCovered() {
        const auto& blk = frsmon::frsBlocks()[blockIdx];
        return std::fabs(vfoCenterHz() - blk.centerHz) <= (VFO_USABLE_SPAN - blk.spanHz) / 2.0;
    }

    // logMtx held
    void closeOpenLogs(double now) {
        for (size_t i = 0; i < openLog.size(); i++) {
            if (openLog[i] >= 0 && openLog[i] < (int)log.size()) {
                log[openLog[i]].open = false;
                if (log[openLog[i]].monoEnd <= 0) { log[openLog[i]].monoEnd = now; }
            }
            openLog[i] = -1;
        }
    }

    // Live or parked channel object for a channel number (bank mutex held).
    Channel* channelFor(int number, bool* live = nullptr) {
        for (auto& ch : bank.channels()) { if (ch->number == number) { if (live) { *live = true; } return ch.get(); } }
        for (auto& set : parkedSets) { for (auto& ch : set) { if (ch->number == number) { if (live) { *live = false; } return ch.get(); } } }
        if (live) { *live = false; }
        return nullptr;
    }

    // ----------------------------------------------------- monitor thread ---
    void monitorLoop() {
        while (running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const double now = frsmon::steadySeconds();
            runStartupAutomation(now);
            if (!enabled) { continue; }
            // follow the VFO if the user dragged it or retuned
            double c = vfoCenterHz();
            if (std::fabs(c - lastVfoCenter) > 1.0) {
                lastVfoCenter = c;
                bank.setVfoCenter(c, VFO_USABLE_SPAN);
            }
            std::lock_guard<std::recursive_mutex> blck(bank.mutex());
            auto& chans = bank.channels();
            std::lock_guard<std::recursive_mutex> lck(logMtx);
            if (wasBusy.size() != chans.size()) { continue; }   // block switch in progress
            for (size_t i = 0; i < chans.size(); i++) {
                auto& ch = *chans[i];
                ch.det->tick(now);
                const bool busy = ch.busy.load();
                auto st = ch.det->state();
                if (busy && !wasBusy[i]) {
                    LogEntry e;
                    e.wallStart = std::time(nullptr);
                    e.monoStart = ch.busySince.load();
                    e.channel = ch.number;
                    log.insert(log.begin(), e);
                    if (log.size() > (size_t)logMaxRows) { log.pop_back(); }
                    openLog[i] = 0;
                    // indices of other open entries shift by one
                    for (size_t k = 0; k < openLog.size(); k++) { if (k != i && openLog[k] >= 0) { openLog[k]++; } }
                }
                if (openLog[i] >= 0 && openLog[i] < (int)log.size()) {
                    LogEntry& e = log[openLog[i]];
                    e.peakDb = ch.peakDb.load();
                    e.monoEnd = now;
                    if (st.confirmedThisKeydown && st.result.kind != frsmon::CodeKind::None) {
                        bool newCode = e.code != st.result.label();
                        e.code = st.result.label();
                        e.privacy = st.result.kind == frsmon::CodeKind::DCS ? st.result.dcsNumber : st.result.ctcssNumber;
                        e.mixed = st.mixed;
                        e.who = presetName(ch.number, st.result, &e);
                        if (newCode) {
                            flog::info("FRS/GMRS Monitor: ch {} {} {}{}", frsmon::frsChannelLabel(ch.number), e.code, st.result.privacyLabel(),
                                       e.who.empty() ? "" : " [" + e.who + "]");
                            if (alertEnabled && e.whoWatched) { alertUntil[i] = now + 3.0; alertCount++; }
                        }
                    }
                }
                if (!busy && wasBusy[i]) {
                    if (openLog[i] >= 0 && openLog[i] < (int)log.size()) {
                        log[openLog[i]].open = false;
                        log[openLog[i]].monoEnd = ch.lastBusy.load();
                    }
                    openLog[i] = -1;
                }
                wasBusy[i] = busy;
            }
            scanTick(now, chans);
        }
    }

    // Start-up automation (monitor thread). rigctl_server drives the play state
    // from its own thread the same way.
    void runStartupAutomation(double now) {
        if (autoArmedAt <= 0 || now - autoArmedAt < 1.5) { return; }
        if (autoPlayPending) {
            autoPlayPending = false;
            if (!gui::mainWindow.isPlaying()) {
                flog::info("FRS/GMRS Monitor: auto-play");
                gui::mainWindow.setPlayState(true);
            }
        }
        if (autoScanPending && enabled && gui::mainWindow.isPlaying()) {
            autoScanPending = false;
            centerOnBlock();
            scanParked = -1;
            scanIdleSince = now;
            scanBlankUntil = now + 1.0;
            scanOn = true;
            flog::info("FRS/GMRS Monitor: auto-scan on ({})", frsmon::frsBlocks()[blockIdx].name);
        }
    }

    // ----------------------------------------------------------- scanner ---
    // All channels of the block are demodulated at once, so "scanning" a block
    // is instantaneous: park the Radio on the first busy channel, stay while it
    // talks plus a hang time, then resume. In "both blocks" mode the SDR hops
    // between the 462 and 467 blocks every `scanDwell` seconds while idle.
    // Called from the monitor thread with bank mutex + logMtx held.
    void scanTick(double now, ChannelSet& chans) {
        if (!scanOn) { scanParked = -1; return; }
        int parked = scanParked.load();
        if (parked >= 0) {
            if (parked >= (int)chans.size()) { scanParked = -1; }
            else if (chans[parked]->busy.load()) { scanLastActive = now; return; }      // still talking
            else if (now - scanLastActive < scanHang) { return; }                         // hang time
            else {
                flog::info("FRS/GMRS Monitor: scan resumed");
                scanParked = -1;
                scanIdleSince = now;
            }
        }
        if (now < scanBlankUntil) { return; }

        int best = -1; double bestSince = 1e300; bool bestWatched = false;
        for (int i = 0; i < (int)chans.size(); i++) {
            auto& ch = *chans[i];
            if (!ch.active.load() || !ch.busy.load() || isSkipped(ch.number)) { continue; }
            bool watched = false;
            auto st = ch.det->state();
            if (st.stage != frsmon::CodeStage::None && st.result.kind != frsmon::CodeKind::None) {
                LogEntry tmp; presetName(ch.number, st.result, &tmp); watched = tmp.whoWatched;
            }
            double since = ch.busySince.load();
            if ((watched && !bestWatched) || (watched == bestWatched && since < bestSince)) {
                best = i; bestSince = since; bestWatched = watched;
            }
        }
        if (best >= 0) {
            auto& ch = *chans[best];
            scanParked = best;
            scanLastActive = now;
            selected = best;
            tuneRadio(ch.freqHz, frsmon::frsChannel(ch.number).wide);
            flog::info("FRS/GMRS Monitor: scan stopped on ch {}", frsmon::frsChannelLabel(ch.number));
            return;
        }
        if (scanMode == 1 && now - scanIdleSince >= scanDwell) {
            int next = (blockIdx + 1) % (int)frsmon::frsBlocks().size();
            flog::info("FRS/GMRS Monitor: scan hop to {}", frsmon::frsBlocks()[next].name);
            switchBlock(next, false);      // resets scanIdleSince / scanBlankUntil
        }
    }

    // logMtx held
    bool isSkipped(int number) const {
        return std::find(scanSkip.begin(), scanSkip.end(), number) != scanSkip.end();
    }
    void setSkipped(int number, bool skip) {
        std::lock_guard<std::recursive_mutex> lck(logMtx);
        auto it = std::find(scanSkip.begin(), scanSkip.end(), number);
        if (skip && it == scanSkip.end()) { scanSkip.push_back(number); std::sort(scanSkip.begin(), scanSkip.end()); }
        if (!skip && it != scanSkip.end()) { scanSkip.erase(it); }
        saveConfig();
    }

    std::string presetName(int channel, const frsmon::DetectResult& r, LogEntry* e) {
        for (auto& p : presets) {
            if (p.matches(channel, r)) { if (e) { e->whoWatched = p.watched; } return p.name; }
        }
        if (e) { e->whoWatched = false; }
        return "";
    }

    // ------------------------------------------------------------- radio ---
    // GUI thread: find the stock Radio module's VFO and which channel it sits on.
    void refreshRadioVfo() {
        std::string target;
        for (auto& [vname, wvfo] : gui::waterfall.vfos) {
            if (vname == name) { continue; }
            if (core::modComManager.interfaceExists(vname) && core::modComManager.getModuleName(vname) == "radio") {
                target = vname;
                if (vname == "Radio") { break; }
            }
        }
        int lc = -1;
        if (!target.empty()) {
            double f = gui::waterfall.getCenterFrequency() + gui::waterfall.vfos[target]->generalOffset;
            for (const auto& ch : frsmon::FRS_CHANNELS) { if (std::fabs(ch.freqHz - f) < 3000.0) { lc = ch.number; break; } }
        }
        std::lock_guard<std::mutex> lck(uiMtx);
        radioVfo = target;
        listenChannel = lc;
    }

    // Any thread. Tunes the Radio module to freqHz in NFM with a channel-appropriate bandwidth.
    bool tuneRadio(double freqHz, bool wide) {
        std::string target;
        { std::lock_guard<std::mutex> lck(uiMtx); target = radioVfo; }
        if (target.empty()) {
            if (!warnedNoRadio) { flog::warn("FRS/GMRS Monitor: no Radio module VFO to listen with"); warnedNoRadio = true; }
            return false;
        }
        int mode = RADIO_IFACE_MODE_NFM;
        float bw = wide ? 20000.0f : 12500.0f;
        core::modComManager.callInterface(target, RADIO_IFACE_CMD_SET_MODE, &mode, NULL);
        core::modComManager.callInterface(target, RADIO_IFACE_CMD_SET_BANDWIDTH, &bw, NULL);
        tuner::tune(tuner::TUNER_MODE_NORMAL, target, freqHz);
        return true;
    }

    // GUI thread: manual listen. Stops the scanner (like pressing a channel on a scanner).
    void listenTo(int number) {
        scanOn = false;
        scanParked = -1;
        const auto& fc = frsmon::frsChannel(number);
        if (tuneRadio(fc.freqHz, fc.wide)) {
            std::lock_guard<std::mutex> lck(uiMtx);
            gui::waterfall.selectedVFO = radioVfo;
        }
    }

    // GUI thread: channel shortcut. Switches block (re-centering the SDR) if needed.
    void goToChannel(int number) {
        if (!frsmon::frsValidChannel(number)) { return; }
        int b = frsmon::frsBlockOf(number);
        if (b != blockIdx) { switchBlock(b, true); }
        else if (!blockCovered()) { centerOnBlock(); }
        selected = frsmon::frsRowOf(blockIdx, number);
        listenTo(number);
    }

    // --------------------------------------------------------------- CSV ---
    void exportCsv() {
        std::time_t t = std::time(nullptr);
        char fn[64];
        std::strftime(fn, sizeof(fn), "frsgmrs_log_%Y%m%d_%H%M%S.csv", std::localtime(&t));
        std::string path = core::args["root"].s() + "/" + fn;
        std::ofstream f(path);
        f << "time,channel,freq_mhz,code,privacy,duration_s,peak_dbfs,who,mixed\n";
        std::lock_guard<std::recursive_mutex> lck(logMtx);
        for (auto it = log.rbegin(); it != log.rend(); ++it) {
            const LogEntry& e = *it;
            f << timeString(e.wallStart) << "," << frsmon::frsChannelLabel(e.channel) << "," << std::fixed;
            f.precision(4); f << frsmon::frsChannel(e.channel).freqHz / 1e6 << ",";
            f << e.code << "," << e.privacy << ",";
            f.precision(1); f << (e.monoEnd - e.monoStart) << "," << e.peakDb << ",";
            f << e.who << "," << (e.mixed ? "yes" : "") << "\n";
        }
        lastExport = path;
        flog::info("FRS/GMRS Monitor: log exported to {}", path);
    }

    // ---------------------------------------------------------------- UI ---
    static void menuHandler(void* ctx) { ((FrsGmrsMonitorModule*)ctx)->drawMenu(); }

    void drawMenu() {
        float menuWidth = ImGui::GetContentRegionAvail().x;
        if (!enabled) { style::beginDisabled(); }
        if (enabled) { refreshRadioVfo(); }
        const double now = frsmon::steadySeconds();

        // Block + centre
        ImGui::LeftLabel("Block");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::BeginCombo(CONCAT("##frsmon_block_", name), frsmon::frsBlocks()[blockIdx].name)) {
            for (int i = 0; i < (int)frsmon::frsBlocks().size(); i++) {
                if (ImGui::Selectable(frsmon::frsBlocks()[i].name, i == blockIdx) && i != blockIdx) {
                    switchBlock(i, true);
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::Button(CONCAT("Center SDR on block##frsmon_center_", name), ImVec2(menuWidth, 0))) { centerOnBlock(); }
        {
            double devRate = sigpath::iqFrontEnd.getSampleRate();
            bool covered = blockCovered();
            if (devRate < VFO_BANDWIDTH + 10000) {
                ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Device rate %.0f kS/s too low (need >= 250)", devRate / 1e3);
            } else if (!covered) {
                ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "VFO off the block - press Center");
            } else {
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "Device %.2f MS/s, block covered", devRate / 1e6);
            }
        }

        // Channel shortcuts
        if (showGrid) { drawChannelGrid(menuWidth, now); }

        // Board
        drawBoard(menuWidth, now);

        // Scanner
        drawScanRow(menuWidth, now);

        // Listen / windows
        {
            std::lock_guard<std::recursive_mutex> blck(bank.mutex());
            auto& chans = bank.channels();
            int sel = selected.load();
            bool haveSel = sel >= 0 && sel < (int)chans.size();
            if (!haveSel) { style::beginDisabled(); }
            if (ImGui::Button(CONCAT("Listen##frsmon_listen_", name), ImVec2(menuWidth * 0.36f, 0)) && haveSel) {
                listenTo(chans[sel]->number);
            }
            if (!haveSel) { style::endDisabled(); }
        }
        ImGui::SameLine();
        if (ImGui::Checkbox(CONCAT("Log##frsmon_showlog_", name), &showLog)) { saveConfig(); }
        ImGui::SameLine();
        if (ImGui::Checkbox(CONCAT("Presets##frsmon_showpre_", name), &showPresets)) { saveConfig(); }
        ImGui::SameLine();
        if (ImGui::Checkbox(CONCAT("Keys##frsmon_showgrid_", name), &showGrid)) { saveConfig(); }
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Show the one-click channel keys (1-22 and repeater inputs 15R-22R)"); }

        // Options
        if (ImGui::CollapsingHeader(CONCAT("Options##frsmon_opts_", name))) {
            ImGui::LeftLabel("Carrier threshold");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::SliderFloat(CONCAT("##frsmon_thr_", name), &carrierThresholdDb, 3.0f, 20.0f, "%.0f dB")) {
                bank.setCarrierThreshold(carrierThresholdDb); saveConfig();
            }
            if (ImGui::Checkbox(CONCAT("AFC (follow off-frequency radios)##frsmon_afc_", name), &afcEnabled)) {
                bank.setAfcEnabled(afcEnabled); saveConfig();
            }
            if (ImGui::Checkbox(CONCAT("Invert DCS polarity##frsmon_inv_", name), &dcsInvert)) {
                bank.setInvert(dcsInvert); saveConfig();
            }
            if (ImGui::Checkbox(CONCAT("Alert on watched presets##frsmon_alert_", name), &alertEnabled)) { saveConfig(); }
            ImGui::LeftLabel("Hold last code");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::SliderInt(CONCAT("##frsmon_hold_", name), &holdMinutes, 1, 60, "%d min")) {
                bank.setHoldSeconds(holdMinutes * 60.0); saveConfig();
            }
            if (ImGui::Checkbox(CONCAT("Start the SDR when SDR++ opens##frsmon_autoplay_", name), &autoPlay)) { saveConfig(); }
            if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Presses Play once everything is loaded (like sdrpp --autostart).\nEnv override: FRSGMRS_AUTOPLAY=1"); }
            if (ImGui::Checkbox(CONCAT("Start scanning when the SDR starts##frsmon_autoscan_", name), &autoScan)) { saveConfig(); }
            if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Centres the SDR on the block and turns Scan on as soon as the SDR is running.\nEnv overrides: FRSGMRS_SCAN=1, FRSGMRS_BLOCK=462|467, FRSGMRS_SCAN_MODE=this|both"); }
            ImGui::LeftLabel("Scan hang time");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::SliderFloat(CONCAT("##frsmon_hang_", name), &scanHang, 0.5f, 10.0f, "%.1f s")) { saveConfig(); }
            ImGui::LeftLabel("Scan dwell per block");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::SliderFloat(CONCAT("##frsmon_dwell_", name), &scanDwell, 1.0f, 10.0f, "%.1f s")) { saveConfig(); }
            {
                std::lock_guard<std::recursive_mutex> blck(bank.mutex());
                std::lock_guard<std::recursive_mutex> lck(logMtx);
                if (scanSkip.empty()) {
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "Scan skips: none (right-click a channel)");
                } else {
                    std::string s = "Scan skips:";
                    for (int n : scanSkip) { s += " " + frsmon::frsChannelLabel(n); }
                    ImGui::TextUnformatted(s.c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton(CONCAT("Clear##frsmon_skipclr_", name))) { scanSkip.clear(); saveConfig(); }
                }
            }
        }

        if (!enabled) { style::endDisabled(); }

        if (showLog) { drawLogWindow(); }
        if (showPresets) { drawPresetsWindow(); }
    }

    // One button per channel: 1-7 / 8-14 / 15-22 / 15R-22R. Click = tune the
    // Radio there (switching the SDR to the other block when needed).
    void drawChannelGrid(float menuWidth, double now) {
        std::lock_guard<std::recursive_mutex> blck(bank.mutex());
        std::lock_guard<std::recursive_mutex> lck(logMtx);
        int lc; { std::lock_guard<std::mutex> ul(uiMtx); lc = listenChannel; }
        const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
        const float w = std::floor((menuWidth - spacing * (GRID_COLS - 1)) / GRID_COLS);
        int goTo = 0;
        for (int r = 0; r < GRID_ROWS; r++) {
            for (int c = 0; c < GRID_COLS; c++) {
                const int n = GRID[r][c];
                if (c > 0) { ImGui::SameLine(0, spacing); }
                if (n == 0) { ImGui::Dummy(ImVec2(w, 0)); continue; }
                bool live = false;
                Channel* ch = channelFor(n, &live);
                const bool busy = live && ch && ch->busy.load();
                const bool listening = (n == lc);
                const bool skipped = isSkipped(n);
                int pushed = 0;
                if (listening) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.85f, 1)); pushed++;
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.55f, 0.95f, 1)); pushed++;
                } else if (busy) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.55f, 0.25f, 1)); pushed++;
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.65f, 0.30f, 1)); pushed++;
                }
                if (skipped) { ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.5f, 0.5f, 1)); pushed++; }
                if (!live) { ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.55f); }
                char lbl[32];
                std::snprintf(lbl, sizeof(lbl), "%s##frsmon_key_%d_%s", frsmon::frsChannelLabel(n).c_str(), n, name.c_str());
                if (ImGui::Button(lbl, ImVec2(w, 0))) { goTo = n; }
                if (!live) { ImGui::PopStyleVar(); }
                if (pushed) { ImGui::PopStyleColor(pushed); }
                if (ImGui::IsItemHovered()) { drawChannelTooltip(n, ch, live, now, /*grid*/ true); }
                drawChannelContext(n, ch);
            }
        }
        if (goTo) { goToChannel(goTo); }
    }

    void drawScanRow(float menuWidth, double now) {
        bool on = scanOn.load();
        if (on) { ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.55f, 0.25f, 1)); }
        if (ImGui::Button(CONCAT(on ? "Stop scan##frsmon_scan_" : "Scan##frsmon_scan_", name), ImVec2(menuWidth * 0.36f, 0))) {
            scanOn = !on;
            scanParked = -1;
            scanIdleSince = now;
            scanBlankUntil = 0;
            flog::info("FRS/GMRS Monitor: scan {}", on ? "off" : "on");
        }
        if (on) { ImGui::PopStyleColor(); }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::Combo(CONCAT("##frsmon_scanmode_", name), &scanMode, "This block only\0Both blocks (hop 462/467)\0")) { saveConfig(); }

        if (!on) { return; }
        std::lock_guard<std::recursive_mutex> blck(bank.mutex());
        auto& chans = bank.channels();
        int parked = scanParked.load();
        if (parked >= 0 && parked < (int)chans.size()) {
            auto& ch = *chans[parked];
            auto st = ch.det->state();
            std::string s = "Stopped on ch " + frsmon::frsChannelLabel(ch.number);
            if (st.stage != frsmon::CodeStage::None && st.result.kind != frsmon::CodeKind::None) {
                s += "  " + codeText(st);
                std::string who = presetName(ch.number, st.result, nullptr);
                if (!who.empty()) { s += "  [" + who + "]"; }
            }
            if (!ch.busy.load()) { s += "  (hang)"; }
            ImGui::TextColored(ImVec4(0.3f, 0.95f, 0.4f, 1), "%s", s.c_str());
        } else {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "Scanning %s%s", scanMode == 1 ? "both blocks, now on " : "",
                               scanMode == 1 ? (blockIdx == 0 ? "462" : "467") : frsmon::frsBlocks()[blockIdx].name);
        }
        // Radio squelch hint: the Radio module's squelch is what silences idle channels.
        std::string target; { std::lock_guard<std::mutex> ul(uiMtx); target = radioVfo; }
        if (target.empty()) {
            ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "No Radio module found to listen with");
        } else {
            int sq = 0;   // 0 = off in every SDR++ version (bool or SquelchMode enum)
            core::modComManager.callInterface(target, RADIO_IFACE_CMD_GET_SQUELCH_MODE, NULL, &sq);
            if (sq == 0) { ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "Tip: turn on the Radio squelch so idle channels are quiet"); }
        }
    }

    void drawBoard(float menuWidth, double now) {
        std::lock_guard<std::recursive_mutex> blck(bank.mutex());
        auto& chans = bank.channels();
        ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadOuterX;
        if (!ImGui::BeginTable(CONCAT("##frsmon_board_", name), 5, flags, ImVec2(menuWidth, 0))) { return; }
        ImGui::TableSetupColumn("Ch", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("15R").x + 6);
        ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, 56);
        ImGui::TableSetupColumn("Code", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Who", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Family").x + 6);
        ImGui::TableSetupColumn("Last", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("59m").x + 6);
        ImGui::TableHeadersRow();

        std::lock_guard<std::recursive_mutex> lck(logMtx);
        const int parked = scanOn ? scanParked.load() : -1;
        int lc; { std::lock_guard<std::mutex> ul(uiMtx); lc = listenChannel; }
        int listenRow = -1, dbl = -1;
        for (int i = 0; i < (int)chans.size(); i++) {
            auto& ch = *chans[i];
            auto st = ch.det->state();
            const bool busy = ch.busy.load();
            const bool active = ch.active.load();
            const float level = ch.levelDb.load(), floor = ch.floorDb.load();
            if (ch.number == lc) { listenRow = i; }
            ImGui::TableNextRow();
            if (i < (int)alertUntil.size() && alertUntil[i] > now && ((int)(now * 4) % 2 == 0)) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImVec4(0.8f, 0.3f, 0.1f, 0.55f)));
            } else if (busy) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImVec4(0.1f, 0.45f, 0.2f, 0.45f)));
            }
            // Ch (selectable row)
            ImGui::TableSetColumnIndex(0);
            char lbl[32]; std::snprintf(lbl, sizeof(lbl), "%s##frsmon_row_%d_%s", frsmon::frsChannelLabel(ch.number).c_str(), i, name.c_str());
            const bool skipped = isSkipped(ch.number);
            if (!active) { ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1)); }
            else if (skipped) { ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.5f, 0.5f, 1)); }
            if (ImGui::Selectable(lbl, selected == i, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_SelectOnClick)) { selected = i; }
            if (!active || skipped) { ImGui::PopStyleColor(); }
            if (ImGui::IsItemHovered()) {
                drawChannelTooltip(ch.number, &ch, true, now, /*grid*/ false);
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { selected = i; dbl = ch.number; }
            }
            drawChannelContext(ch.number, &ch);
            // Level
            ImGui::TableSetColumnIndex(1);
            float frac = active ? std::clamp((level - floor) / 40.0f, 0.0f, 1.0f) : 0.0f;
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, busy ? ImVec4(0.3f, 0.85f, 0.4f, 1) : ImVec4(0.4f, 0.4f, 0.4f, 1));
            ImGui::ProgressBar(frac, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight()), "");
            ImGui::PopStyleColor();
            // Code
            ImGui::TableSetColumnIndex(2);
            drawCodeCell(st, busy);
            // Who
            ImGui::TableSetColumnIndex(3);
            if (st.stage != frsmon::CodeStage::None) {
                std::string who = presetName(ch.number, st.result, nullptr);
                ImGui::TextUnformatted(who.c_str());
            }
            // Last
            ImGui::TableSetColumnIndex(4);
            double lb = ch.lastBusy.load();
            if (i == parked) { ImGui::TextColored(ImVec4(0.3f, 0.95f, 0.4f, 1), "scan"); }
            else if (i == listenRow) { ImGui::TextColored(ImVec4(0.4f, 0.65f, 1.0f, 1), "radio"); }
            else if (busy) { ImGui::TextUnformatted("now"); }
            else if (lb > 0) { ImGui::TextUnformatted(ago(now - lb).c_str()); }
        }
        ImGui::EndTable();
        if (dbl > 0) { listenTo(dbl); }
    }

    void drawCodeCell(const frsmon::ChannelCodeState& st, bool busy) {
        using frsmon::CodeStage;
        if (st.stage == CodeStage::None || st.result.kind == frsmon::CodeKind::None) {
            if (busy) { ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "..."); }
            return;
        }
        std::string txt = codeText(st);
        ImVec4 col;
        switch (st.stage) {
        case CodeStage::Tentative: col = ImVec4(1.0f, 0.85f, 0.2f, 1); txt += "?"; break;
        case CodeStage::Confirmed: col = ImVec4(0.3f, 0.95f, 0.4f, 1); break;
        default: col = ImVec4(0.55f, 0.55f, 0.55f, 1); break;    // Held
        }
        if (st.mixed) { txt += " *"; }
        ImGui::TextColored(col, "%s", txt.c_str());
    }

    // Right-click menu shared by grid keys and board rows (last item is the anchor).
    void drawChannelContext(int number, Channel* ch) {
        if (!ImGui::BeginPopupContextItem()) { return; }
        ImGui::Text("Channel %s  %.4f MHz", frsmon::frsChannelLabel(number).c_str(), frsmon::frsChannel(number).freqHz / 1e6);
        ImGui::Separator();
        if (ImGui::MenuItem("Listen here")) { goToChannel(number); }
        bool skip = isSkipped(number);
        if (ImGui::MenuItem("Skip in scan", nullptr, &skip)) { setSkipped(number, skip); }
        int pair = frsmon::frsIsRepeaterInput(number) ? frsmon::frsRepeaterOutputOf(number) : frsmon::frsRepeaterInputOf(number);
        if (pair > 0) {
            std::string s = frsmon::frsIsRepeaterInput(number) ? "Repeater output: ch " : "Repeater input: ch ";
            s += frsmon::frsChannelLabel(pair);
            if (ImGui::MenuItem(s.c_str())) { goToChannel(pair); }
        }
        if (ch) {
            auto st = ch->det->state();
            if (st.stage != frsmon::CodeStage::None && st.result.kind != frsmon::CodeKind::None) {
                if (ImGui::MenuItem(("Add preset for " + codeText(st)).c_str())) {
                    Preset p; p.channel = number;
                    if (st.result.kind == frsmon::CodeKind::DCS) { p.kind = 1; p.dcs = st.result.dcsCode; } else { p.kind = 0; p.hz = st.result.ctcssHz; }
                    std::lock_guard<std::recursive_mutex> lck(logMtx);
                    presets.push_back(p); saveConfig(); showPresets = true;
                }
            }
        }
        ImGui::EndPopup();
    }

    void drawChannelTooltip(int number, Channel* ch, bool live, double now, bool grid) {
        const auto& fc = frsmon::frsChannel(number);
        ImGui::BeginTooltip();
        ImGui::Text("Channel %s  %.4f MHz  %s", frsmon::frsChannelLabel(number).c_str(), fc.freqHz / 1e6, fc.wide ? "(20 kHz)" : "(12.5 kHz)");
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1), "%s", fc.group);
        int pair = frsmon::frsIsRepeaterInput(number) ? frsmon::frsRepeaterOutputOf(number) : frsmon::frsRepeaterInputOf(number);
        if (pair > 0) {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1), frsmon::frsIsRepeaterInput(number) ? "users keying a repeater that answers on ch %s" : "repeater output; its input is ch %s",
                               frsmon::frsChannelLabel(pair).c_str());
        }
        if (isSkipped(number)) { ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.5f, 1), "skipped by the scanner"); }
        if (!live) {
            ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "in the other block - click retunes the SDR to %s MHz", fc.block == 0 ? "462" : "467");
        } else if (ch && !ch->active.load()) {
            ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "outside the VFO - press Center SDR on block");
        }
        if (ch) {
            auto st = ch->det->state();
            if (live) {
                ImGui::Text("Level %.1f dBFS, floor %.1f dBFS, %s", ch->levelDb.load(), ch->floorDb.load(), ch->busy.load() ? "carrier" : "idle");
                float afc = ch->afcHzOut.load();
                if (std::fabs(afc) > 1) { ImGui::Text("AFC offset %+.0f Hz", afc); }
            }
            if (st.stage != frsmon::CodeStage::None && st.result.kind != frsmon::CodeKind::None) {
                const char* stage = st.stage == frsmon::CodeStage::Tentative ? "tentative" : st.stage == frsmon::CodeStage::Confirmed ? "confirmed" : "held (last transmission)";
                ImGui::Separator();
                ImGui::Text("%s  %s  [%s]", st.result.label().c_str(), st.result.privacyLabel().c_str(), stage);
                std::string who = presetName(number, st.result, nullptr);
                if (!who.empty()) { ImGui::Text("preset: %s", who.c_str()); }
                if (st.result.kind == frsmon::CodeKind::DCS) {
                    int altPriv = frsmon::dcsPrivacyCode(st.result.dcsAltCode);
                    ImGui::Text("corr %.2f, margin %.2f, %.1f bps", st.result.corr, st.result.margin, st.result.bitrate);
                    ImGui::Text("if your radio reads it inverted: D%sN%s", frsmon::dcsOctal(st.result.dcsAltCode).c_str(),
                                altPriv ? (" (privacy " + std::to_string(altPriv) + ")").c_str() : "");
                } else {
                    ImGui::Text("measured %.2f Hz, %.0f dB over floor, %.0f dB over next tone", st.result.ctcssMeasuredHz, st.result.prominenceDb, st.result.dominanceDb);
                }
                if (st.mixed) { ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "mixed: another code was heard in this key-down"); }
                if (st.stage == frsmon::CodeStage::Held) { ImGui::Text("heard %s ago", ago(now - st.stageSince).c_str()); }
            } else if (live && ch->busy.load()) {
                double buffered = ch->det->bufferedSeconds();
                ImGui::Text(buffered < 1.0 ? "listening (%.1f s)" : "carrier, no code found yet (%.1f s)", buffered);
            } else if (ch->lastBusy.load() > 0) {
                ImGui::Text("last heard %s ago", ago(now - ch->lastBusy.load()).c_str());
            }
        }
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), grid ? "Click: listen here (stops the scan). Right-click: options"
                                                              : "Click: select. Double-click: listen. Right-click: options");
        ImGui::EndTooltip();
    }

    void drawLogWindow() {
        ImGui::SetNextWindowSize(ImVec2(560, 320), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 580, ImGui::GetIO().DisplaySize.y - 340), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(CONCAT("FRS/GMRS Log##frsmon_logwin_", name), &showLog)) { ImGui::End(); return; }
        if (ImGui::Button("Export CSV")) { exportCsv(); }
        ImGui::SameLine();
        if (ImGui::Button("Clear")) { std::lock_guard<std::recursive_mutex> lck(logMtx); log.clear(); std::fill(openLog.begin(), openLog.end(), -1); }
        ImGui::SameLine();
        if (!lastExport.empty()) { ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "saved %s", lastExport.c_str()); }
        ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
        if (ImGui::BeginTable("##frsmon_logtable", 7, flags)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Time"); ImGui::TableSetupColumn("Ch"); ImGui::TableSetupColumn("Code", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Priv"); ImGui::TableSetupColumn("Dur"); ImGui::TableSetupColumn("Peak"); ImGui::TableSetupColumn("Who");
            ImGui::TableHeadersRow();
            std::lock_guard<std::recursive_mutex> lck(logMtx);
            for (const auto& e : log) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(timeString(e.wallStart).c_str());
                ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(frsmon::frsChannelLabel(e.channel).c_str());
                ImGui::TableSetColumnIndex(2);
                if (e.code.empty()) { ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.55f, 1), e.open ? "..." : "(no code)"); }
                else { ImGui::Text("%s%s", e.code.c_str(), e.mixed ? " *" : ""); }
                ImGui::TableSetColumnIndex(3); if (e.privacy) { ImGui::Text("%d", e.privacy); }
                ImGui::TableSetColumnIndex(4); ImGui::Text("%.1fs", e.monoEnd - e.monoStart);
                ImGui::TableSetColumnIndex(5); ImGui::Text("%.0f", e.peakDb);
                ImGui::TableSetColumnIndex(6); ImGui::TextUnformatted(e.who.c_str());
            }
            ImGui::EndTable();
        }
        ImGui::End();
    }

    void drawPresetsWindow() {
        ImGui::SetNextWindowSize(ImVec2(520, 260), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 540, 60), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(CONCAT("FRS/GMRS Presets##frsmon_prewin_", name), &showPresets)) { ImGui::End(); return; }
        ImGui::TextWrapped("Name a channel + code pair. Matches show in the Who column; watched ones flash the row and win the scanner.");
        // Lock order everywhere: bank mutex first, then logMtx (the monitor thread does the same).
        std::lock_guard<std::recursive_mutex> blck(bank.mutex());
        std::lock_guard<std::recursive_mutex> lck(logMtx);
        bool changed = false;
        ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit;
        if (ImGui::BeginTable("##frsmon_pretable", 6, flags)) {
            ImGui::TableSetupColumn("Ch"); ImGui::TableSetupColumn("Type"); ImGui::TableSetupColumn("Code");
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Watch"); ImGui::TableSetupColumn("");
            ImGui::TableHeadersRow();
            for (int i = 0; i < (int)presets.size(); i++) {
                Preset& p = presets[i];
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::SetNextItemWidth(60);
                if (ImGui::BeginCombo("##ch", frsmon::frsChannelLabel(p.channel).c_str())) {
                    for (int n = 1; n <= frsmon::FRS_CHANNEL_COUNT; n++) {
                        if (ImGui::Selectable(frsmon::frsChannelLabel(n).c_str(), n == p.channel)) { p.channel = n; changed = true; }
                    }
                    ImGui::EndCombo();
                }
                ImGui::TableSetColumnIndex(1); ImGui::SetNextItemWidth(70);
                if (ImGui::Combo("##kind", &p.kind, "CTCSS\0DCS\0")) { changed = true; }
                ImGui::TableSetColumnIndex(2); ImGui::SetNextItemWidth(70);
                if (p.kind == 0) {
                    if (ImGui::InputFloat("##hz", &p.hz, 0, 0, "%.1f")) { changed = true; }
                } else {
                    char oct[8]; std::snprintf(oct, sizeof(oct), "%03o", p.dcs);
                    if (ImGui::InputText("##dcs", oct, sizeof(oct), ImGuiInputTextFlags_CharsDecimal)) { p.dcs = (int)std::strtol(oct, nullptr, 8) & 0777; changed = true; }
                }
                ImGui::TableSetColumnIndex(3); ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::InputText("##name", p.name, sizeof(p.name))) { changed = true; }
                ImGui::TableSetColumnIndex(4);
                if (ImGui::Checkbox("##w", &p.watched)) { changed = true; }
                ImGui::TableSetColumnIndex(5);
                if (ImGui::SmallButton("X")) { presets.erase(presets.begin() + i); changed = true; ImGui::PopID(); break; }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (ImGui::Button("Add preset")) { presets.push_back(Preset{}); changed = true; }
        ImGui::SameLine();
        {
            auto& chans = bank.channels();
            int sel = selected.load();
            if (sel >= 0 && sel < (int)chans.size()) {
                auto st = chans[sel]->det->state();
                if (st.stage != frsmon::CodeStage::None && st.result.kind != frsmon::CodeKind::None) {
                    if (ImGui::Button("Add from selected channel")) {
                        Preset p; p.channel = chans[sel]->number;
                        if (st.result.kind == frsmon::CodeKind::DCS) { p.kind = 1; p.dcs = st.result.dcsCode; } else { p.kind = 0; p.hz = st.result.ctcssHz; }
                        presets.push_back(p); changed = true;
                    }
                }
            }
        }
        if (changed) { saveConfig(); }
        ImGui::End();
    }

    // ------------------------------------------------------------- state ---
    std::string name;
    bool enabled = true;
    VFOManager::VFO* vfo = nullptr;
    frsmon::ChannelBank bank;
    std::array<ChannelSet, 2> parkedSets;   // channel sets of blocks that are not live
    int liveBlock = -1;
    std::thread monitorThread;
    std::atomic<bool> running{false};
    double lastVfoCenter = 0;

    // settings
    int blockIdx = 0;
    float carrierThresholdDb = 8.0f;
    bool dcsInvert = false;
    bool afcEnabled = true;
    int holdMinutes = 10;
    bool showLog = false, showPresets = false, showGrid = true, alertEnabled = true;
    bool autoPlay = false, autoScan = false;
    double autoArmedAt = 0;
    bool autoPlayPending = false, autoScanPending = false;
    int logMaxRows = 2000;
    std::vector<Preset> presets;            // logMtx
    int scanMode = 0;                       // 0 = this block, 1 = both (hop)
    float scanHang = 3.0f;                  // s after the carrier drops before resuming
    float scanDwell = 2.0f;                 // s idle on a block before hopping (both mode)
    std::vector<int> scanSkip;              // channel numbers the scanner ignores (logMtx)

    // runtime
    std::atomic<int> selected{-1};
    std::recursive_mutex logMtx;
    std::vector<LogEntry> log;        // newest first
    std::vector<int> openLog;         // per channel: index into log of the open entry, -1 none
    std::vector<bool> wasBusy;
    std::vector<double> alertUntil;
    int alertCount = 0;
    std::string lastExport;

    std::atomic<bool> scanOn{false};
    std::atomic<int> scanParked{-1};  // row the scanner is parked on, -1 none
    double scanLastActive = 0, scanIdleSince = 0, scanBlankUntil = 0;

    std::mutex uiMtx;
    std::string radioVfo;             // name of the Radio module's VFO (GUI thread refreshes)
    int listenChannel = -1;           // channel the Radio VFO currently sits on
    bool warnedNoRadio = false;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    config.setPath(core::args["root"].s() + "/frsgmrs_monitor_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new FrsGmrsMonitorModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (FrsGmrsMonitorModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
