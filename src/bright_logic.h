// bright_logic.h — pure decision and math core for the brightness plugin.
//
// Nothing here touches Win32: gestures, wheel accumulation, percent/VCP
// conversion, the warm-filter LUT, monitor list normalization and query
// parsing are all side-effect free so they can be unit tested exhaustively.
// The hook thread, the worker and the protocol layer only glue real events
// into these functions.
//
// English-only comments per project convention.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace br {

constexpr int kWheelDelta = 120; // one standard wheel notch

// ---- overlay placement ---------------------------------------------------------

// The nine fixed anchors of the manifest, plus "monitor-under-mouse" (the
// panel is centred on the monitor the pointer is on).
enum class OverlayAnchor {
    Center,
    TopLeft,
    TopCenter,
    TopRight,
    LeftCenter,
    RightCenter,
    BottomLeft,
    BottomCenter,
    BottomRight,
    MonitorUnderMouse,
};

// ---- monitors ------------------------------------------------------------------

enum class Channel { None, Wmi, Ddc };

// One display as the plugin sees it. Plain data: the backend fills it, the
// query layer renders it, the tests build it by hand.
struct MonitorInfo {
    std::wstring id;   // GDI device name, e.g. \\.\DISPLAY1
    std::wstring name; // EDID / WMI friendly name, or the device name
    bool internal = false;
    Channel channel = Channel::None;
    int percent = -1; // -1 when unknown
    int vcpMax = 100; // DDC maximum for VCP 0x10
    bool adjustable = false;
};

// Drops duplicate ids (first wins) and sorts internal panels first, then by
// device name. Stable, so equal keys keep their discovery order. Called after
// every rebuild (startup, WM_DISPLAYCHANGE, hot-plug).
std::vector<MonitorInfo> normalizeMonitors(std::vector<MonitorInfo> in);

// ---- gestures ------------------------------------------------------------------

enum class GestureKind { None, Wheel, Click };

// One low-level mouse event, normalized. Modifiers come from
// GetAsyncKeyState at hook time; `injected` from LLMHF_INJECTED.
struct GestureEvent {
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
    bool win = false;
    bool injected = false;
    bool wheel = false;
    int wheelDelta = 0; // raw signed delta; only meaningful when wheel
    bool leftDown = false;
};

struct GestureDecision {
    bool swallow = false;
    GestureKind kind = GestureKind::None;
    int steps = 0; // signed; valid for Wheel

    static GestureDecision pass() { return {}; }
};

// Ctrl+Alt+wheel and Ctrl+Alt+left-down are ours; everything else — injected
// input, Ctrl-only (the volume plugin's gesture), Alt-only, Shift/Win combos,
// plain clicks — passes through untouched.
GestureDecision decideGesture(const GestureEvent& e);

// High-precision touchpads emit many small deltas (e.g. 8 per pixel); apply
// one adjustment per full notch and keep the remainder for later.
class WheelAccumulator {
public:
    // Feed one raw delta; returns the number of full steps to apply now
    // (positive = up). A direction change discards the stale remainder.
    int add(int delta);

    int remainder() const { return m_remainder; }
    void reset() { m_remainder = 0; }

private:
    int m_remainder = 0;
};

// ---- percent math --------------------------------------------------------------

int clampPercent(int v); // clamp to 0..100
// current + steps * percentPerStep, clamped to 0..100.
int steppedPercent(int current, int steps, int percentPerStep);

// DDC/CI VCP 0x10 values are 0..max, not 0..100. Both directions round and
// clamp; a non-positive max yields 0.
int vcpToPercent(int value, int max);
int percentToVcp(int percent, int max);

// Rounded mean of the values; -1 when the list is empty.
int averagePercent(const std::vector<int>& values);

// ---- warm filter ---------------------------------------------------------------

// Fills a 3x256 gamma ramp: R is untouched, G and B fall off linearly with
// strength (0 = identity, 100 = deepest). Values stay inside 0..65535.
void buildWarmLut(int strength, std::uint16_t lut[3][256]);

// ---- the `bright` query --------------------------------------------------------

struct BrightnessQuery {
    enum class Kind { List, Set, Invalid };
    Kind kind = Kind::List;
    int percent = 0; // unclamped; the caller clamps and reports
};

// "" -> List; "70" / "70%" / " 70 " -> Set 70; "105" -> Set 105 (clamped
// later); "abc" / "7 0" / "%" -> Invalid.
BrightnessQuery parseBrightnessQuery(const std::string& text);

} // namespace br
