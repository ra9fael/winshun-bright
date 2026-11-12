// bright_worker.cpp — the only thread that touches WMI, DDC and the gamma
// ramp after startup.
#include "bright_worker.h"

#include "dim_layer.h"
#include "overlay.h"
#include "winshun.h"

#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <memory>

namespace br {

namespace {

constexpr UINT kOpMessage = WM_APP + 1;
constexpr UINT kWakeMessage = WM_APP + 2; // "look at the settings again"
constexpr UINT_PTR kCalibrateTimer = 2;
constexpr int kCalibrateMs = 800;
constexpr int kCalibrateTolerance = 2; // percent
constexpr UINT_PTR kSessionTimer = 5;
constexpr UINT_PTR kEggFinishTimer = 6;
constexpr int kSessionPollMs = 3000;

constexpr wchar_t kTimerClass[] = L"WinShunBrightTimer";

// Local wall clock as minutes since midnight, for the night window.
int minutesOfDayNow()
{
    SYSTEMTIME st {};
    ::GetLocalTime(&st);
    return st.wHour * 60 + st.wMinute;
}

long long tickNowMs()
{
    return static_cast<long long>(::GetTickCount64());
}

// The soft layer paints `percent` as darkness; the display keeps its own
// brightness. Empty `simulated` records what the user asked for, so a later
// gesture steps from there rather than from an unreadable hardware value.
struct DimState {
    std::map<std::wstring, int> percent; // monitor id -> simulated brightness
};

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

void BrightWorker::setEggDiscoveredSink(EggDiscoveredSink sink)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_eggDiscoveredSink = std::move(sink);
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
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_settings = s;
        m_warmDirty = true; // a changed warmStrength must take effect at once
    }
    // The worker loop sleeps in GetMessage between messages; without a nudge
    // a settings edit would sit unread until the next timer or gesture.
    ::PostThreadMessageW(m_threadId, kWakeMessage, 0, 0);
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

    // The software dim layers live here too: one click-through window per
    // panel the hardware cannot drive.
    DimLayer dim;

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
    if (timerWnd)
        ::SetTimer(timerWnd, kSessionTimer, kSessionPollMs, nullptr);

    // Predicted state of the last write, for the DDC read-back calibration:
    // per monitor, so a gesture that changed one panel is not calibrated
    // against the others.
    std::vector<std::pair<std::wstring, int>> predicted;
    std::wstring predictedLabel;
    Rect predictedBounds; // the target panel; empty when all were changed

    Settings current;
    WarmSink sink;
    bool warmDirty = true;
    bool warmApplied = false;
    int appliedStrength = 0;
    bool lastRemote = isRemoteSession();

    // Software dimming and the persistent overlay-only mushroom.
    DimState dimState;
    std::map<std::wstring, EggTracker> eggTrackers;
    bool eggRunning = false;
    OverlayPlacement eggPlacement;
    SoftwareDim lastDimMode = SoftwareDim::Off;
    int lastMaxDark = -1;
    bool lastRemoteDim = true;
    bool sessionChanged = false;

    // Recomputes every dim layer from the settings and the simulated values.
    // Panels the hardware channel drives are left alone unless
    // softwareBrightness says "always".
    const auto refreshDim = [&](const Settings& settings) {
        if (!backend)
            return;
        // A remote session cannot reach the physical panels at all, so the
        // layer is the only working dimmer there — unless the user turned it
        // off for remote use.
        const bool remote = isRemoteSession();
        const bool remoteWanted = remote && settings.remoteDim;
        if (settings.softwareBrightness == SoftwareDim::Off && !remoteWanted) {
            dim.hideAll();
            return;
        }
        for (const MonitorInfo& m : backend->monitors()) {
            const bool wanted = remoteWanted
                || settings.softwareBrightness == SoftwareDim::Always
                || !m.adjustable;
            if (!wanted) {
                dim.hide(m.id);
                continue;
            }
            int percent = 100;
            const auto it = dimState.percent.find(m.id);
            if (it != dimState.percent.end())
                percent = it->second;
            else if (m.adjustable && m.percent >= 0)
                percent = m.percent; // start where the hardware is
            dim.show(m.id, m.bounds, dimAlpha(percent, settings.softwareMaxDark));
        }
    };

    const auto finishEgg = [&]() {
        if (!eggRunning)
            return;
        eggRunning = false;
        m_eggRunningSnapshot.store(false);
        if (timerWnd)
            ::KillTimer(timerWnd, kEggFinishTimer);
        if (overlay.valid())
            overlay.hideMushroomEgg();
    };

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
            dim.destroyAll(); // the rectangles may have moved
            dimState.percent.clear();
            refreshDim(current);
        }

        // A settings edit may have changed the dim layer or switched the egg
        // off mid-animation. A session switch also changes what it should cover.
        if (backend && (current.softwareBrightness != lastDimMode
                           || current.softwareMaxDark != lastMaxDark
                           || current.remoteDim != lastRemoteDim || sessionChanged)) {
            lastDimMode = current.softwareBrightness;
            lastMaxDark = current.softwareMaxDark;
            lastRemoteDim = current.remoteDim;
            refreshDim(current);
        }
        sessionChanged = false;
        if (!current.mushroomEgg || !current.showOverlay) {
            for (auto& [id, tracker] : eggTrackers) {
                (void)id;
                tracker.reset();
            }
            finishEgg(); // disabling the setting immediately hides the animation
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
            finishEgg();
            eggTrackers.clear();
            if (backend) {
                backend->invalidate();
                backend->monitors();
            }
            predicted.clear();
            warmDirty = true; // the new display needs its ramp too
            dim.destroyAll();
            dimState.percent.clear();
            refreshDim(current);
            continue;
        }

        if (msg.message == kWakeMessage) {
            // applySettings ran on the protocol thread: re-read the settings
            // and re-decide the dim layers (the branch below does the rest).
            continue;
        }

        // A session switch (RDP connect/disconnect, console takeover) changes
        // which physical panels this process can reach. The stale backend's
        // handles and monitor list survive the switch, so a gesture kept
        // hitting the dead RDP-era list and reporting "unsupported". A cheap
        // poll notices the change and resets everything.
        if (msg.message == WM_TIMER && msg.hwnd == timerWnd && msg.wParam == kSessionTimer) {
            const bool remote = isRemoteSession();
            if (remote != lastRemote) {
                finishEgg();
                eggTrackers.clear();
                lastRemote = remote;
                sessionChanged = true; // the dim layers must re-decide too
                winshun::debug("WinShunBright: session switched, remote="
                    + std::to_string(remote ? 1 : 0) + "; rebuilding the backend");
                backend = createBackend();
                if (!backend || !backend->init())
                    winshun::debug("WinShunBright: display backend unavailable after the session switch");
                else
                    backend->monitors();
                predicted.clear();
                dimState.percent.clear();
                dim.destroyAll();
                warmDirty = true;
                warmApplied = false; // the old ramps are gone with the session
                refreshDim(current);
            }
            continue;
        }

        if (msg.message == WM_TIMER && msg.hwnd == timerWnd && msg.wParam == kCalibrateTimer) {
            ::KillTimer(timerWnd, kCalibrateTimer);
            if (eggRunning) {
                predicted.clear();
                continue;
            }
            if (!backend || predicted.empty())
                continue;
            backend->refreshValues();
            // Compare only the panels this write touched: the others were
            // read at the same instant but not written, and DDC read-back is
            // noisy on some monitors.
            std::vector<int> written;
            std::vector<int> actual;
            for (const auto& [id, value] : predicted) {
                for (const MonitorInfo& m : backend->monitors()) {
                    if (m.id != id)
                        continue;
                    if (m.adjustable && m.percent >= 0) {
                        written.push_back(value);
                        actual.push_back(m.percent);
                    }
                    break;
                }
            }
            const int predictedAvg = averagePercent(written);
            const int actualAvg = averagePercent(actual);
            if (predictedAvg >= 0 && actualAvg >= 0
                && std::abs(actualAvg - predictedAvg) > kCalibrateTolerance
                && current.showOverlay && overlay.valid()) {
                const OverlayPlacement placement {
                    current.overlayAnchor, current.overlayOffsetX, current.overlayOffsetY,
                    predictedBounds
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
        // Once the egg begins, user adjustments are ignored until StopEgg. The
        // trigger wheel itself is also consumed without changing brightness.
        if (eggRunning && op->kind != BrightOp::Kind::StopEgg) {
            if (current.gestureDebug)
                winshun::debug("WinShunBright: ignored brightness operation while the easter egg is active");
            continue;
        }
        if (!backend) {
            winshun::debug("WinShunBright: dropping op, display backend unavailable");
            continue; // the unique_ptr frees the op
        }

        std::wstring message;
        bool hasValue = false;
        int value = 0;
        std::wstring label;
        Rect overlayBounds; // the area the panel should sit in, when known
        if (op->kind == BrightOp::Kind::Wheel) {
            const std::vector<MonitorInfo> eggMonitors = backend->monitors();
            const int eggMonitorIndex = monitorIndexAtPoint(eggMonitors, op->x, op->y, false);
            if (eggMonitorIndex >= 0) {
                const MonitorInfo& eggMonitor = eggMonitors[static_cast<std::size_t>(eggMonitorIndex)];
                EggTracker::Context context;
                context.eggEnabled = current.mushroomEgg && current.showOverlay;
                context.sameMonitor = true;
                EggTracker& tracker = eggTrackers[eggMonitor.id];
                const long long eventTime = op->eventTimeMs > 0 ? op->eventTimeMs : tickNowMs();
                if (!eggRunning && tracker.feed(op->steps, context, eventTime)) {
                    // Capture the monitor rectangle at trigger time. The egg
                    // window uses this rectangle's center, not the cursor.
                    eggPlacement = OverlayPlacement {
                        OverlayAnchor::Center, 0, 0, eggMonitor.bounds
                    };
                    if (overlay.valid()) {
                        overlay.showMushroomEgg(eggPlacement, kEggEffectMs);
                        eggRunning = overlay.mushroomEggVisible();
                        m_eggRunningSnapshot.store(eggRunning);
                    }
                    if (eggRunning) {
                        EggDiscoveredSink discoveredSink;
                        {
                            std::lock_guard<std::mutex> lock(m_mutex);
                            discoveredSink = m_eggDiscoveredSink;
                        }
                        if (discoveredSink)
                            discoveredSink();
                        winshun::debug("WinShunBright: easter egg starts on " + winshun::utf8(eggMonitor.id));
                        // The wheel that completed the trigger only starts the
                        // egg; it must not also adjust brightness.
                        continue;
                    }
                }
            }
        }

        switch (op->kind) {
        case BrightOp::Kind::Wheel:
        case BrightOp::Kind::SetAll: {
            const bool wheel = op->kind == BrightOp::Kind::Wheel;
            std::vector<int> values;
            predicted.clear();
            // A copy: setPercent() may rebuild the cache.
            const std::vector<MonitorInfo> monitors = backend->monitors();
            // Which displays this op changes: everything, or only the one the
            // pointer was over when the gesture happened. The panel is picked
            // without the adjustable hint, so a display that was asleep at
            // start-up still gets its chance (the backend re-probes the DDC
            // handle on the way).
            std::vector<MonitorInfo> targets;
            if (op->target == BrightOp::Target::All) {
                for (const MonitorInfo& m : monitors) {
                    if (m.adjustable)
                        targets.push_back(m);
                }
            } else if (!monitors.empty()) {
                const int index = monitorIndexAtPoint(monitors, op->x, op->y, false);
                if (index >= 0)
                    targets.push_back(monitors[static_cast<std::size_t>(index)]);
            }
            std::wstring failure; // the reason for a target that could not be changed

            // By id: a rebuild inside setPercent()/readPercent() may reorder the
            // cache the loop started from.
            for (const MonitorInfo& target : targets) {
                MonitorInfo m = target;
                // Software dimming: the panel the hardware cannot drive, or any
                // panel when the setting says so, is simulated by the layer and
                // its value lives in dimState instead of the display. A remote
                // session follows remoteDim: its panels never answer the
                // hardware, so the layer is the only working dimmer there.
                const bool remoteDimWanted = isRemoteSession() && current.remoteDim;
                const bool software = remoteDimWanted
                    || current.softwareBrightness == SoftwareDim::Always
                    || (current.softwareBrightness == SoftwareDim::UnsupportedOnly && !m.adjustable);
                if (software) {
                    if (op->kind == BrightOp::Kind::SetAll) {
                        dimState.percent[m.id] = clampPercent(op->percent);
                    } else {
                        int base = 100;
                        const auto it = dimState.percent.find(m.id);
                        if (it != dimState.percent.end())
                            base = it->second;
                        else if (m.adjustable && m.percent >= 0)
                            base = m.percent;
                        dimState.percent[m.id] = steppedPercent(base, op->steps, current.percentPerStep);
                    }
                    const int percent = dimState.percent[m.id];
                    dim.show(m.id, m.bounds, dimAlpha(percent, current.softwareMaxDark));
                    if (current.gestureDebug) {
                        winshun::debug("WinShunBright: soft dim " + winshun::utf8(m.id)
                            + " -> " + std::to_string(percent) + "% alpha="
                            + std::to_string(dimAlpha(percent, current.softwareMaxDark))
                            + " layers=" + std::to_string(dim.count()));
                    }
                    values.push_back(percent);
                    continue;
                }

                if (m.percent < 0)
                    m.percent = backend->readPercent(m.id).value_or(-1);
                int next = op->percent;
                if (wheel) {
                    if (m.percent < 0) {
                        failure = backend->lastError();
                        continue;
                    }
                    next = steppedPercent(m.percent, op->steps, current.percentPerStep);
                }
                int applied = next;
                if (backend->setPercent(m.id, next, applied)) {
                    values.push_back(applied);
                    predicted.emplace_back(m.id, applied);

                } else {
                    failure = backend->lastError();
                    if (wheel)
                        predicted.emplace_back(m.id, m.percent < 0 ? 0 : m.percent);
                    if (current.gestureDebug) {
                        winshun::debug("WinShunBright: set failed on " + winshun::utf8(m.id)
                            + " (" + winshun::utf8(backend->lastError()) + ")");
                    }
                }
            }
            if (values.empty()) {
                // Nothing changed: say why instead of silently doing nothing.
                // A panel that cannot be adjusted at all (a TV through a
                // receiver, a KVM) and one that is merely asleep look the same
                // from here; the DDC re-probe in the backend decides next time.
                const bool noTarget = op->target == BrightOp::Target::UnderMouse
                    && (targets.empty()
                        || (!targets.front().adjustable
                            && current.softwareBrightness == SoftwareDim::Off));
                if (isRemoteSession() && (noTarget || targets.empty()))
                    message = current.chinese
                        ? L"远程桌面会话调不了物理屏亮度"
                        : L"A remote session cannot change the physical brightness";
                else if (noTarget && !monitors.empty())
                    message = current.chinese ? L"这块显示器不支持亮度调节"
                                              : L"This display cannot change its brightness";
                else if (!failure.empty())
                    message = (current.chinese ? L"调整亮度失败：" : L"Failed to change the brightness: ")
                        + failure;
                else
                    message = current.chinese ? L"没有可调节的显示器" : L"No adjustable display";
                break;
            }
            hasValue = true;
            value = averagePercent(values);
            if (op->target == BrightOp::Target::All)
                label = current.chinese ? L"全部显示器" : L"All displays";
            else
                label = targets.front().name;
            if (op->target == BrightOp::Target::UnderMouse)
                overlayBounds = targets.front().bounds;
            predictedLabel = label;
            predictedBounds = overlayBounds;
            if (timerWnd)
                ::SetTimer(timerWnd, kCalibrateTimer, kCalibrateMs, nullptr);
            break;
        }        case BrightOp::Kind::ToggleWarm: {
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
        case BrightOp::Kind::StopEgg:
            finishEgg();
            break;
        case BrightOp::Kind::PlayEgg: {
            if (!current.mushroomEgg || !overlay.valid())
                break;
            const std::vector<MonitorInfo> monitors = backend->monitors();
            const int index = monitorIndexAtPoint(monitors, op->x, op->y, false);
            if (index >= 0 && !eggRunning) {
                eggPlacement = OverlayPlacement {
                    OverlayAnchor::Center, 0, 0,
                    monitors[static_cast<std::size_t>(index)].bounds
                };
                overlay.showMushroomEgg(eggPlacement, kEggEffectMs);
                eggRunning = overlay.mushroomEggVisible();
                m_eggRunningSnapshot.store(eggRunning);
            }
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

    // The egg lives in its own window, so a value or message panel shown here
    // (including the 800 ms read-back calibration) cannot replace it.
    if (current.showOverlay && overlay.valid() && hasValue) {
        const OverlayPlacement placement {
            current.overlayAnchor, current.overlayOffsetX, current.overlayOffsetY, overlayBounds
        };
        overlay.showValue(label, value, placement, current.overlayMs);
    } else if (current.showOverlay && overlay.valid() && !message.empty()) {
        const OverlayPlacement placement {
            current.overlayAnchor, current.overlayOffsetX, current.overlayOffsetY, overlayBounds
        };
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
    dim.destroyAll(); // never leave a dim layer behind
    overlay.destroy();
    if (backend)
        backend->restoreWarm(); // the session may end with the filter on
    m_running.store(false);
}

} // namespace br
