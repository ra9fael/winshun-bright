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

// ---- rectangles ----------------------------------------------------------------

// A monitor rectangle in virtual-screen pixels. Kept free of windows.h so the
// placement and targeting rules stay unit-testable.
struct Rect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    bool empty() const { return right <= left || bottom <= top; }
    bool contains(int x, int y) const
    {
        return !empty() && x >= left && x < right && y >= top && y < bottom;
    }
    // Squared distance from the point to the rectangle; 0 inside.
    long long distanceSquared(int x, int y) const;
};

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
    // The panel the gesture changed, whatever the pointer does afterwards.
    TargetMonitor,
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
    // Desktop area where the display lives: what lets a gesture act on the
    // panel under the pointer instead of on every panel.
    Rect bounds;
};

// Drops duplicate ids (first wins) and sorts internal panels first, then by
// device name. Stable, so equal keys keep their discovery order. Called after
// every rebuild (startup, WM_DISPLAYCHANGE, hot-plug).
std::vector<MonitorInfo> normalizeMonitors(std::vector<MonitorInfo> in);

// The display whose rectangle holds the point, else the nearest one (so a
// gesture near a bezel still lands somewhere sensible). `adjustableOnly`
// skips the panels a gesture cannot change. Index into `monitors`, or -1.
int monitorIndexAtPoint(const std::vector<MonitorInfo>& monitors, int x, int y,
    bool adjustableOnly = true);

// ---- gestures ------------------------------------------------------------------

enum class GestureKind { None, Wheel, Click };

// The mouse-chord the wheel and click gestures react to. The default is the
// historical Ctrl+Alt; a settings choice can move it out of the volume
// plugin's Ctrl-only way.
struct GestureModifiers {
    bool ctrl = true;
    bool alt = true;
    bool shift = false;
    bool win = false;

    static GestureModifiers ctrlAlt() { return {}; }
    // Parses "ctrl+alt", "alt+shift", "win+alt", "ctrl+shift+alt"; an empty or
    // unknown value yields the default.
    static GestureModifiers parse(const std::string& text);
    std::string text() const; // "ctrl+alt"
    // The modifiers actually held now match this chord exactly.
    bool matches(bool ctrlDown, bool altDown, bool shiftDown, bool winDown) const;
};

// Which displays a gesture changes.
enum class GestureTarget {
    MonitorUnderMouse, // the panel the pointer is on
    All,
};

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
    int x = 0; // pointer position at the event, virtual-screen pixels
    int y = 0;
    long long eventTimeMs = 0; // monotonic time captured by the hook thread
};

struct GestureDecision {
    bool swallow = false;
    bool stopEgg = false; // physical click notification; never swallows input
    GestureKind kind = GestureKind::None;
    int steps = 0; // signed; valid for Wheel
    int x = 0;     // where the gesture happened (target picking, overlay)
    int y = 0;
    long long eventTimeMs = 0;

    static GestureDecision pass() { return {}; }
};

// The chord's wheel and left-down are ours; everything else — injected input,
// other modifier combinations (Ctrl-only belongs to the volume plugin), plain
// clicks — passes through untouched. An empty chord means gestures are off.
GestureDecision decideGesture(const GestureEvent& e, const GestureModifiers& chord = GestureModifiers::ctrlAlt());


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

// ---- software dimming ------------------------------------------------------------

// Per mille of black the full-screen layer paints for a simulated brightness,
// so a panel without WMI/DDC can still look dimmer. 100% is invisible (0), 0%
// is the cap the user allowed (maxDark, 0..90); the result is 0..255 alpha,
// which is what SetLayeredWindowAttributes takes.
int dimAlpha(int percent, int maxDark);

// ---- the night mushroom ----------------------------------------------------------

// Local-time predicate used by older trigger rules and their boundary tests.
bool isNight(int minutesOfDay);

constexpr int kEggUpCountMs = 4000; // continuous up-scrolling that triggers it
constexpr int kEggGapMs = 10000; // tolerate touchpad/scroll pauses up to ten seconds
constexpr int kEggAnimationMs = 1800; // brief mushroom animation
static_assert(kEggUpCountMs == 4000, "the trigger is four seconds by design");

// Watches upward wheel events on one monitor. Pure state: fed one event at a
// time, it says when the persistent egg should start.
class EggTracker {
public:
    struct Context {
        int percent = 0;        // the target panel's brightness
        int minutesOfDay = 0;   // local time
        bool night = false;     // isNight(minutesOfDay), computed by the caller
        bool eggEnabled = true;
        bool sameMonitor = true; // the pointer stayed on the panel being watched
    };

    // Returns true once after sustained upward scrolling on one monitor.
    // A downward step, long pause, monitor change, or disabled setting resets the count.
    bool feed(int steps, const Context& context, long long nowMs);

    void reset();
    bool counting() const { return m_startedAt >= 0; }
    int elapsedMs(long long nowMs) const;

private:
    long long m_startedAt = -1; // first up-step of the current run
    long long m_lastUpAt = -1;
};

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
