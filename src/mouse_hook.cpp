// mouse_hook.cpp — low-level mouse hook implementation.
#include "mouse_hook.h"

#include "winshun.h"

namespace br {

namespace {
MouseHook* g_hook = nullptr; // exactly one instance per process

unsigned chordMaskOf(const GestureModifiers& chord)
{
    unsigned mask = 0;
    if (chord.ctrl)
        mask |= 1u << 0;
    if (chord.alt)
        mask |= 1u << 1;
    if (chord.shift)
        mask |= 1u << 2;
    if (chord.win)
        mask |= 1u << 3;
    return mask;
}

GestureModifiers chordOf(unsigned mask)
{
    GestureModifiers chord;
    chord.ctrl = (mask & (1u << 0)) != 0;
    chord.alt = (mask & (1u << 1)) != 0;
    chord.shift = (mask & (1u << 2)) != 0;
    chord.win = (mask & (1u << 3)) != 0;
    return chord;
}

constexpr UINT_PTR kReinstallTimer = 3;
constexpr UINT kReinstallMs = 10000;

bool keyDown(int vk)
{
    return (::GetAsyncKeyState(vk) & 0x8000) != 0;
}

} // namespace

bool MouseHook::start(Sink sink, GestureModifiers chord)
{
    if (m_running.load())
        return true;
    if (m_thread.joinable())
        m_thread.join(); // a previous run ended on its own
    m_sink = std::move(sink);
    m_chordMask.store(chordMaskOf(chord));
    m_running.store(true);
    try {
        m_thread = std::thread([this] { run(); });
    } catch (...) {
        m_running.store(false);
        return false;
    }
    return true;
}

void MouseHook::setChord(GestureModifiers chord)
{
    m_chordMask.store(chordMaskOf(chord));
}

void MouseHook::stop()
{
    if (m_thread.joinable()) {
        m_running.store(false);
        ::PostThreadMessageW(m_threadId, WM_QUIT, 0, 0);
        m_thread.join();
    }
}

void MouseHook::run()
{
    m_threadId = ::GetCurrentThreadId();
    // Force the thread message queue into existence so stop() can post to it.
    MSG msg;
    ::PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    // Hook-thread-only state; reset here instead of in stop() so the stopping
    // thread never touches it while the hook is running.
    m_accum.reset();
    m_pendingClick = false;
    g_hook = this;

    // The installing thread must pump messages for the hook to fire. A failed
    // installation is retried briefly and then reported instead of leaving the
    // plugin silently gesture-less.
    HHOOK hook = nullptr;
    for (int attempt = 0; attempt < 3 && m_running.load() && !hook; ++attempt) {
        if (attempt > 0)
            ::Sleep(300);
        hook = ::SetWindowsHookExW(WH_MOUSE_LL, &MouseHook::hookProcStatic,
            ::GetModuleHandleW(nullptr), 0);
    }
    if (!hook) {
        winshun::debug("WinShunBright: SetWindowsHookExW(WH_MOUSE_LL) failed");
    } else {
        // The system drops a low-level hook whose callback takes too long and
        // never tells us: the gestures would stop working until the process
        // restarted. Re-installing it on a timer is the only reliable health
        // check — it is cheap and idempotent for a healthy hook.
        ::SetTimer(nullptr, kReinstallTimer, kReinstallMs, nullptr);
        while (m_running.load()) {
            const BOOL got = ::GetMessageW(&msg, nullptr, 0, 0);
            if (got <= 0)
                break;
            if (msg.message == WM_TIMER && msg.wParam == kReinstallTimer) {
                ::UnhookWindowsHookEx(hook);
                hook = ::SetWindowsHookExW(WH_MOUSE_LL, &MouseHook::hookProcStatic,
                    ::GetModuleHandleW(nullptr), 0);
                if (!hook) {
                    winshun::debug("WinShunBright: the mouse hook is gone and could not be re-installed");
                    break; // the loop ends, m_running says the gesture is off
                }
            }
        }
        ::KillTimer(nullptr, kReinstallTimer);
        if (hook)
            ::UnhookWindowsHookEx(hook);
    }
    g_hook = nullptr;
    m_running.store(false);
}

LRESULT CALLBACK MouseHook::hookProcStatic(int code, WPARAM wp, LPARAM lp)
{
    if (g_hook && code >= 0) {
        const LRESULT handled = g_hook->hookProc(code, wp, lp);
        if (handled)
            return handled; // swallowed
    }
    return ::CallNextHookEx(nullptr, code, wp, lp);
}

bool MouseHook::fastGate(const MSLLHOOKSTRUCT* m) const
{
    if (!m || (m->flags & (LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED)))
        return false; // simulated input is never ours
    const GestureModifiers chord = chordOf(m_chordMask.load());
    return chord.matches(keyDown(VK_CONTROL), keyDown(VK_MENU), keyDown(VK_SHIFT),
        keyDown(VK_LWIN) || keyDown(VK_RWIN));
}

LRESULT MouseHook::hookProc(int /*code*/, WPARAM wp, LPARAM lp)
{
    const MSLLHOOKSTRUCT* m = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
    if (!m)
        return 0;

    switch (wp) {
    case WM_MOUSEWHEEL: {
        if (!fastGate(m))
            return 0; // pass plain/injected/modifier wheels through instantly
        const int raw = static_cast<short>(HIWORD(m->mouseData));
        const int steps = m_accum.add(raw);
        if (steps == 0)
            return 0; // partial high-res delta: pass through and keep counting

        GestureEvent e;
        e.ctrl = keyDown(VK_CONTROL);
        e.alt = keyDown(VK_MENU);
        e.shift = keyDown(VK_SHIFT);
        e.win = keyDown(VK_LWIN) || keyDown(VK_RWIN);
        e.wheel = true;
        e.wheelDelta = steps * kWheelDelta;
        e.x = m->pt.x;
        e.y = m->pt.y;
        e.eventTimeMs = static_cast<long long>(::GetTickCount64());
        GestureDecision d = decideGesture(e, chordOf(m_chordMask.load()));
        if (!d.swallow)
            return 0;
        d.steps = steps; // a burst delta may carry more than one notch
        if (m_sink)
            m_sink(d);
        return 1;
    }
    case WM_LBUTTONDOWN: {
        // Notify the plugin about every real physical click, even without the
        // configured brightness chord; decideGesture marks it non-swallowing.
        GestureEvent physical;
        physical.leftDown = true;
        physical.x = m->pt.x;
        physical.y = m->pt.y;
        const GestureDecision stop = decideGesture(physical, GestureModifiers::ctrlAlt());
        if (stop.stopEgg && m_sink)
            m_sink(stop);
        m_accum.reset();
        // A new down means any earlier up never reached us; drop the stale
        // pairing so this click cannot inherit it.
        m_pendingClick = false;
        if (!fastGate(m))
            return 0;

        GestureEvent e;
        e.ctrl = keyDown(VK_CONTROL);
        e.alt = keyDown(VK_MENU);
        e.shift = keyDown(VK_SHIFT);
        e.win = keyDown(VK_LWIN) || keyDown(VK_RWIN);
        e.leftDown = true;
        e.x = m->pt.x;
        e.y = m->pt.y;
        const GestureDecision d = decideGesture(e, chordOf(m_chordMask.load()));
        if (!d.swallow)
            return 0;
        m_pendingClick = true;
        if (m_sink)
            m_sink(d);
        return 1;
    }
    case WM_LBUTTONUP: {
        if (!m_pendingClick)
            return 0; // no recognized down: native behavior
        m_pendingClick = false;
        return 1; // swallow the matching up
    }
    default:
        return 0;
    }
}

} // namespace br
