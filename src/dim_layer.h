// dim_layer.h — full-screen black layers that simulate a lower brightness on
// displays no hardware channel can drive (a TV through an AVR, a projector, a
// remote session's virtual panel).
//
// Contract:
//  * One click-through, never-activated, topmost window per panel, covering
//    exactly that panel's rectangle. It never takes focus, never appears in
//    the taskbar or Alt+Tab, and never eats a mouse event.
//  * Lives on the worker thread: create/update/destroy all happen there, so
//    the same thread that owns the overlay and the display cache owns these.
//  * Alpha comes from dimAlpha(); the colour is only changed by the egg, which
//    paints the layer a poisonous purple-green instead of plain black.
//  * Destroyed when the plugin goes away, so nothing is left on screen.
#pragma once

#include "bright_logic.h"

#include <windows.h>

#include <map>
#include <string>

namespace br {

class DimLayer {
public:
    DimLayer() = default;
    ~DimLayer() { destroyAll(); }
    DimLayer(const DimLayer&) = delete;
    DimLayer& operator=(const DimLayer&) = delete;

    // Shows (or updates) the layer over `bounds`. `alpha` 0 hides it entirely.
    // Called on the worker thread only.
    void show(const std::wstring& monitorId, const Rect& bounds, int alpha,
        COLORREF colour = RGB(0, 0, 0));
    void hide(const std::wstring& monitorId);
    void hideAll();
    // A display change: the rectangles moved, so every layer goes and is made
    // again by the caller's next show().
    void destroyAll();

    // The alpha currently painted, 0 when there is no window at all.
    int alphaOf(const std::wstring& monitorId);
    std::size_t count() const { return m_windows.size(); }

private:
    struct Entry {
        HWND hwnd = nullptr;
        Rect bounds;
        int alpha = 0;
        COLORREF colour = RGB(0, 0, 0);
    };

    static void registerClassOnce();
    static LRESULT CALLBACK procStatic(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    std::map<std::wstring, Entry> m_windows;
};

} // namespace br
