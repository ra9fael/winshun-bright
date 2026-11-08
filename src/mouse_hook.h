// mouse_hook.h — the WH_MOUSE_LL hook on its own message-pumping thread.
//
// Contract:
//  * The callback does O(µs) work only: read the point, check modifiers,
//    decide. WMI, DDC and gamma are NEVER called here; actions are handed to
//    a sink that posts to the worker.
//  * Only Ctrl+Alt+wheel and Ctrl+Alt+left-click are intercepted; everything
//    else — injected input, Ctrl-only (the volume plugin's gesture), Alt-only,
//    Shift/Win combos, plain clicks — passes through untouched.
//  * Partial high-precision wheel deltas are passed through immediately and
//    accumulated; the brightness changes one step per full notch.
//  * A recognized left-down remembers that its up must be swallowed too, so
//    the desktop never sees a lone button-up.
#pragma once

#include "bright_logic.h"

#include <windows.h>

#include <atomic>
#include <functional>
#include <thread>

namespace br {

class MouseHook {
public:
    // Runs on the hook thread but must only enqueue (fast).
    using Sink = std::function<void(const GestureDecision&)>;

    MouseHook() = default;
    ~MouseHook() { stop(); }
    MouseHook(const MouseHook&) = delete;
    MouseHook& operator=(const MouseHook&) = delete;

    bool start(Sink sink);
    void stop();
    bool active() const { return m_running.load(); }

private:
    void run();
    static LRESULT CALLBACK hookProcStatic(int code, WPARAM wp, LPARAM lp);
    LRESULT hookProc(int code, WPARAM wp, LPARAM lp);
    // Cheap gate before building a GestureEvent.
    static bool fastGate(const MSLLHOOKSTRUCT* m);

    std::thread m_thread;
    std::atomic<bool> m_running { false };
    DWORD m_threadId = 0;
    Sink m_sink;

    // Hook-thread-only state.
    WheelAccumulator m_accum;
    bool m_pendingClick = false;
};

} // namespace br
