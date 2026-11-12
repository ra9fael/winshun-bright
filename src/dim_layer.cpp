// dim_layer.cpp — see dim_layer.h.
#include "dim_layer.h"

#include "winshun.h"

#include <algorithm>

namespace br {

namespace {

constexpr wchar_t kClassName[] = L"WinShunBrightDim";
constexpr wchar_t kColourProp[] = L"WinShunBrightDimColour";

} // namespace

void DimLayer::registerClassOnce()
{
    static const bool done = [] {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = &DimLayer::procStatic;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = nullptr; // never sees the pointer
        wc.hbrBackground = nullptr; // painted by the layered colour
        wc.lpszClassName = kClassName;
        return ::RegisterClassW(&wc) != 0; // ERROR_CLASS_ALREADY_EXISTS is fine
    }();
    (void)done;
}

void DimLayer::show(const std::wstring& monitorId, const Rect& bounds, int alpha, COLORREF colour)
{
    if (bounds.empty())
        return;
    const int clamped = std::clamp(alpha, 0, 255);
    auto it = m_windows.find(monitorId);
    if (it == m_windows.end()) {
        if (clamped == 0)
            return; // nothing to show
        registerClassOnce();
        // A plain popup: WS_EX_LAYERED for the alpha, TRANSPARENT so every
        // click lands on the window below, NOACTIVATE so it never steals focus,
        // TOOLWINDOW so it stays out of the taskbar and Alt+Tab.
        const HWND hwnd = ::CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW
                | WS_EX_TOPMOST,
            kClassName, L"", WS_POPUP, bounds.left, bounds.top,
            bounds.right - bounds.left, bounds.bottom - bounds.top,
            nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        if (!hwnd)
            return;
        ::SetPropW(hwnd, kColourProp,
            reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(RGB(0, 0, 0))));
        Entry entry;
        entry.hwnd = hwnd;
        entry.bounds = bounds;
        it = m_windows.emplace(monitorId, std::move(entry)).first;
        // Logged once per window so a failing layer shows in plugins.log
        // instead of leaving the user with an unexplained dark screen.
        winshun::debug("WinShunBright: dim layer for " + winshun::utf8(monitorId)
            + " hwnd=" + std::to_string(reinterpret_cast<ULONG_PTR>(hwnd))
            + " at " + std::to_string(bounds.left) + "," + std::to_string(bounds.top)
            + " " + std::to_string(bounds.right - bounds.left) + "x"
            + std::to_string(bounds.bottom - bounds.top));
    }

    Entry& entry = it->second;
    const bool moved = entry.bounds.left != bounds.left || entry.bounds.top != bounds.top
        || entry.bounds.right != bounds.right || entry.bounds.bottom != bounds.bottom;
    if (moved) {
        entry.bounds = bounds;
        ::SetWindowPos(entry.hwnd, HWND_TOPMOST, bounds.left, bounds.top,
            bounds.right - bounds.left, bounds.bottom - bounds.top,
            SWP_NOACTIVATE);
    }
    if (entry.colour != colour) {
        entry.colour = colour;
        ::SetPropW(entry.hwnd, kColourProp,
            reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(colour)));
        ::InvalidateRect(entry.hwnd, nullptr, TRUE);
    }
    if (entry.alpha != clamped || moved) {
        entry.alpha = clamped;
        // A fully transparent layer is hidden instead of shown with alpha 0:
        // some drivers still composite a zero-alpha window, and a hidden one
        // costs nothing at all.
        if (clamped == 0) {
            ::ShowWindow(entry.hwnd, SW_HIDE);
        } else {
            ::SetLayeredWindowAttributes(entry.hwnd, 0, static_cast<BYTE>(clamped), LWA_ALPHA);
            ::ShowWindow(entry.hwnd, SW_SHOWNOACTIVATE);
            ::SetWindowPos(entry.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
}

void DimLayer::hide(const std::wstring& monitorId)
{
    const auto it = m_windows.find(monitorId);
    if (it == m_windows.end() || !it->second.hwnd)
        return;
    // Zero the layered alpha too: a hidden window that keeps its old value
    // would come back at the old darkness on the next show().
    ::SetLayeredWindowAttributes(it->second.hwnd, 0, 0, LWA_ALPHA);
    it->second.alpha = 0;
    ::ShowWindow(it->second.hwnd, SW_HIDE);
}

void DimLayer::hideAll()
{
    for (auto& [id, entry] : m_windows) {
        if (!entry.hwnd)
            continue;
        ::SetLayeredWindowAttributes(entry.hwnd, 0, 0, LWA_ALPHA);
        entry.alpha = 0;
        ::ShowWindow(entry.hwnd, SW_HIDE);
    }
}

void DimLayer::destroyAll()
{
    for (auto& [id, entry] : m_windows) {
        if (entry.hwnd)
            ::DestroyWindow(entry.hwnd);
    }
    m_windows.clear();
}

int DimLayer::alphaOf(const std::wstring& monitorId)
{
    const auto it = m_windows.find(monitorId);
    return it == m_windows.end() ? 0 : it->second.alpha;
}

LRESULT CALLBACK DimLayer::procStatic(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    return proc(hwnd, msg, wp, lp);
}

LRESULT CALLBACK DimLayer::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NCHITTEST:
        // Belt and braces on top of WS_EX_TRANSPARENT: never intercept a click.
        return HTTRANSPARENT;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = ::BeginPaint(hwnd, &ps);
        RECT rc;
        ::GetClientRect(hwnd, &rc);
        // The colour is a window property so the static proc can reach it;
        // SetLayeredWindowAttributes then applies the alpha to it.
        const COLORREF colour = static_cast<COLORREF>(reinterpret_cast<ULONG_PTR>(
            ::GetPropW(hwnd, kColourProp)));
        HBRUSH brush = ::CreateSolidBrush(colour);
        ::FillRect(dc, &rc, brush);
        ::DeleteObject(brush);
        ::EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        ::RemovePropW(hwnd, kColourProp);
        return 0;
    default:
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
}

} // namespace br
