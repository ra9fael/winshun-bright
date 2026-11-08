// mouse_hook.cpp — low-level mouse hook implementation.
#include "mouse_hook.h"

#include "winshun.h"

namespace br {

namespace {
MouseHook* g_hook = nullptr; // exactly one instance per process
} // namespace

bool MouseHook::start(Sink sink)
{
    if (m_running.load())
        return true;
    if (m_thread.joinable())
        m_thread.join(); // a previous run ended on its own
    m_sink = std::move(sink);
    m_running.store(true);
    try {
        m_thread = std::thread([this] { run(); });
    } catch (...) {
        m_running.store(false);
        return false;
    }
    return true;
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
        while (m_running.load()) {
            const BOOL got = ::GetMessageW(&msg, nullptr, 0, 0);
            if (got <= 0)
                break;
        }
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

bool MouseHook::fastGate(const MSLLHOOKSTRUCT* m)
{
    if (!m || (m->flags & (LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED)))
        return false; // simulated input is never ours
    if ((::GetAsyncKeyState(VK_CONTROL) & 0x8000) == 0)
        return false;
    if ((::GetAsyncKeyState(VK_MENU) & 0x8000) == 0)
        return false; // Ctrl-only belongs to the volume plugin
    if ((::GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 || (::GetAsyncKeyState(VK_RWIN) & 0x8000) != 0)
        return false;
    if ((::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0)
        return false;
    return true;
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
        e.ctrl = true;
        e.alt = true;
        e.wheel = true;
        e.wheelDelta = steps * kWheelDelta;
        GestureDecision d = decideGesture(e);
        if (!d.swallow)
            return 0;
        d.steps = steps; // a burst delta may carry more than one notch
        if (m_sink)
            m_sink(d);
        return 1;
    }
    case WM_LBUTTONDOWN: {
        m_accum.reset();
        // A new down means any earlier up never reached us; drop the stale
        // pairing so this click cannot inherit it.
        m_pendingClick = false;
        if (!fastGate(m))
            return 0;

        GestureEvent e;
        e.ctrl = true;
        e.alt = true;
        e.leftDown = true;
        const GestureDecision d = decideGesture(e);
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
