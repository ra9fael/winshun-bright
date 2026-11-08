// settings.h — plugin settings with strict validation. All bad values fall
// back to defaults instead of erroring: a wrong settings file must never
// disable the plugin or produce out-of-range behavior.
#pragma once

#include "bright_logic.h"
#include "winshun.h"

namespace br {

struct Settings {
    int percentPerStep = 5; // 1..10
    bool gestureEnabled = true;
    bool showOverlay = true;
    int overlayMs = 600; // 300..3000
    OverlayAnchor overlayAnchor = OverlayAnchor::BottomCenter;
    int overlayOffsetX = 0; // -4000..4000 px from the anchor
    int overlayOffsetY = 0;
    int warmStrength = 0; // 0..100; 0 disables the filter
    bool warmRemember = false;
    bool chinese = true; // host language for overlay / list text; not a user setting
};

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
