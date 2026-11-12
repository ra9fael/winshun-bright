// overlay.cpp — non-activating tooltip-style brightness indicator.
#include "overlay.h"

#include "winshun.h"

#include <algorithm>
#include <cmath>
#include <random>

namespace br {

namespace {

constexpr wchar_t kClassName[] = L"WinShunBrightOverlay";
constexpr wchar_t kEggClassName[] = L"WinShunBrightEgg";
constexpr int kWidth = 220;      // value mode (big percentage)
constexpr int kHeight = 76;
constexpr int kTextWidth = 320;  // message mode: long reasons fit on one line
constexpr int kTextHeight = 72;
constexpr UINT_PTR kHideTimer = 1;
// The egg has its own window, so its timers are its own ids on that window.
constexpr UINT_PTR kEggHideTimer = 1;
constexpr UINT_PTR kEggAnimationTimer = 2;
constexpr int kAnimationTickMs = 33;
constexpr double kEggCanvasSize = kEggEffectSpace;
// The emoji sprites: a fixed font size, drawn into a slightly larger bitmap
// so wide glyphs are not clipped.
constexpr int kEggSpriteFontSize = 512;
constexpr int kEggSpriteSize = 640;

HFONT makeFont(int height, int weight)
{
    return ::CreateFontW(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

} // namespace

void Overlay::registerClassOnce()
{
    static const bool done = [] {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = &Overlay::procStatic;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kClassName;
        return ::RegisterClassW(&wc) != 0; // ERROR_CLASS_ALREADY_EXISTS is fine
    }();
    (void)done;
}

void Overlay::registerEggClassOnce()
{
    static const bool done = [] {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = &Overlay::eggProcStatic;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = nullptr; // the egg never takes the pointer
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kEggClassName;
        return ::RegisterClassW(&wc) != 0; // ERROR_CLASS_ALREADY_EXISTS is fine
    }();
    (void)done;
}

bool Overlay::create()
{
    if (m_hwnd)
        return true;
    registerClassOnce();

    m_hwnd = ::CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        kClassName, L"", WS_POPUP, 0, 0, kWidth, kHeight, nullptr, nullptr,
        ::GetModuleHandleW(nullptr), this); // arrives as lpCreateParams
    return m_hwnd != nullptr;
}

void Overlay::destroy()
{
    if (m_eggHwnd) {
        ::DestroyWindow(m_eggHwnd);
        m_eggHwnd = nullptr;
    }
    m_eggShown = false;
    releaseEggRenderer();
    if (m_hwnd) {
        ::DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

void Overlay::show(const std::wstring& text, const OverlayPlacement& placement, int ms)
{
    if (!m_hwnd || text.empty())
        return;
    m_valueMode = false;
    m_text = text;
    present(placement, ms);
}

void Overlay::showValue(const std::wstring& label, int percent, const OverlayPlacement& placement, int ms)
{
    if (!m_hwnd)
        return;
    m_valueMode = true;
    m_label = label;
    m_percent = std::clamp(percent, 0, 100);
    present(placement, ms);
}

bool Overlay::createEggWindow()
{
    if (m_eggHwnd)
        return true;
    registerEggClassOnce();
    // Same styles as the brightness panel: click-through, never focused, out
    // of the taskbar and Alt+Tab. It is a window of its own so that a value
    // overlay shown while the user keeps scrolling cannot replace the egg.
    m_eggHwnd = ::CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED,
        kEggClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
        ::GetModuleHandleW(nullptr), this); // arrives as lpCreateParams
    return m_eggHwnd != nullptr;
}

void Overlay::showMushroomEgg(const OverlayPlacement& placement, int durationMs)
{
    if (!m_hwnd || !createEggWindow())
        return;
    // Recreated lazily for the target display's current pixel dimensions.
    // One of the five concepts, picked per play and then looped until the
    // user clicks.
    static std::mt19937 rng { std::random_device {}() };
    m_eggEffect = std::uniform_int_distribution<int>(0, kEggEffectCount - 1)(rng);
    // Debug aid: BRIGHT_EFFECT_ID pins the concept so a frame can be dumped.
    if (const char* forced = ::getenv("BRIGHT_EFFECT_ID")) {
        const int id = std::atoi(forced);
        if (id >= 0 && id < kEggEffectCount)
            m_eggEffect = id;
    }
    m_eggStartedAt = static_cast<long long>(::GetTickCount64());
    m_eggFrameIntervalMs = kAnimationTickMs;
    placeEggWindow(placement);
    ensureEggRenderer(m_eggWidth, m_eggHeight);
    // Render and submit the first alpha frame before exposing a full-screen
    // window. If graphics setup fails, no blank fullscreen surface flashes.
    if (!paintEgg()) {
        winshun::debug("WinShunBright: could not render the first full-screen egg frame");
        return;
    }
    m_eggShown = true;
    ::ShowWindow(m_eggHwnd, SW_SHOWNOACTIVATE);
    (void)durationMs; // the click is the only normal way to end the effect
    ::SetTimer(m_eggHwnd, kEggAnimationTimer, kAnimationTickMs, nullptr);
}

void Overlay::hideMushroomEgg()
{
    if (!m_eggHwnd || !m_eggShown)
        return;
    ::KillTimer(m_eggHwnd, kEggAnimationTimer);
    ::KillTimer(m_eggHwnd, kEggHideTimer);
    m_eggShown = false;
    ::ShowWindow(m_eggHwnd, SW_HIDE);
}

// Covers the monitor containing the gesture with a per-pixel transparent
// layered window. The full desktop remains visible between effect primitives.
void Overlay::placeEggWindow(const OverlayPlacement& placement)
{
    POINT reference;
    if (!placement.target.empty()) {
        reference.x = placement.target.left + (placement.target.right - placement.target.left) / 2;
        reference.y = placement.target.top + (placement.target.bottom - placement.target.top) / 2;
    } else {
        ::GetCursorPos(&reference);
    }
    RECT bounds { 0, 0, ::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN) };
    MONITORINFO mi { sizeof mi };
    if (::GetMonitorInfoW(::MonitorFromPoint(reference, MONITOR_DEFAULTTONEAREST), &mi))
        bounds = mi.rcMonitor;

    m_eggBounds = bounds;
    m_eggWidth = std::max(1, static_cast<int>(bounds.right - bounds.left));
    m_eggHeight = std::max(1, static_cast<int>(bounds.bottom - bounds.top));
    m_eggScale = std::min(m_eggWidth, m_eggHeight) / kEggCanvasSize;
    if (m_eggScale < 1.0)
        m_eggScale = 1.0;

    releaseEggBitmap();
    ::SetWindowPos(m_eggHwnd, HWND_TOPMOST, bounds.left, bounds.top, m_eggWidth, m_eggHeight,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

// Places the panel on the monitor the gesture changed, or the one the pointer
// is on. Fixed anchors use the work area (so the taskbar never covers it).
void Overlay::present(const OverlayPlacement& placement, int ms)
{
    m_durationMs = ms;
    ::KillTimer(m_hwnd, kHideTimer);

    POINT cursor;
    ::GetCursorPos(&cursor);

    // Which monitor: the one the action touched when the worker knows it,
    // otherwise the pointer's.
    POINT reference = cursor;
    if (!placement.target.empty()) {
        reference.x = placement.target.left + (placement.target.right - placement.target.left) / 2;
        reference.y = placement.target.top + (placement.target.bottom - placement.target.top) / 2;
    }

    RECT bounds { 0, 0, ::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN) };
    MONITORINFO mi { sizeof mi };
    if (::GetMonitorInfoW(::MonitorFromPoint(reference, MONITOR_DEFAULTTONEAREST), &mi))
        bounds = mi.rcWork;

    const int midX = bounds.left + (bounds.right - bounds.left) / 2;
    const int midY = bounds.top + (bounds.bottom - bounds.top) / 2;
    const int w = m_valueMode ? kWidth : kTextWidth;
    const int h = m_valueMode ? kHeight : kTextHeight;
    const int margin = 24;
    int x = midX - w / 2;
    int y = midY - h / 2;

    switch (placement.anchor) {
    case OverlayAnchor::TopLeft:
        x = bounds.left + margin;
        y = bounds.top + margin;
        break;
    case OverlayAnchor::TopCenter:
        x = midX - w / 2;
        y = bounds.top + margin;
        break;
    case OverlayAnchor::TopRight:
        x = bounds.right - w - margin;
        y = bounds.top + margin;
        break;
    case OverlayAnchor::LeftCenter:
        x = bounds.left + margin;
        y = midY - h / 2;
        break;
    case OverlayAnchor::RightCenter:
        x = bounds.right - w - margin;
        y = midY - h / 2;
        break;
    case OverlayAnchor::BottomLeft:
        x = bounds.left + margin;
        y = bounds.bottom - h - margin;
        break;
    case OverlayAnchor::BottomCenter:
        x = midX - w / 2;
        y = bounds.bottom - h - margin;
        break;
    case OverlayAnchor::BottomRight:
        x = bounds.right - w - margin;
        y = bounds.bottom - h - margin;
        break;
    // A fixed anchor now centers on the target panel instead of doing nothing
    // (it used to be the same branch as Center).
    case OverlayAnchor::MonitorUnderMouse:
    case OverlayAnchor::TargetMonitor:
        if (placement.anchor == OverlayAnchor::TargetMonitor) {
            // Big value, centre of the panel that was changed.
            x = midX - w / 2;
            y = midY - h / 2;
        }
        break;
    case OverlayAnchor::Center:
    default:
        break;
    }

    x += placement.offsetX;
    y += placement.offsetY;

    if (x + w > bounds.right)
        x = bounds.right - w;
    if (x < bounds.left)
        x = bounds.left;
    if (y + h > bounds.bottom)
        y = bounds.bottom - h;
    if (y < bounds.top)
        y = bounds.top;

    ::SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, w, h,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ::SetTimer(m_hwnd, kHideTimer, static_cast<UINT>(ms), nullptr);
    ::InvalidateRect(m_hwnd, nullptr, TRUE);
}

void Overlay::paint()
{
    PAINTSTRUCT ps;
    HDC dc = ::BeginPaint(m_hwnd, &ps);
    RECT rc;
    ::GetClientRect(m_hwnd, &rc);

    // Dark rounded panel, light text — readable over both light and dark
    // desktops, independent of the host theme.
    HBRUSH bg = ::CreateSolidBrush(RGB(32, 32, 36));
    HRGN rgn = ::CreateRoundRectRgn(rc.left, rc.top, rc.right + 1, rc.bottom + 1, 12, 12);
    ::FillRgn(dc, rgn, bg);
    ::DeleteObject(rgn);
    ::DeleteObject(bg);

    ::SetBkMode(dc, TRANSPARENT);

    if (!m_valueMode) {
        ::SetTextColor(dc, RGB(240, 240, 240));
        HFONT font = makeFont(15, FW_SEMIBOLD);
        HFONT old = static_cast<HFONT>(::SelectObject(dc, font));
        RECT textRect { rc.left + 12, rc.top + 8, rc.right - 12, rc.bottom - 8 };
        // Long reasons need two lines; measure first so a single line can
        // still be centred vertically.
        RECT measure = textRect;
        ::DrawTextW(dc, m_text.c_str(), -1, &measure,
            DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
        const int textHeight = measure.bottom - measure.top;
        const int room = textRect.bottom - textRect.top;
        if (textHeight > 0 && textHeight < room)
            textRect.top += (room - textHeight) / 2;
        ::DrawTextW(dc, m_text.c_str(), -1, &textRect,
            DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
        ::SelectObject(dc, old);
        ::DeleteObject(font);
        ::EndPaint(m_hwnd, &ps);
        return;
    }

    // Small label at the top.
    ::SetTextColor(dc, RGB(198, 198, 208));
    HFONT small = makeFont(13, FW_NORMAL);
    HFONT old = static_cast<HFONT>(::SelectObject(dc, small));
    RECT labelRect { rc.left + 12, rc.top + 8, rc.right - 12, rc.top + 26 };
    ::DrawTextW(dc, m_label.c_str(), -1, &labelRect,
        DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    ::SelectObject(dc, old);
    ::DeleteObject(small);

    // Big percentage.
    ::SetTextColor(dc, RGB(245, 245, 248));
    HFONT big = makeFont(38, FW_BOLD);
    old = static_cast<HFONT>(::SelectObject(dc, big));
    RECT valueRect { rc.left, rc.top + 22, rc.right, rc.top + 64 };
    const std::wstring value = std::to_wstring(m_percent) + L"%";
    ::DrawTextW(dc, value.c_str(), -1, &valueRect,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    ::SelectObject(dc, old);
    ::DeleteObject(big);

    // Progress bar along the bottom.
    const int trackLeft = rc.left + 14;
    const int trackRight = rc.right - 14;
    const int trackTop = rc.bottom - 10;
    const int trackBottom = rc.bottom - 7;
    HBRUSH track = ::CreateSolidBrush(RGB(58, 58, 64));
    RECT trackRect { trackLeft, trackTop, trackRight, trackBottom };
    ::FillRect(dc, &trackRect, track);
    ::DeleteObject(track);

    if (m_percent > 0) {
        const int fillRight = trackLeft
            + static_cast<int>((trackRight - trackLeft) * (m_percent / 100.0));
        HBRUSH fill = ::CreateSolidBrush(RGB(255, 214, 122));
        RECT fillRect { trackLeft, trackTop, std::max(trackLeft + 1, fillRight), trackBottom };
        ::FillRect(dc, &fillRect, fill);
        ::DeleteObject(fill);
    }

    ::EndPaint(m_hwnd, &ps);
}

LRESULT CALLBACK Overlay::procStatic(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Overlay* self = nullptr;
    if (msg == WM_NCCREATE) {
        const CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<Overlay*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<Overlay*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self)
        return self->proc(hwnd, msg, wp, lp);
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Overlay::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT:
        paint();
        return 0;
    case WM_TIMER:
        if (wp == kHideTimer) {
            ::KillTimer(hwnd, kHideTimer);
            ::ShowWindow(hwnd, SW_HIDE);
        }
        return 0;
    default:
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
}

LRESULT CALLBACK Overlay::eggProcStatic(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Overlay* self = nullptr;
    if (msg == WM_NCCREATE) {
        const CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<Overlay*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<Overlay*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self)
        return self->eggProc(hwnd, msg, wp, lp);
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Overlay::eggProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NCHITTEST:
        return HTTRANSPARENT; // on top of WS_EX_TRANSPARENT: never eats a click
    case WM_PAINT: {
        PAINTSTRUCT ps;
        ::BeginPaint(hwnd, &ps);
        ::EndPaint(hwnd, &ps);
        paintEgg();
        return 0;
    }
    case WM_TIMER:
        if (wp == kEggHideTimer) {
            // The animation's own duration is up: hide it, nothing else.
            ::KillTimer(hwnd, kEggHideTimer);
            ::KillTimer(hwnd, kEggAnimationTimer);
            m_eggShown = false;
            ::ShowWindow(hwnd, SW_HIDE);
        } else if (wp == kEggAnimationTimer && m_eggShown) {
            // Keep the effect id chosen at trigger time. paintEgg() wraps its
            // local time so this one concept loops smoothly until click.
            const ULONGLONG frameStart = ::GetTickCount64();
            if (!paintEgg())
                winshun::debug("WinShunBright: failed to refresh the full-screen egg frame");
            const int renderMs = static_cast<int>(::GetTickCount64() - frameStart);
            // Avoid scheduling frames faster than this machine can submit the
            // full layered window. Keep 30 fps when cheap, back off smoothly
            // toward 20 fps on high-resolution/slow displays.
            m_eggFrameIntervalMs = std::clamp(renderMs + 8, 33, 50);
            ::SetTimer(hwnd, kEggAnimationTimer, m_eggFrameIntervalMs, nullptr);
        }
        return 0;
    default:
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
}

// ---- egg renderer -----------------------------------------------------------------

// Prepares the D2D/DWrite state once per process. A failure anywhere leaves
// m_d2dReady false and paintEgg() draws the degraded text-only frame instead.
void Overlay::ensureEggRenderer(int width, int height)
{
    if (m_d2dReady && m_eggWidth == width && m_eggHeight == height)
        return;
    releaseEggRenderer();
    if (FAILED(::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
            IID_PPV_ARGS(&m_d2dFactory))) || !m_d2dFactory)
        return;
    if (FAILED(::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(&m_dwFactory))) || !m_dwFactory) {
        releaseEggRenderer();
        return;
    }
    if (FAILED(m_dwFactory->CreateTextFormat(L"Segoe UI Emoji", nullptr,
            DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 24.0f, L"en-US", &m_emojiFormat)) || !m_emojiFormat) {
        releaseEggRenderer();
        return;
    }
    m_emojiFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    m_emojiFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    // The sprite format draws at a fixed size; every frame scales it.
    if (FAILED(m_dwFactory->CreateTextFormat(L"Segoe UI Emoji", nullptr,
            DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, static_cast<FLOAT>(kEggSpriteFontSize),
            L"en-US", &m_eggSpriteFormat)) || !m_eggSpriteFormat) {
        releaseEggRenderer();
        return;
    }
    m_eggSpriteFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    m_eggSpriteFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    m_d2dReady = true;
}

// Rasterises one emoji into a reusable D2D bitmap. Colour glyphs are
// expensive to shape and rasterise, and the animation changes their size
// every frame; rendering them once and scaling the bitmap is what keeps the
// fullscreen effect smooth.
bool Overlay::ensureCom()
{
    if (m_comInitialized)
        return true;
    const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // RPC_E_CHANGED_MODE means COM is already up in another mode; the WIC
    // factory still works then.
    m_comInitialized = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    return m_comInitialized;
}

bool Overlay::buildEggSprite(const wchar_t* glyph, ID2D1Bitmap** out)
{
    if (!out || !m_eggDcRt || !m_d2dFactory || !m_emojiFormat || !m_eggTextBrush)
        return false;
    if (!ensureCom())
        return false;
    if (!m_wicFactory) {
        if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_wicFactory))))
            return false;
    }
    constexpr int kSprite = kEggSpriteSize; // the glyph is 512 px inside it
    BITMAPINFO bi {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = kSprite;
    bi.bmiHeader.biHeight = -kSprite; // top-down BGRA
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = ::CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HDC dc = ::CreateCompatibleDC(nullptr);
    if (!bitmap || !dc || !pixels) {
        if (bitmap)
            ::DeleteObject(bitmap);
        if (dc)
            ::DeleteDC(dc);
        return false;
    }
    HGDIOBJ previous = ::SelectObject(dc, bitmap);
    RECT spriteRect { 0, 0, kSprite, kSprite };
    bool ok = SUCCEEDED(m_eggDcRt->BindDC(dc, &spriteRect));
    if (ok) {
        m_eggDcRt->BeginDraw();
        // Explicit transparent black: ColorF(0, 0.0f) picks the (r,g,b) overload
        // in the MinGW headers and would clear to opaque black.
        m_eggDcRt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        // No transform: the layout rectangle is device space, so the glyph
        // lands centred in the sprite as it is.
        m_eggDcRt->SetTransform(D2D1::Matrix3x2F::Identity());
        const D2D1_RECT_F layout { 0.0f, 0.0f, static_cast<float>(kSprite),
            static_cast<float>(kSprite) };
        m_eggDcRt->DrawTextW(glyph, static_cast<UINT32>(::wcslen(glyph)), m_eggSpriteFormat,
            layout, m_eggTextBrush, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        m_eggDcRt->SetTransform(D2D1::Matrix3x2F::Identity());
        ok = SUCCEEDED(m_eggDcRt->EndDraw());
    }
    // The render target is bound to the sprite DC now; the caller rebinds the
    // screen DIB before the next frame.
    if (ok) {
        IWICBitmap* wic = nullptr;
        const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        // The rendered pixels go *into* the WIC bitmap; CopyPixels would copy
        // the other way and hand us an empty image.
        if (SUCCEEDED(m_wicFactory->CreateBitmapFromMemory(kSprite, kSprite,
                GUID_WICPixelFormat32bppPBGRA, kSprite * 4,
                static_cast<UINT>(kSprite * kSprite * 4),
                static_cast<BYTE*>(pixels), &wic))
            && SUCCEEDED(m_eggDcRt->CreateBitmapFromWicBitmap(wic, &props, out)))
            ok = true;
        else
            ok = false;
        if (wic)
            wic->Release();
    }
    ::SelectObject(dc, previous);
    ::DeleteObject(bitmap);
    ::DeleteDC(dc);
    return ok;
}

void Overlay::releaseEggBitmap()
{
    if (m_eggMushroomSprite) {
        m_eggMushroomSprite->Release();
        m_eggMushroomSprite = nullptr;
    }
    if (m_eggStarSprite) {
        m_eggStarSprite->Release();
        m_eggStarSprite = nullptr;
    }
    if (m_eggGlowBrush) {
        m_eggGlowBrush->Release();
        m_eggGlowBrush = nullptr;
    }
    if (m_eggGlowStops) {
        m_eggGlowStops->Release();
        m_eggGlowStops = nullptr;
    }
    if (m_eggTextBrush) {
        m_eggTextBrush->Release();
        m_eggTextBrush = nullptr;
    }
    if (m_eggGoldBrush) {
        m_eggGoldBrush->Release();
        m_eggGoldBrush = nullptr;
    }
    if (m_eggRingBrush) {
        m_eggRingBrush->Release();
        m_eggRingBrush = nullptr;
    }
    if (m_eggDcRt) {
        m_eggDcRt->Release();
        m_eggDcRt = nullptr;
    }
    if (m_eggMemoryDc && m_eggOldBitmap)
        ::SelectObject(m_eggMemoryDc, m_eggOldBitmap);
    m_eggOldBitmap = nullptr;
    if (m_eggBitmap) {
        ::DeleteObject(m_eggBitmap);
        m_eggBitmap = nullptr;
    }
    if (m_eggMemoryDc) {
        ::DeleteDC(m_eggMemoryDc);
        m_eggMemoryDc = nullptr;
    }
    m_eggPixels = nullptr;
}

void Overlay::releaseEggRenderer()
{
    releaseEggBitmap();
    if (m_eggSpriteFormat) {
        m_eggSpriteFormat->Release();
        m_eggSpriteFormat = nullptr;
    }
    if (m_wicFactory) {
        m_wicFactory->Release();
        m_wicFactory = nullptr;
    }
    if (m_comInitialized) {
        ::CoUninitialize();
        m_comInitialized = false;
    }
    if (m_emojiFormat) {
        m_emojiFormat->Release();
        m_emojiFormat = nullptr;
    }
    if (m_dwFactory) {
        m_dwFactory->Release();
        m_dwFactory = nullptr;
    }
    if (m_d2dFactory) {
        m_d2dFactory->Release();
        m_d2dFactory = nullptr;
    }
    m_d2dReady = false;
}

// Each frame draws emoji and particles into a transparent full-monitor DIB.
// The per-pixel alpha path below preserves the desktop between primitives.
bool Overlay::paintEgg()
{
    const double elapsed = static_cast<double>(
        static_cast<long long>(::GetTickCount64()) - m_eggStartedAt);
    const double t = std::fmod(std::max(0.0, elapsed), static_cast<double>(kEggEffectMs))
        / 1000.0;

    EggPrimitive prims[kEggMaxPrimitives];
    const int n = eggEffectFrame(m_eggEffect, t, prims, kEggMaxPrimitives);

    if (!m_d2dReady || !m_d2dFactory)
        return false; // no full-screen surface can be submitted

    if (!m_eggBitmap || !m_eggMemoryDc || !m_eggPixels || !m_eggDcRt) {
        releaseEggBitmap();
        ensureEggRenderer(m_eggWidth, m_eggHeight);
        BITMAPINFO bi {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = m_eggWidth;
        bi.bmiHeader.biHeight = -m_eggHeight; // top-down BGRA
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        m_eggBitmap = ::CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &m_eggPixels, nullptr, 0);
        m_eggMemoryDc = ::CreateCompatibleDC(nullptr);
        if (!m_eggBitmap || !m_eggMemoryDc || !m_eggPixels) {
            releaseEggBitmap();
            return false;
        }
        m_eggOldBitmap = ::SelectObject(m_eggMemoryDc, m_eggBitmap);
        RECT client { 0, 0, m_eggWidth, m_eggHeight };
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
        if (FAILED(m_d2dFactory->CreateDCRenderTarget(&props, &m_eggDcRt)) || !m_eggDcRt
            || FAILED(m_eggDcRt->BindDC(m_eggMemoryDc, &client))) {
            releaseEggBitmap();
            return false;
        }
        m_eggDcRt->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF), &m_eggTextBrush);
        m_eggDcRt->CreateSolidColorBrush(D2D1::ColorF(0xFFD770), &m_eggGoldBrush);
        m_eggDcRt->CreateSolidColorBrush(D2D1::ColorF(0xFFD67A), &m_eggRingBrush);
        const D2D1_GRADIENT_STOP stops[2] = {
            { 0.0f, D2D1::ColorF(0xFFD67A, 0.35f) },
            { 1.0f, D2D1::ColorF(0xFFD67A, 0.0f) },
        };
        m_eggDcRt->CreateGradientStopCollection(stops, 2, &m_eggGlowStops);
        if (m_eggGlowStops) {
            const float initialRadius = std::max(1.0f, static_cast<float>(m_eggScale) * 16.0f);
            m_eggDcRt->CreateRadialGradientBrush(
                D2D1::RadialGradientBrushProperties(
                    D2D1::Point2F(m_eggWidth * 0.5f, m_eggHeight * 0.5f),
                    D2D1::Point2F(0.0f, 0.0f), initialRadius, initialRadius),
                m_eggGlowStops, &m_eggGlowBrush);
        }
        // Rasterise the two emoji once; the frames then only scale bitmaps.
        buildEggSprite(L"\xD83C\xDF44", &m_eggMushroomSprite);
        buildEggSprite(L"\x2728", &m_eggStarSprite);
    }

    RECT client { 0, 0, m_eggWidth, m_eggHeight };
    if (FAILED(m_eggDcRt->BindDC(m_eggMemoryDc, &client)))
        return false;
    m_eggDcRt->BeginDraw();
    m_eggDcRt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    // Effect coordinates use a square 256x256 logical space. Map its center
    // exactly to the center of the selected monitor; x/y map independently so
    // trajectories fill the display, while glyphs/radii use the uniform
    // short-edge scale below and remain undistorted.
    const float glyphScale = static_cast<float>(m_eggScale);

    for (int i = 0; i < n; ++i) {
        const EggPrimitive& p = prims[i];
        const EggPoint mapped = eggPointOnMonitor(p.x, p.y, m_eggWidth, m_eggHeight);
        const float x = static_cast<float>(mapped.x);
        const float y = static_cast<float>(mapped.y);
        switch (p.kind) {
        case EggPrim::Emoji: {
            if (!m_eggSpriteFormat)
                break;
            // Effect primitives already carry the user's default oversized
            // mushroom/star scale; preserve their animated size per frame.
            const float size = static_cast<float>(p.size) * glyphScale;
            ID2D1Bitmap* sprite = nullptr;
            if (p.glyph[0] == 0xD83C && p.glyph[1] == 0xDF44)
                sprite = m_eggMushroomSprite;
            else if (p.glyph[0] == 0x2728)
                sprite = m_eggStarSprite;
            if (sprite) {
                // Scale the cached bitmap: no glyph shaping per frame. The
                // destination rectangle is device space, so the position and
                // size are written out directly and only the rotation comes
                // from the transform.
                const float scale = size / static_cast<float>(kEggSpriteFontSize);
                const float half = static_cast<float>(kEggSpriteSize) * 0.5f * scale;
                m_eggDcRt->SetTransform(D2D1::Matrix3x2F::Rotation(
                    static_cast<float>(p.rotDeg), D2D1::Point2F(x, y)));
                const D2D1_RECT_F dest { x - half, y - half, x + half, y + half };
                m_eggDcRt->DrawBitmap(sprite, dest, static_cast<float>(p.alpha),
                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr);
                m_eggDcRt->SetTransform(D2D1::Matrix3x2F::Identity());
                break;
            }
            ID2D1SolidColorBrush* textBrush = m_eggTextBrush;
            if (!textBrush)
                break;
            textBrush->SetOpacity(static_cast<float>(p.alpha));
            // The layout rectangle is device space: the glyph is laid out in
            // a fixed box that the transform then moves, rotates and scales.
            const float scale = size / static_cast<float>(kEggSpriteFontSize);
            const float half = static_cast<float>(kEggSpriteFontSize) * 0.5f;
            m_eggDcRt->SetTransform(D2D1::Matrix3x2F::Translation(x, y)
                * D2D1::Matrix3x2F::Rotation(static_cast<float>(p.rotDeg))
                * D2D1::Matrix3x2F::Scale(scale, scale * static_cast<float>(p.sy))
                * D2D1::Matrix3x2F::Translation(-half, -half));
            const D2D1_RECT_F layout { 0.0f, 0.0f, static_cast<float>(kEggSpriteFontSize),
                static_cast<float>(kEggSpriteFontSize) };
            const UINT32 glyphLength = static_cast<UINT32>(::wcslen(p.glyph));
            m_eggDcRt->DrawTextW(p.glyph, glyphLength,
                m_eggSpriteFormat, layout, textBrush,
                D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
            m_eggDcRt->SetTransform(D2D1::Matrix3x2F::Identity());
            break;
        }
        case EggPrim::Dot: {
            ID2D1SolidColorBrush* b = m_eggGoldBrush;
            if (!b)
                break;
            b->SetOpacity(static_cast<float>(p.alpha));
            m_eggDcRt->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(x, y),
                    static_cast<float>(p.size) * glyphScale,
                    static_cast<float>(p.size) * glyphScale),
                b);
            break;
        }
        case EggPrim::Ring: {
            ID2D1SolidColorBrush* b = m_eggRingBrush;
            if (!b)
                break;
            b->SetOpacity(static_cast<float>(p.alpha));
            m_eggDcRt->DrawEllipse(
                D2D1::Ellipse(D2D1::Point2F(x, y),
                    static_cast<float>(p.size) * glyphScale,
                    static_cast<float>(p.size) * glyphScale),
                b, static_cast<float>(p.lw) * glyphScale);
            break;
        }
        case EggPrim::Glow: {
            // Reuse one radial brush per monitor instead of allocating D2D
            // gradient resources on every frame and for every glow primitive.
            ID2D1RadialGradientBrush* b = m_eggGlowBrush;
            if (!b)
                break;
            const float radius = std::max(1.0f, static_cast<float>(p.size) * glyphScale);
            b->SetCenter(D2D1::Point2F(x, y));
            b->SetRadiusX(radius);
            b->SetRadiusY(radius);
            b->SetOpacity(static_cast<float>(p.alpha));
            D2D1_ELLIPSE bounds { D2D1::Point2F(x, y), radius, radius };
            m_eggDcRt->FillEllipse(bounds, b);
            break;
        }
        }
    }

    m_eggDcRt->SetTransform(D2D1::Matrix3x2F::Identity());
    const HRESULT drawHr = m_eggDcRt->EndDraw();
    if (FAILED(drawHr)) {
        releaseEggBitmap(); // re-create the bitmap/target on the next animation tick
        return false;
    }

    // Preserve the desktop exactly: per-pixel alpha is only contributed by the
    // emoji/particle pixels rendered into this transparent DIB.
    // Debug aid: BRIGHT_DUMP_EGG=<path.bmp> writes the rendered frame, which
    // is the only way to inspect what the effect actually drew.
    {
        static const char* dumpPath = ::getenv("BRIGHT_DUMP_EGG");
        if (dumpPath && *dumpPath) {
            BITMAPFILEHEADER fh {};
            BITMAPINFOHEADER ih {};
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof fh + sizeof ih;
            fh.bfSize = fh.bfOffBits + static_cast<UINT>(m_eggWidth * 4) * m_eggHeight;
            ih.biSize = sizeof ih;
            ih.biWidth = m_eggWidth;
            ih.biHeight = -m_eggHeight;
            ih.biPlanes = 1;
            ih.biBitCount = 32;
            ih.biCompression = BI_RGB;
            FILE* f = nullptr;
            if (::_wfopen_s(&f, winshun::wide(dumpPath).c_str(), L"wb") == 0 && f) {
                std::fwrite(&fh, sizeof fh, 1, f);
                std::fwrite(&ih, sizeof ih, 1, f);
                // The header declares a top-down image, so write rows in order.
                for (int row = 0; row < m_eggHeight; ++row)
                    std::fwrite(static_cast<const unsigned char*>(m_eggPixels)
                            + static_cast<size_t>(row) * m_eggWidth * 4,
                        1, static_cast<size_t>(m_eggWidth) * 4, f);
                std::fclose(f);
            }
        }
    }
    POINT dst { m_eggBounds.left, m_eggBounds.top };
    SIZE size { m_eggWidth, m_eggHeight };
    POINT src { 0, 0 };
    BLENDFUNCTION blend { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    if (!::UpdateLayeredWindow(m_eggHwnd, nullptr, &dst, &size, m_eggMemoryDc, &src,
            0, &blend, ULW_ALPHA)) {
        winshun::debug("WinShunBright: UpdateLayeredWindow failed for the full-screen egg");
        return false;
    }
    return true;
}

} // namespace br
