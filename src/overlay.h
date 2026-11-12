// overlay.h — a tiny non-activating window that flashes the result
// ("全部 65%", "暖色滤镜 · 开") near the configured anchor. Lives on the
// worker thread.
#pragma once

#include "bright_logic.h"
#include "egg_effects.h"

#include <windows.h>
#include <d2d1.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>

#include <string>

namespace br {

struct OverlayPlacement {
    OverlayAnchor anchor = OverlayAnchor::BottomCenter;
    int offsetX = 0; // pixels from the anchor, applied after positioning
    int offsetY = 0;
    // The panel the action touched (virtual-screen pixels). With
    // OverlayAnchor::TargetMonitor this is where the panel goes; empty means
    // the pointer decides instead.
    Rect target;
};

class Overlay {
public:
    Overlay() = default;
    ~Overlay() { destroy(); }
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    // Creates the window; must be called from the owning thread.
    bool create();
    void destroy();
    bool valid() const { return m_hwnd != nullptr; }

    // Shows a one-line message for `ms` milliseconds, then hides itself.
    void show(const std::wstring& text, const OverlayPlacement& placement, int ms);

    // Shows a large percentage with a label and a progress bar, for fine
    // adjustment while scrolling.
    void showValue(const std::wstring& label, int percent, const OverlayPlacement& placement, int ms);

    // Shows a brief mushroom animation without changing the desktop image.
    // It has a window of its own, so a brightness overlay drawn while the user
    // keeps scrolling cannot replace it; only hideMushroomEgg() (or its own
    // duration expiring) ends it.
    void showMushroomEgg(const OverlayPlacement& placement, int durationMs);
    void hideMushroomEgg();
    bool mushroomEggVisible() const { return m_eggHwnd != nullptr && m_eggShown; }

private:
    static LRESULT CALLBACK procStatic(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void present(const OverlayPlacement& placement, int ms);
    void paint();
    static void registerClassOnce();

    // The egg window is separate from the value/message window: the two never
    // share state or timers, so one cannot hide the other.
    static LRESULT CALLBACK eggProcStatic(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT eggProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static void registerEggClassOnce();
    bool createEggWindow();
    void placeEggWindow(const OverlayPlacement& placement);
    bool paintEgg();
    // Per-play renderer state. D2D renders into a 32-bit premultiplied-alpha
    // DIB, then UpdateLayeredWindow composites only the non-transparent pixels.
    void ensureEggRenderer(int width, int height);
    void releaseEggRenderer();
    void releaseEggBitmap();
    // Rasterises one emoji into a cached bitmap for cheap per-frame scaling.
    bool buildEggSprite(const wchar_t* glyph, ID2D1Bitmap** out);
    // WIC needs COM on the thread that renders; the worker thread has none.
    bool ensureCom();

    HWND m_hwnd = nullptr;
    int m_durationMs = 600;
    bool m_valueMode = false;
    std::wstring m_text;
    std::wstring m_label;
    int m_percent = 0;
    HWND m_eggHwnd = nullptr;
    bool m_eggShown = false;
    long long m_eggStartedAt = 0;
    int m_eggEffect = 0;
    int m_eggFrameIntervalMs = 33;
    RECT m_eggBounds {};
    int m_eggWidth = 0;
    int m_eggHeight = 0;
    double m_eggScale = 1.0;
    // D2D renders through a compatible target into a top-down 32-bit DIB.
    ID2D1Factory1* m_d2dFactory = nullptr;
    ID2D1DCRenderTarget* m_eggDcRt = nullptr;
    // The emoji are rasterised once per monitor and then scaled as bitmaps:
    // re-rasterising a colour glyph at a new size every frame is what made
    // the animation stutter.
    ID2D1Bitmap* m_eggMushroomSprite = nullptr;
    ID2D1Bitmap* m_eggStarSprite = nullptr;
    ID2D1SolidColorBrush* m_eggTextBrush = nullptr;
    ID2D1SolidColorBrush* m_eggGoldBrush = nullptr;
    ID2D1SolidColorBrush* m_eggRingBrush = nullptr;
    ID2D1GradientStopCollection* m_eggGlowStops = nullptr;
    ID2D1RadialGradientBrush* m_eggGlowBrush = nullptr;
    IDWriteFactory* m_dwFactory = nullptr;
    IDWriteTextFormat* m_emojiFormat = nullptr;
    // A fixed-size format: the layout rectangle is device space, so the
    // animated size has to come from the transform, not from the font size.
    IDWriteTextFormat* m_eggSpriteFormat = nullptr;
    IWICImagingFactory* m_wicFactory = nullptr;
    bool m_comInitialized = false;
    HBITMAP m_eggBitmap = nullptr;
    HDC m_eggMemoryDc = nullptr;
    HGDIOBJ m_eggOldBitmap = nullptr;
    void* m_eggPixels = nullptr;
    bool m_d2dReady = false;   // false: DrawText fallback into the same alpha DIB
};

} // namespace br
