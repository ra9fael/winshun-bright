// bright_worker.h — a message-loop thread that owns the display backend
// (WMI / DDC / gamma) and the overlay window. The hook never blocks on a
// display call: it posts ops here and returns.
#pragma once

#include "bright_logic.h"
#include "display_controller.h"
#include "settings.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace br {

// One queued action for the worker.
struct BrightOp {
    enum class Kind { None, Wheel, ToggleWarm, SetAll, BlockedReason };
    Kind kind = Kind::None;
    int steps = 0;      // Wheel
    int percent = 0;    // SetAll (already clamped by the caller)
    std::string reason; // BlockedReason: stable key
};

class BrightWorker {
public:
    // Runs on the worker thread when the warm filter was turned on or off by
    // a gesture; the plugin persists it when warmRemember is set.
    using WarmSink = std::function<void(bool on)>;

    BrightWorker() = default;
    ~BrightWorker() { stop(); }
    BrightWorker(const BrightWorker&) = delete;
    BrightWorker& operator=(const BrightWorker&) = delete;

    bool start();
    void stop();
    bool running() const { return m_running.load(); }

    void setWarmSink(WarmSink sink);

    // Thread-safe; the op is heap-copied and delivered via thread message.
    void post(BrightOp op);
    // Thread-safe settings update (step size, overlay, warm strength).
    void applySettings(const Settings& s);
    // Thread-safe: the wanted on/off state of the warm filter (restored from
    // state.json at startup, flipped by the gesture afterwards).
    void setWarmEnabled(bool on);
    // Thread-safe: the display list changed (WM_DISPLAYCHANGE, hot-plug).
    void invalidateDisplays();

private:
    void run();

    std::thread m_thread;
    std::atomic<bool> m_running { false };
    DWORD m_threadId = 0;
    std::mutex m_mutex;
    Settings m_settings;
    bool m_warmWanted = false;
    bool m_warmDirty = false;
    bool m_displaysDirty = false;
    WarmSink m_warmSink;
    // Handshake so the first post() cannot race the worker's message queue.
    std::mutex m_startMutex;
    std::condition_variable m_startCv;
    bool m_ready = false;
};

} // namespace br
