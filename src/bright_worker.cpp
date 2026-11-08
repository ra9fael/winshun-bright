// bright_worker.cpp — the only thread that touches WMI, DDC and the gamma
// ramp after startup.
#include "bright_worker.h"

#include "overlay.h"
#include "winshun.h"

#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <memory>

namespace br {

namespace {

constexpr UINT kOpMessage = WM_APP + 1;
constexpr UINT_PTR kCalibrateTimer = 2;
constexpr int kCalibrateMs = 800;
constexpr int kCalibrateTolerance = 2; // percent

constexpr wchar_t kTimerClass[] = L"WinShunBrightTimer";

} // namespace

bool BrightWorker::start()
{
    if (m_running.load())
        return true;
    if (m_thread.joinable())
        m_thread.join(); // a previous run ended on its own
    {
        std::lock_guard<std::mutex> lock(m_startMutex);
        m_ready = false;
    }
    m_running.store(true);
    try {
        m_thread = std::thread([this] { run(); });
    } catch (...) {
        m_running.store(false);
        return false;
    }
    // Wait until the worker owns a message queue, so the very first post()
    // cannot be dropped by PostThreadMessageW.
    std::unique_lock<std::mutex> lock(m_startMutex);
    m_startCv.wait_for(lock, std::chrono::milliseconds(500), [this] { return m_ready; });
    return true;
}

void BrightWorker::stop()
{
    if (m_thread.joinable()) {
        m_running.store(false);
        ::PostThreadMessageW(m_threadId, WM_QUIT, 0, 0);
        m_thread.join();
    }
}

void BrightWorker::setWarmSink(WarmSink sink)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_warmSink = std::move(sink);
}

void BrightWorker::post(BrightOp op)
{
    if (!m_running.load())
        return;
    auto* copy = new BrightOp(std::move(op));
    if (!::PostThreadMessageW(m_threadId, kOpMessage, 0, reinterpret_cast<LPARAM>(copy)))
        delete copy; // the thread died before its queue existed
}

void BrightWorker::applySettings(const Settings& s)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_settings = s;
    m_warmDirty = true; // a changed warmStrength must take effect at once
}

void BrightWorker::setWarmEnabled(bool on)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_warmWanted != on) {
        m_warmWanted = on;
        m_warmDirty = true;
    }
}

void BrightWorker::invalidateDisplays()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_displaysDirty = true;
}

void BrightWorker::run()
{
    m_threadId = ::GetCurrentThreadId();
    MSG msg;
    // Force the thread message queue into existence before signaling ready.
    ::PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    {
        std::lock_guard<std::mutex> lock(m_startMutex);
        m_ready = true;
    }
    m_startCv.notify_all();

    std::unique_ptr<IDisplayBackend> backend = createBackend();
    if (!backend || !backend->init())
        winshun::debug("WinShunBright: display backend unavailable on the worker thread");
    if (backend)
        backend->monitors(); // warm the cache once

    // The overlay is on this thread, so its WM_PAINT and hide timer reach it.
    Overlay overlay;
    if (!overlay.create())
        winshun::debug("WinShunBright: overlay window creation failed");

    // A zero-size, never-shown top-level window: SetTimer needs a receiver,
    // and WM_DISPLAYCHANGE is broadcast only to top-level windows (a
    // HWND_MESSAGE window would never see a hot-plug).
    WNDCLASSW wc = {};
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = kTimerClass;
    ::RegisterClassW(&wc); // ERROR_CLASS_ALREADY_EXISTS is fine
    HWND timerWnd = ::CreateWindowExW(0, kTimerClass, L"", WS_POPUP,
        0, 0, 0, 0, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);

    // Predicted state of the last write, for the DDC read-back calibration.
    std::vector<int> predicted;
    std::wstring predictedLabel;

    Settings current;
    WarmSink sink;
    bool warmDirty = true;
    bool warmApplied = false;
    int appliedStrength = 0;

    while (m_running.load()) {
        const BOOL got = ::GetMessageW(&msg, nullptr, 0, 0);
        if (got <= 0)
            break;

        bool displaysDirty = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            current = m_settings;
            sink = m_warmSink;
            displaysDirty = m_displaysDirty;
            m_displaysDirty = false;
            warmDirty = warmDirty || m_warmDirty;
            m_warmDirty = false;
        }

        if (displaysDirty && backend) {
            backend->invalidate();
            backend->monitors();
            warmDirty = true; // a new display needs its ramp too
        }

        // Warm filter: desired state, re-evaluated after a settings change,
        // a toggle op, or a display change.
        if (warmDirty && backend) {
            bool wanted = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                wanted = m_warmWanted && m_settings.warmStrength > 0;
            }
            if (wanted && (!warmApplied || appliedStrength != current.warmStrength)) {
                warmApplied = backend->setWarm(current.warmStrength);
                appliedStrength = warmApplied ? current.warmStrength : 0;
            } else if (!wanted && warmApplied) {
                backend->restoreWarm();
                warmApplied = false;
                appliedStrength = 0;
            }
            warmDirty = false;
        }

        if (msg.message == WM_DISPLAYCHANGE) {
            if (backend) {
                backend->invalidate();
                backend->monitors();
            }
            predicted.clear();
            warmDirty = true; // the new display needs its ramp too
            continue;
        }

        if (msg.message == WM_TIMER && msg.hwnd == timerWnd && msg.wParam == kCalibrateTimer) {
            ::KillTimer(timerWnd, kCalibrateTimer);
            if (!backend || predicted.empty())
                continue;
            backend->refreshValues();
            std::vector<int> actual;
            for (const MonitorInfo& m : backend->monitors()) {
                if (m.adjustable && m.percent >= 0)
                    actual.push_back(m.percent);
            }
            const int predictedAvg = averagePercent(predicted);
            const int actualAvg = averagePercent(actual);
            if (predictedAvg >= 0 && actualAvg >= 0
                && std::abs(actualAvg - predictedAvg) > kCalibrateTolerance
                && current.showOverlay && overlay.valid()) {
                const OverlayPlacement placement {
                    current.overlayAnchor, current.overlayOffsetX, current.overlayOffsetY
                };
                overlay.showValue(predictedLabel, actualAvg, placement, current.overlayMs);
            }
            continue;
        }

        if (msg.message != kOpMessage || !msg.lParam) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
            continue;
        }

        std::unique_ptr<BrightOp> op(reinterpret_cast<BrightOp*>(msg.lParam));
        if (!backend) {
            winshun::debug("WinShunBright: dropping op, display backend unavailable");
            continue; // the unique_ptr frees the op
        }

        std::wstring message;
        bool hasValue = false;
        int value = 0;
        std::wstring label;

        switch (op->kind) {
        case BrightOp::Kind::Wheel:
        case BrightOp::Kind::SetAll: {
            const bool wheel = op->kind == BrightOp::Kind::Wheel;
            std::vector<int> values;
            predicted.clear();
            // A copy: setPercent() may rebuild the cache.
            const std::vector<MonitorInfo> monitors = backend->monitors();
            for (const MonitorInfo& m : monitors) {
                if (!m.adjustable)
                    continue;
                int next = op->percent;
                if (wheel) {
                    if (m.percent < 0)
                        continue;
                    next = steppedPercent(m.percent, op->steps, current.percentPerStep);
                }
                int applied = next;
                if (backend->setPercent(m.id, next, applied)) {
                    values.push_back(applied);
                    predicted.push_back(applied);
                } else if (wheel) {
                    predicted.push_back(m.percent);
                }
            }
            if (values.empty()) {
                message = current.chinese ? L"没有可调节的显示器" : L"No adjustable display";
                break;
            }
            hasValue = true;
            value = averagePercent(values);
            label = current.chinese ? L"全部显示器" : L"All displays";
            predictedLabel = label;
            if (timerWnd)
                ::SetTimer(timerWnd, kCalibrateTimer, kCalibrateMs, nullptr);
            break;
        }
        case BrightOp::Kind::ToggleWarm: {
            if (current.warmStrength <= 0) {
                message = current.chinese
                    ? L"暖色滤镜强度为 0，请先在设置里调高 warmStrength"
                    : L"Warm filter strength is 0; raise warmStrength first";
                break;
            }
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_warmWanted = !warmApplied;
            }
            if (warmApplied) {
                backend->restoreWarm();
                warmApplied = false;
                appliedStrength = 0;
                message = current.chinese ? L"暖色滤镜 · 关" : L"Warm filter · off";
            } else {
                warmApplied = backend->setWarm(current.warmStrength);
                appliedStrength = warmApplied ? current.warmStrength : 0;
                message = warmApplied
                    ? (current.chinese ? L"暖色滤镜 · 开" : L"Warm filter · on")
                    : (current.chinese ? L"无法应用暖色滤镜" : L"Cannot apply the warm filter");
            }
            if (sink)
                sink(warmApplied);
            break;
        }
        case BrightOp::Kind::BlockedReason:
            message = op->reason == "no-display"
                ? (current.chinese ? L"没有可调节的显示器" : L"No adjustable display")
                : (current.chinese ? L"该位置无法操作" : L"Nothing to control here");
            break;
        case BrightOp::Kind::None:
            break;
        }

        if (current.showOverlay && overlay.valid()) {
            const OverlayPlacement placement {
                current.overlayAnchor, current.overlayOffsetX, current.overlayOffsetY
            };
            if (hasValue)
                overlay.showValue(label, value, placement, current.overlayMs);
            else if (!message.empty())
                overlay.show(message, placement, current.overlayMs);
        }
        if (!message.empty())
            winshun::debug("WinShunBright: " + winshun::utf8(message));
    }

    // WM_QUIT leaves queued ops behind; free them instead of leaking the
    // heap copies posted by the hook thread.
    while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == kOpMessage && msg.lParam)
            delete reinterpret_cast<BrightOp*>(msg.lParam);
    }

    if (timerWnd)
        ::DestroyWindow(timerWnd);
    overlay.destroy();
    if (backend)
        backend->restoreWarm(); // the session may end with the filter on
    m_running.store(false);
}

} // namespace br
