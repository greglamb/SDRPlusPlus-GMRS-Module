// Paced null audio sink for headless SDR++ test runs.
//
// SDR++ relies on the audio device to pace the whole pipeline; with no audio
// sink a File source plays as fast as the CPU allows. This sink discards the
// audio but sleeps so that samples are consumed at the stream's sample rate,
// which makes File-source replays run in real time without any audio hardware.
#include <imgui.h>
#include <core.h>
#include <module.h>
#include <signal_path/signal_path.h>
#include <signal_path/sink.h>
#include <utils/flog.h>

#include <atomic>
#include <chrono>
#include <thread>

SDRPP_MOD_INFO{
    /* Name:            */ "paced_null_sink",
    /* Description:     */ "Real-time paced null audio sink (for headless replay tests)",
    /* Author:          */ "Greg Lamb",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

class PacedNullSink : SinkManager::Sink {
public:
    PacedNullSink(SinkManager::Stream* stream, std::string streamName) : _stream(stream), _name(streamName) {
        _stream->setSampleRate(48000.0);
    }
    ~PacedNullSink() { stop(); }

    void start() {
        if (_running) { return; }
        _running = true;
        _thread = std::thread(&PacedNullSink::worker, this);
    }
    void stop() {
        if (!_running) { return; }
        _running = false;
        _stream->sinkOut->stopReader();
        if (_thread.joinable()) { _thread.join(); }
        _stream->sinkOut->clearReadStop();
    }
    void menuHandler() { ImGui::TextUnformatted("Paced null sink (48 kHz, real-time)"); }

private:
    void worker() {
        using clock = std::chrono::steady_clock;
        auto next = clock::now();
        while (_running) {
            int count = _stream->sinkOut->read();
            if (count < 0) { break; }
            _stream->sinkOut->flush();
            next += std::chrono::microseconds((long long)(1e6 * count / 48000.0));
            std::this_thread::sleep_until(next);
            if (clock::now() - next > std::chrono::milliseconds(500)) { next = clock::now(); }  // don't run away after a stall
        }
    }

    SinkManager::Stream* _stream;
    std::string _name;
    std::atomic<bool> _running{false};
    std::thread _thread;
};

class PacedNullSinkModule : public ModuleManager::Instance {
public:
    PacedNullSinkModule(std::string name) : name(name) {
        provider.create = create_sink;
        provider.ctx = this;
        sigpath::sinkManager.registerSinkProvider("Paced Null", provider);
    }
    ~PacedNullSinkModule() { sigpath::sinkManager.unregisterSinkProvider("Paced Null"); }
    void postInit() {}
    void enable() { enabled = true; }
    void disable() { enabled = false; }
    bool isEnabled() { return enabled; }
private:
    static SinkManager::Sink* create_sink(SinkManager::Stream* stream, std::string streamName, void* ctx) {
        return (SinkManager::Sink*)(new PacedNullSink(stream, streamName));
    }
    std::string name;
    bool enabled = true;
    SinkManager::SinkProvider provider;
};

MOD_EXPORT void _INIT_() {}
MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) { return new PacedNullSinkModule(name); }
MOD_EXPORT void _DELETE_INSTANCE_(void* instance) { delete (PacedNullSinkModule*)instance; }
MOD_EXPORT void _END_() {}
