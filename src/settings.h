// settings.h — plugin settings with strict validation. All bad values fall
// back to defaults instead of erroring: a wrong settings file must never
// disable the plugin or produce out-of-range behavior.
#pragma once

#include "bright_logic.h"
#include "winshun.h"

namespace br {

// Which panels get a software dim layer: none, only those the hardware
// channels cannot drive, or every panel (hardware and layer together).
enum class SoftwareDim {
    Off,
    UnsupportedOnly,
    Always,
};

struct Settings {
    int percentPerStep = 5; // 1..10
    bool gestureEnabled = true;
    GestureModifiers gestureModifiers = GestureModifiers::ctrlAlt();
    GestureTarget gestureTarget = GestureTarget::MonitorUnderMouse;
    SoftwareDim softwareBrightness = SoftwareDim::Off;
    // A remote session cannot reach the physical panels at all, so the layer
    // is the only working dimmer there; on by default for that reason. Users
    // who find the layer intrusive over RDP can turn it off.
    bool remoteDim = true;
    int softwareMaxDark = 60; // 0..90 percent of black at 0% brightness
    bool mushroomEgg = true;
    bool showOverlay = true;
    int overlayMs = 600; // 300..3000
    OverlayAnchor overlayAnchor = OverlayAnchor::BottomCenter;
    int overlayOffsetX = 0; // -4000..4000 px from the anchor
    int overlayOffsetY = 0;
    int warmStrength = 0; // 0..100; 0 disables the filter
    bool warmRemember = false;
    // Register global hotkeys for +step / -step / max (host-registered when
    // the host offers it, RegisterHotKey otherwise). Empty: none.
    std::wstring hotkeyUp;
    std::wstring hotkeyDown;
    std::wstring hotkeyMax;
    bool gestureDebug = false;
    bool chinese = true; // host language for overlay / list text; not a user setting
};

inline std::wstring hotkeyOf(const winshun::Json& value)
{
    if (!value.isString())
        return {};
    const std::string text = value.toString();
    const std::wstring wide = winshun::wide(text);
    return wide;
}

inline bool effectiveEggEnabled(bool discovered, bool hostEnabled, bool persistedEnabled)
{
    return discovered ? persistedEnabled : hostEnabled;
}

inline Settings parseSettings(const winshun::Json& s)
{
    Settings out;
    if (!s.isObject())
        return out;

    const winshun::Json& step = s["percentPerStep"];
    if (step.isNumber()) {
        const double v = step.toNumber();
        if (v >= 1 && v <= 10)
            out.percentPerStep = static_cast<int>(v);
    }

    const winshun::Json& gesture = s["gestureEnabled"];
    if (gesture.isBool())
        out.gestureEnabled = gesture.toBool();

    const winshun::Json& chord = s["gestureModifiers"];
    if (chord.isString())
        out.gestureModifiers = GestureModifiers::parse(chord.toString());

    const winshun::Json& target = s["gestureTarget"];
    if (target.isString()) {
        const std::string v = target.toString();
        if (v == "all")
            out.gestureTarget = GestureTarget::All;
        else if (v == "monitor-under-mouse" || v == "mouse")
            out.gestureTarget = GestureTarget::MonitorUnderMouse;
        else
            out.gestureTarget = GestureTarget::MonitorUnderMouse;
    }

    const winshun::Json& debug = s["gestureDebug"];
    if (debug.isBool())
        out.gestureDebug = debug.toBool();

    const winshun::Json& soft = s["softwareBrightness"];
    if (soft.isString()) {
        const std::string v = soft.toString();
        if (v == "always")
            out.softwareBrightness = SoftwareDim::Always;
        else if (v == "unsupported-only")
            out.softwareBrightness = SoftwareDim::UnsupportedOnly;
        else if (v == "off")
            out.softwareBrightness = SoftwareDim::Off;
    }

    const winshun::Json& remote = s["remoteDim"];
    if (remote.isBool())
        out.remoteDim = remote.toBool();

    const winshun::Json& dark = s["softwareMaxDark"];
    if (dark.isNumber()) {
        const double v = dark.toNumber();
        if (v >= 0 && v <= 90)
            out.softwareMaxDark = static_cast<int>(v);
    }

    const winshun::Json& egg = s["mushroomEgg"];
    if (egg.isBool())
        out.mushroomEgg = egg.toBool();

    out.hotkeyUp = hotkeyOf(s["hotkeyUp"]);
    out.hotkeyDown = hotkeyOf(s["hotkeyDown"]);
    out.hotkeyMax = hotkeyOf(s["hotkeyMax"]);

    const winshun::Json& overlay = s["showOverlay"];
    if (overlay.isBool())
        out.showOverlay = overlay.toBool();

    const winshun::Json& ms = s["overlayMs"];
    if (ms.isNumber()) {
        const double v = ms.toNumber();
        if (v >= 300 && v <= 3000)
            out.overlayMs = static_cast<int>(v);
    }

    const winshun::Json& anchor = s["overlayAnchor"];
    if (anchor.isString()) {
        const std::string v = anchor.toString();
        if (v == "center")
            out.overlayAnchor = OverlayAnchor::Center;
        else if (v == "top-left")
            out.overlayAnchor = OverlayAnchor::TopLeft;
        else if (v == "top-center")
            out.overlayAnchor = OverlayAnchor::TopCenter;
        else if (v == "top-right")
            out.overlayAnchor = OverlayAnchor::TopRight;
        else if (v == "left-center" || v == "middle-left")
            out.overlayAnchor = OverlayAnchor::LeftCenter;
        else if (v == "right-center" || v == "middle-right")
            out.overlayAnchor = OverlayAnchor::RightCenter;
        else if (v == "bottom-left")
            out.overlayAnchor = OverlayAnchor::BottomLeft;
        else if (v == "bottom-center")
            out.overlayAnchor = OverlayAnchor::BottomCenter;
        else if (v == "bottom-right")
            out.overlayAnchor = OverlayAnchor::BottomRight;
        else if (v == "monitor-under-mouse" || v == "mouse")
            out.overlayAnchor = OverlayAnchor::MonitorUnderMouse;
        else if (v == "target-monitor" || v == "target")
            out.overlayAnchor = OverlayAnchor::TargetMonitor;
        else
            out.overlayAnchor = OverlayAnchor::BottomCenter;
    }

    auto offset = [](const winshun::Json& value) {
        if (!value.isNumber())
            return 0;
        double d = value.toNumber();
        if (d < -4000)
            d = -4000;
        if (d > 4000)
            d = 4000;
        return static_cast<int>(d);
    };
    out.overlayOffsetX = offset(s["overlayOffsetX"]);
    out.overlayOffsetY = offset(s["overlayOffsetY"]);

    const winshun::Json& warm = s["warmStrength"];
    if (warm.isNumber()) {
        const double v = warm.toNumber();
        if (v >= 0 && v <= 100)
            out.warmStrength = static_cast<int>(v);
    }

    const winshun::Json& remember = s["warmRemember"];
    if (remember.isBool())
        out.warmRemember = remember.toBool();

    return out;
}

} // namespace br
