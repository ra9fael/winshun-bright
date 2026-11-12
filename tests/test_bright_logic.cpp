// test_bright_logic.cpp — exhaustive tests for the pure core: gesture
// decisions, wheel accumulation, percent/VCP math, the warm LUT, monitor list
// normalization and the `bright` query parser. No display, WMI or DDC access.
#include "bright_logic.h"
#include "egg_effects.h"
#include "settings.h"
#include "test_harness.h"

#include <winshun.h>

using namespace br;

namespace {

MonitorInfo monitor(const wchar_t* id, const wchar_t* name, bool internal, Channel channel, int percent)
{
    MonitorInfo m;
    m.id = id;
    m.name = name;
    m.internal = internal;
    m.channel = channel;
    m.percent = percent;
    m.adjustable = channel != Channel::None;
    return m;
}

GestureEvent ctrlAltWheel(int delta)
{
    GestureEvent e;
    e.ctrl = true;
    e.alt = true;
    e.wheel = true;
    e.wheelDelta = delta;
    return e;
}

} // namespace

// ---- gesture decisions ---------------------------------------------------------

TEST("gesture: ctrl+alt+wheel up is ours, one step up")
{
    const GestureDecision d = decideGesture(ctrlAltWheel(kWheelDelta));
    CHECK(d.swallow);
    CHECK_EQ(d.kind, GestureKind::Wheel);
    CHECK_EQ(d.steps, 1);
}

TEST("gesture: ctrl+alt+wheel down is one step down")
{
    const GestureDecision d = decideGesture(ctrlAltWheel(-kWheelDelta));
    CHECK(d.swallow);
    CHECK_EQ(d.steps, -1);
}

TEST("gesture: ctrl+alt+left-down toggles warm and signals egg stop")
{
    GestureEvent e;
    e.ctrl = true;
    e.alt = true;
    e.leftDown = true;
    const GestureDecision d = decideGesture(e);
    CHECK(d.swallow);
    CHECK(d.stopEgg);
    CHECK_EQ(d.kind, GestureKind::Click);
}

TEST("gesture: any physical left-down stops egg without swallowing")
{
    GestureEvent e;
    e.leftDown = true;
    e.x = 901;
    e.y = 377;
    const GestureDecision d = decideGesture(e);
    CHECK(!d.swallow);
    CHECK(d.stopEgg);
    CHECK_EQ(d.x, 901);
    CHECK_EQ(d.y, 377);
}

TEST("gesture: injected left-down does not stop the egg")
{
    GestureEvent e;
    e.leftDown = true;
    e.injected = true;
    const GestureDecision d = decideGesture(e);
    CHECK(!d.swallow);
    CHECK(!d.stopEgg);
}

TEST("gesture: ctrl only passes through (the volume plugin's gesture)")
{
    GestureEvent e = ctrlAltWheel(kWheelDelta);
    e.alt = false;
    CHECK(!decideGesture(e).swallow);
}

TEST("gesture: alt only passes through")
{
    GestureEvent e = ctrlAltWheel(kWheelDelta);
    e.ctrl = false;
    CHECK(!decideGesture(e).swallow);
}

TEST("gesture: shift combos pass through")
{
    GestureEvent e = ctrlAltWheel(kWheelDelta);
    e.shift = true;
    CHECK(!decideGesture(e).swallow);

    GestureEvent click;
    click.ctrl = true;
    click.alt = true;
    click.shift = true;
    click.leftDown = true;
    CHECK(!decideGesture(click).swallow);
}

TEST("gesture: win combos pass through")
{
    GestureEvent e = ctrlAltWheel(kWheelDelta);
    e.win = true;
    CHECK(!decideGesture(e).swallow);
}

TEST("gesture: a click without alt passes through")
{
    GestureEvent e;
    e.ctrl = true;
    e.leftDown = true;
    CHECK(!decideGesture(e).swallow);
}

TEST("gesture: injected input is never swallowed")
{
    GestureEvent e = ctrlAltWheel(kWheelDelta);
    e.injected = true;
    CHECK(!decideGesture(e).swallow);

    GestureEvent click;
    click.ctrl = true;
    click.alt = true;
    click.injected = true;
    click.leftDown = true;
    CHECK(!decideGesture(click).swallow);
}

TEST("gesture: plain wheel and plain click pass through")
{
    GestureEvent wheel;
    wheel.wheel = true;
    wheel.wheelDelta = kWheelDelta;
    CHECK(!decideGesture(wheel).swallow);

    GestureEvent click;
    click.leftDown = true;
    CHECK(!decideGesture(click).swallow);
}

TEST("gesture: a zero wheel delta is not an adjustment")
{
    CHECK(!decideGesture(ctrlAltWheel(0)).swallow);
}

TEST("gesture: an empty event is a pass")
{
    CHECK(!decideGesture(GestureEvent {}).swallow);
    GestureEvent modified;
    modified.ctrl = true;
    modified.alt = true;
    CHECK(!decideGesture(modified).swallow); // neither wheel nor click
}

// ---- wheel accumulation --------------------------------------------------------

TEST("accumulator: single standard notch yields one step")
{
    WheelAccumulator a;
    CHECK_EQ(a.add(kWheelDelta), 1);
    CHECK_EQ(a.add(-kWheelDelta), -1);
}

TEST("accumulator: two half notches yield one step and keep the remainder")
{
    WheelAccumulator a;
    CHECK_EQ(a.add(60), 0);
    CHECK_EQ(a.add(60), 1);
    CHECK_EQ(a.remainder(), 0);
}

TEST("accumulator: high-resolution stream accumulates into periodic steps")
{
    WheelAccumulator a;
    int steps = 0;
    for (int i = 0; i < 1000; ++i)
        steps += a.add(8); // 1000 * 8 = 8000 = 66.67 notches
    CHECK_EQ(steps, 66);
    CHECK_EQ(a.remainder(), 80);
}

TEST("accumulator: direction change discards the stale remainder")
{
    WheelAccumulator a;
    CHECK_EQ(a.add(100), 0);
    CHECK_EQ(a.add(-100), 0); // +100 discarded, not -200 total
    CHECK_EQ(a.add(-20), -1);
}

TEST("accumulator: a multi-notch burst yields several steps at once")
{
    WheelAccumulator a;
    CHECK_EQ(a.add(5 * kWheelDelta), 5);
    CHECK_EQ(a.add(-2 * kWheelDelta), -2);
}

TEST("accumulator: zero delta is a no-op")
{
    WheelAccumulator a;
    CHECK_EQ(a.add(0), 0);
    CHECK_EQ(a.add(60), 0);
    CHECK_EQ(a.add(0), 0);
    CHECK_EQ(a.add(60), 1);
}

TEST("accumulator: reset drops the remainder")
{
    WheelAccumulator a;
    CHECK_EQ(a.add(100), 0);
    a.reset();
    CHECK_EQ(a.remainder(), 0);
    CHECK_EQ(a.add(20), 0);
}

// ---- percent math --------------------------------------------------------------

TEST("percent: clamping at both ends")
{
    CHECK_EQ(clampPercent(-5), 0);
    CHECK_EQ(clampPercent(0), 0);
    CHECK_EQ(clampPercent(55), 55);
    CHECK_EQ(clampPercent(100), 100);
    CHECK_EQ(clampPercent(105), 100);
}

TEST("percent: stepping clamps at 0 and 100")
{
    CHECK_EQ(steppedPercent(100, 1, 5), 100);
    CHECK_EQ(steppedPercent(0, -1, 5), 0);
    CHECK_EQ(steppedPercent(98, 1, 5), 100); // overflow caps at 100
    CHECK_EQ(steppedPercent(2, -1, 5), 0);
    CHECK_EQ(steppedPercent(50, 3, 5), 65);
    CHECK_EQ(steppedPercent(50, -3, 5), 35);
}

TEST("percent: out-of-range percentPerStep is clamped, not trusted")
{
    CHECK_EQ(steppedPercent(50, 1, 0), 51);
    CHECK_EQ(steppedPercent(50, 1, 99), 60);
    CHECK_EQ(steppedPercent(50, -1, -5), 49);
}

// ---- DDC/CI VCP conversion -----------------------------------------------------

TEST("vcp: a max of 100 is an identity mapping")
{
    CHECK_EQ(vcpToPercent(0, 100), 0);
    CHECK_EQ(vcpToPercent(50, 100), 50);
    CHECK_EQ(vcpToPercent(100, 100), 100);
    CHECK_EQ(percentToVcp(50, 100), 50);
}

TEST("vcp: a max other than 100 converts both ways")
{
    CHECK_EQ(vcpToPercent(0, 255), 0);
    CHECK_EQ(vcpToPercent(255, 255), 100);
    CHECK_EQ(vcpToPercent(128, 255), 50);
    CHECK_EQ(percentToVcp(100, 255), 255);
    CHECK_EQ(percentToVcp(0, 255), 0);
    CHECK_EQ(percentToVcp(50, 255), 128);
}

TEST("vcp: percent survives a round trip within one point")
{
    for (const int max : { 100, 255, 200, 63 }) {
        for (int p = 0; p <= 100; p += 5) {
            const int back = vcpToPercent(percentToVcp(p, max), max);
            CHECK(back - p <= 1 && p - back <= 1);
        }
    }
}

TEST("vcp: out-of-range values are clamped")
{
    CHECK_EQ(vcpToPercent(-10, 100), 0);
    CHECK_EQ(vcpToPercent(200, 100), 100);
    CHECK_EQ(percentToVcp(-20, 100), 0);
    CHECK_EQ(percentToVcp(150, 100), 100);
}

TEST("vcp: a non-positive max degrades to zero")
{
    CHECK_EQ(vcpToPercent(50, 0), 0);
    CHECK_EQ(vcpToPercent(50, -1), 0);
    CHECK_EQ(percentToVcp(50, 0), 0);
    CHECK_EQ(percentToVcp(50, -1), 0);
}

TEST("average: empty list has no value, otherwise rounded mean")
{
    CHECK_EQ(averagePercent({}), -1);
    CHECK_EQ(averagePercent({ 60 }), 60);
    CHECK_EQ(averagePercent({ 50, 60 }), 55);
    CHECK_EQ(averagePercent({ 50, 51 }), 51); // 50.5 rounds up
    CHECK_EQ(averagePercent({ 0, 100, 50 }), 50);
}

// ---- warm filter LUT -----------------------------------------------------------

TEST("warm: strength 0 is an identity ramp")
{
    std::uint16_t lut[3][256];
    buildWarmLut(0, lut);
    for (int i = 0; i < 256; ++i) {
        CHECK_EQ(lut[0][i], static_cast<std::uint16_t>(i * 257));
        CHECK_EQ(lut[1][i], lut[0][i]);
        CHECK_EQ(lut[2][i], lut[0][i]);
    }
}

TEST("warm: strength 100 is the deepest filter")
{
    std::uint16_t lut[3][256];
    buildWarmLut(100, lut);
    CHECK_EQ(lut[0][255], static_cast<std::uint16_t>(65535));
    CHECK(lut[1][255] < lut[0][255]);
    CHECK(lut[2][255] < lut[1][255]);
    CHECK_EQ(lut[0][0], 0);
}

TEST("warm: red is untouched, green falls less than blue")
{
    std::uint16_t lut[3][256];
    buildWarmLut(50, lut);
    for (int i = 1; i < 256; ++i) {
        CHECK_EQ(lut[0][i], static_cast<std::uint16_t>(i * 257));
        CHECK(lut[1][i] < lut[0][i]);
        CHECK(lut[2][i] < lut[1][i]);
    }
}

TEST("warm: the ramps stay monotonic and inside the gamma range")
{
    std::uint16_t lut[3][256];
    for (const int strength : { 1, 25, 50, 75, 100 }) {
        buildWarmLut(strength, lut);
        for (int channel = 0; channel < 3; ++channel) {
            CHECK_EQ(lut[channel][0], 0);
            for (int i = 1; i < 256; ++i) {
                CHECK(lut[channel][i] >= lut[channel][i - 1]); // never decreases
                CHECK(lut[channel][i] <= 65535);
            }
        }
    }
}

TEST("warm: out-of-range strengths are clamped")
{
    std::uint16_t low[3][256];
    std::uint16_t zero[3][256];
    buildWarmLut(-40, low);
    buildWarmLut(0, zero);
    CHECK_EQ(low[2][255], zero[2][255]);

    std::uint16_t high[3][256];
    std::uint16_t hundred[3][256];
    buildWarmLut(250, high);
    buildWarmLut(100, hundred);
    CHECK_EQ(high[2][255], hundred[2][255]);
}

// ---- monitor list normalization -------------------------------------------------

TEST("monitors: duplicate ids are dropped, the first one wins")
{
    std::vector<MonitorInfo> in = {
        monitor(L"\\\\.\\DISPLAY1", L"Internal", true, Channel::Wmi, 70),
        monitor(L"\\\\.\\DISPLAY1", L"Duplicate", false, Channel::Ddc, 10),
        monitor(L"\\\\.\\DISPLAY2", L"External", false, Channel::Ddc, 40),
    };
    const std::vector<MonitorInfo> out = normalizeMonitors(in);
    CHECK_EQ(out.size(), std::size_t { 2 });
    CHECK_EQ(out[0].name, std::wstring(L"Internal"));
}

TEST("monitors: internal panels sort first, then by device name")
{
    std::vector<MonitorInfo> in = {
        monitor(L"\\\\.\\DISPLAY3", L"C", false, Channel::Ddc, 30),
        monitor(L"\\\\.\\DISPLAY2", L"B", true, Channel::Wmi, 80),
        monitor(L"\\\\.\\DISPLAY1", L"A", false, Channel::Ddc, 50),
    };
    const std::vector<MonitorInfo> out = normalizeMonitors(in);
    CHECK_EQ(out.size(), std::size_t { 3 });
    CHECK_EQ(out[0].id, std::wstring(L"\\\\.\\DISPLAY2"));
    CHECK_EQ(out[1].id, std::wstring(L"\\\\.\\DISPLAY1"));
    CHECK_EQ(out[2].id, std::wstring(L"\\\\.\\DISPLAY3"));
}

TEST("monitors: an empty list stays empty")
{
    CHECK_EQ(normalizeMonitors({}).size(), std::size_t { 0 });
}

TEST("monitors: a hot-plug rebuild keeps the remaining order stable")
{
    // Snapshot before the external display is unplugged.
    std::vector<MonitorInfo> before = {
        monitor(L"\\\\.\\DISPLAY2", L"External", false, Channel::Ddc, 44),
        monitor(L"\\\\.\\DISPLAY1", L"Internal", true, Channel::Wmi, 80),
    };
    const std::vector<MonitorInfo> sortedBefore = normalizeMonitors(before);
    CHECK_EQ(sortedBefore.size(), std::size_t { 2 });
    CHECK_EQ(sortedBefore[0].id, std::wstring(L"\\\\.\\DISPLAY1"));

    // Rebuild after unplugging: only the internal panel is left.
    std::vector<MonitorInfo> after = {
        monitor(L"\\\\.\\DISPLAY1", L"Internal", true, Channel::Wmi, 80),
    };
    const std::vector<MonitorInfo> sortedAfter = normalizeMonitors(after);
    CHECK_EQ(sortedAfter.size(), std::size_t { 1 });
    CHECK_EQ(sortedAfter[0].id, sortedBefore[0].id);

    // Plugging it back in restores the same two rows, in the same order.
    std::vector<MonitorInfo> again = {
        monitor(L"\\\\.\\DISPLAY1", L"Internal", true, Channel::Wmi, 80),
        monitor(L"\\\\.\\DISPLAY2", L"External", false, Channel::Ddc, 44),
    };
    const std::vector<MonitorInfo> sortedAgain = normalizeMonitors(again);
    CHECK_EQ(sortedAgain.size(), sortedBefore.size());
    for (std::size_t i = 0; i < sortedAgain.size(); ++i)
        CHECK_EQ(sortedAgain[i].id, sortedBefore[i].id);
}

TEST("monitors: unsupported displays keep their slot")
{
    std::vector<MonitorInfo> in = {
        monitor(L"\\\\.\\DISPLAY1", L"Internal", true, Channel::Wmi, 70),
        monitor(L"\\\\.\\DISPLAY2", L"Dock", false, Channel::None, -1),
    };
    const std::vector<MonitorInfo> out = normalizeMonitors(in);
    CHECK_EQ(out.size(), std::size_t { 2 });
    CHECK(!out[1].adjustable);
    CHECK_EQ(out[1].percent, -1);
}

// ---- the `bright` query parser --------------------------------------------------

TEST("query: empty text lists the monitors")
{
    CHECK_EQ(parseBrightnessQuery("").kind, BrightnessQuery::Kind::List);
    CHECK_EQ(parseBrightnessQuery("   ").kind, BrightnessQuery::Kind::List);
    CHECK_EQ(parseBrightnessQuery("\t\r\n").kind, BrightnessQuery::Kind::List);
}

TEST("query: a plain number sets that percentage")
{
    const BrightnessQuery q = parseBrightnessQuery("70");
    CHECK_EQ(q.kind, BrightnessQuery::Kind::Set);
    CHECK_EQ(q.percent, 70);
    CHECK_EQ(parseBrightnessQuery("0").percent, 0);
    CHECK_EQ(parseBrightnessQuery("100").percent, 100);
}

TEST("query: surrounding space and a percent sign are accepted")
{
    CHECK_EQ(parseBrightnessQuery("  70  ").percent, 70);
    CHECK_EQ(parseBrightnessQuery("70%").percent, 70);
    CHECK_EQ(parseBrightnessQuery(" 45 % ").percent, 45);
}

TEST("query: out-of-range numbers are kept for the caller to clamp")
{
    const BrightnessQuery high = parseBrightnessQuery("105");
    CHECK_EQ(high.kind, BrightnessQuery::Kind::Set);
    CHECK_EQ(high.percent, 105);

    const BrightnessQuery negative = parseBrightnessQuery("-5");
    CHECK_EQ(negative.kind, BrightnessQuery::Kind::Set);
    CHECK_EQ(negative.percent, -5);
}

TEST("query: non-numeric text is invalid")
{
    CHECK_EQ(parseBrightnessQuery("abc").kind, BrightnessQuery::Kind::Invalid);
    CHECK_EQ(parseBrightnessQuery("7 0").kind, BrightnessQuery::Kind::Invalid);
    CHECK_EQ(parseBrightnessQuery("70%%").kind, BrightnessQuery::Kind::Invalid);
    CHECK_EQ(parseBrightnessQuery("%").kind, BrightnessQuery::Kind::Invalid);
    CHECK_EQ(parseBrightnessQuery("7a").kind, BrightnessQuery::Kind::Invalid);
}

TEST("query: a huge number saturates instead of overflowing")
{
    const BrightnessQuery q = parseBrightnessQuery("99999999999999999999");
    CHECK_EQ(q.kind, BrightnessQuery::Kind::Set);
    CHECK(q.percent > 100);
}

// ---- settings -------------------------------------------------------------------

TEST("settings: defaults when the object is empty or not an object")
{
    const Settings a = parseSettings(winshun::Json(winshun::Json::Object {}));
    CHECK_EQ(a.percentPerStep, 5);
    CHECK(a.gestureEnabled);
    CHECK(a.showOverlay);
    CHECK_EQ(a.overlayMs, 600);
    CHECK_EQ(a.warmStrength, 0);
    CHECK(!a.warmRemember);
    CHECK_EQ(a.overlayAnchor, OverlayAnchor::BottomCenter);

    const Settings b = parseSettings(winshun::Json("nonsense"));
    CHECK_EQ(b.percentPerStep, 5);
    CHECK_EQ(b.overlayMs, 600);
}

TEST("settings: valid values are kept")
{
    winshun::Json s(winshun::Json::Object {});
    s.set("percentPerStep", 8);
    s.set("gestureEnabled", false);
    s.set("showOverlay", false);
    s.set("overlayMs", 1500);
    s.set("warmStrength", 40);
    s.set("warmRemember", true);
    const Settings out = parseSettings(s);
    CHECK_EQ(out.percentPerStep, 8);
    CHECK(!out.gestureEnabled);
    CHECK(!out.showOverlay);
    CHECK_EQ(out.overlayMs, 1500);
    CHECK_EQ(out.warmStrength, 40);
    CHECK(out.warmRemember);
}

TEST("settings: percentPerStep outside 1-10 falls back to the default")
{
    winshun::Json low(winshun::Json::Object {});
    low.set("percentPerStep", 0);
    CHECK_EQ(parseSettings(low).percentPerStep, 5);

    winshun::Json high(winshun::Json::Object {});
    high.set("percentPerStep", 11);
    CHECK_EQ(parseSettings(high).percentPerStep, 5);

    winshun::Json text(winshun::Json::Object {});
    text.set("percentPerStep", "fast");
    CHECK_EQ(parseSettings(text).percentPerStep, 5);

    winshun::Json top(winshun::Json::Object {});
    top.set("percentPerStep", 10);
    CHECK_EQ(parseSettings(top).percentPerStep, 10);
}

TEST("settings: overlayMs outside 300-3000 falls back to the default")
{
    winshun::Json low(winshun::Json::Object {});
    low.set("overlayMs", 299);
    CHECK_EQ(parseSettings(low).overlayMs, 600);

    winshun::Json high(winshun::Json::Object {});
    high.set("overlayMs", 3001);
    CHECK_EQ(parseSettings(high).overlayMs, 600);

    winshun::Json lowEdge(winshun::Json::Object {});
    lowEdge.set("overlayMs", 300);
    CHECK_EQ(parseSettings(lowEdge).overlayMs, 300);

    winshun::Json highEdge(winshun::Json::Object {});
    highEdge.set("overlayMs", 3000);
    CHECK_EQ(parseSettings(highEdge).overlayMs, 3000);
}

TEST("settings: warmStrength outside 0-100 falls back to the default")
{
    winshun::Json low(winshun::Json::Object {});
    low.set("warmStrength", -1);
    CHECK_EQ(parseSettings(low).warmStrength, 0);

    winshun::Json high(winshun::Json::Object {});
    high.set("warmStrength", 101);
    CHECK_EQ(parseSettings(high).warmStrength, 0);

    winshun::Json edge(winshun::Json::Object {});
    edge.set("warmStrength", 100);
    CHECK_EQ(parseSettings(edge).warmStrength, 100);
}

TEST("settings: an unknown overlay anchor falls back to bottom-center")
{
    winshun::Json s(winshun::Json::Object {});
    s.set("overlayAnchor", "nope");
    CHECK_EQ(parseSettings(s).overlayAnchor, OverlayAnchor::BottomCenter);
}

TEST("settings: every declared anchor is understood")
{
    const struct {
        const char* value;
        OverlayAnchor anchor;
    } cases[] = {
        { "center", OverlayAnchor::Center },
        { "top-left", OverlayAnchor::TopLeft },
        { "top-center", OverlayAnchor::TopCenter },
        { "top-right", OverlayAnchor::TopRight },
        { "left-center", OverlayAnchor::LeftCenter },
        { "right-center", OverlayAnchor::RightCenter },
        { "bottom-left", OverlayAnchor::BottomLeft },
        { "bottom-center", OverlayAnchor::BottomCenter },
        { "bottom-right", OverlayAnchor::BottomRight },
        { "monitor-under-mouse", OverlayAnchor::MonitorUnderMouse },
        { "target-monitor", OverlayAnchor::TargetMonitor },
    };
    for (const auto& c : cases) {
        winshun::Json s(winshun::Json::Object {});
        s.set("overlayAnchor", c.value);
        CHECK_EQ(parseSettings(s).overlayAnchor, c.anchor);
    }
}

TEST("settings: offsets are clamped to +/-4000")
{
    winshun::Json s(winshun::Json::Object {});
    s.set("overlayOffsetX", 12);
    s.set("overlayOffsetY", -34);
    const Settings out = parseSettings(s);
    CHECK_EQ(out.overlayOffsetX, 12);
    CHECK_EQ(out.overlayOffsetY, -34);

    winshun::Json extreme(winshun::Json::Object {});
    extreme.set("overlayOffsetX", 99999);
    extreme.set("overlayOffsetY", -99999);
    const Settings clamped = parseSettings(extreme);
    CHECK_EQ(clamped.overlayOffsetX, 4000);
    CHECK_EQ(clamped.overlayOffsetY, -4000);
}

// ---- the configurable chord and the per-monitor target --------------------------

TEST("chord: ctrl+alt is the default when the setting is missing or empty")
{
    const Settings defaults = parseSettings(winshun::Json(winshun::Json::Object {}));
    CHECK_EQ(defaults.gestureModifiers.text(), std::string("ctrl+alt"));

    winshun::Json empty(winshun::Json::Object {});
    empty.set("gestureModifiers", "");
    CHECK_EQ(parseSettings(empty).gestureModifiers.text(), std::string("ctrl+alt"));
}

TEST("chord: parses and re-serializes the modifier names")
{
    const struct {
        const char* text;
        const char* expected;
    } cases[] = {
        { "ctrl+alt", "ctrl+alt" },
        { "alt+ctrl", "ctrl+alt" },
        { "alt+shift", "alt+shift" },
        { "win+alt", "win+alt" },
        { "Ctrl+Shift+Alt", "ctrl+alt+shift" },
        { "meta", "win" },
    };
    for (const auto& c : cases) {
        winshun::Json s(winshun::Json::Object {});
        s.set("gestureModifiers", c.text);
        CHECK_EQ(parseSettings(s).gestureModifiers.text(), std::string(c.expected));
    }
}

TEST("chord: an unknown name or no modifier falls back to ctrl+alt")
{
    CHECK_EQ(GestureModifiers::parse("hyper").text(), std::string("ctrl+alt"));
    CHECK_EQ(GestureModifiers::parse("ctrl+hyper").text(), std::string("ctrl+alt"));
}

TEST("chord: an empty chord matches nothing, so gestures cannot swallow everything")
{
    GestureModifiers none;
    none.ctrl = false;
    none.alt = false;
    CHECK(!none.matches(false, false, false, false));
    CHECK(!none.matches(true, false, false, false));

    GestureEvent plainWheel;
    plainWheel.wheel = true;
    plainWheel.wheelDelta = kWheelDelta;
    CHECK(!decideGesture(plainWheel, none).swallow);
}

TEST("chord: a re-mapped chord takes over, the old one passes through")
{
    const GestureModifiers altShift = GestureModifiers::parse("alt+shift");

    GestureEvent altShiftWheel;
    altShiftWheel.alt = true;
    altShiftWheel.shift = true;
    altShiftWheel.wheel = true;
    altShiftWheel.wheelDelta = kWheelDelta;
    CHECK(decideGesture(altShiftWheel, altShift).swallow);

    GestureEvent ctrlAltWheelEvent;
    ctrlAltWheelEvent.ctrl = true;
    ctrlAltWheelEvent.alt = true;
    ctrlAltWheelEvent.wheel = true;
    ctrlAltWheelEvent.wheelDelta = kWheelDelta;
    CHECK(!decideGesture(ctrlAltWheelEvent, altShift).swallow);
    CHECK(decideGesture(ctrlAltWheelEvent, GestureModifiers::ctrlAlt()).swallow);
}

TEST("gesture: the pointer position travels with the decision")
{
    GestureEvent e = ctrlAltWheel(kWheelDelta);
    e.x = 2400;
    e.y = 720;
    const GestureDecision d = decideGesture(e);
    CHECK(d.swallow);
    CHECK_EQ(d.x, 2400);
    CHECK_EQ(d.y, 720);
}

TEST("rect: contains its area and measures the distance outside it")
{
    const Rect r { 0, 0, 1920, 1080 };
    CHECK(r.contains(0, 0));
    CHECK(r.contains(1919, 1079));
    CHECK(!r.contains(1920, 0)); // half-open on the right/bottom edge
    CHECK(!r.contains(0, 1080));
    CHECK_EQ(r.distanceSquared(100, 100), 0LL);
    // One pixel outside the area is one away: 1930 is 11 pixels past 1919.
    CHECK_EQ(r.distanceSquared(1930, 100), 11LL * 11);
    CHECK_EQ(r.distanceSquared(1920, 100), 1LL);
    CHECK_EQ(Rect {}.distanceSquared(5, 5), -1LL); // empty: no distance
}

namespace {

std::vector<MonitorInfo> twoPanels()
{
    std::vector<MonitorInfo> monitors;
    MonitorInfo left = monitor(L"\\\\.\\DISPLAY1", L"Left", true, Channel::Wmi, 50);
    left.bounds = Rect { 0, 0, 1920, 1080 };
    MonitorInfo right = monitor(L"\\\\.\\DISPLAY2", L"Right", false, Channel::Ddc, 70);
    right.bounds = Rect { 1920, 0, 3840, 1080 };
    monitors.push_back(std::move(left));
    monitors.push_back(std::move(right));
    return normalizeMonitors(std::move(monitors));
}

} // namespace

TEST("target: the point picks the panel it is on")
{
    const std::vector<MonitorInfo> monitors = twoPanels();
    CHECK_EQ(monitorIndexAtPoint(monitors, 100, 100), 0);
    CHECK_EQ(monitorIndexAtPoint(monitors, 2000, 500), 1);
}

TEST("target: a point in the seam takes the nearest panel, never a random one")
{
    std::vector<MonitorInfo> monitors = twoPanels();
    // A gap between two panels (a monitor in the middle switched off).
    monitors[0].bounds = Rect { 0, 0, 1000, 1000 };
    monitors[1].bounds = Rect { 1100, 0, 2100, 1000 };
    CHECK_EQ(monitorIndexAtPoint(monitors, 1040, 500), 0); // 41 px from the left panel
    CHECK_EQ(monitorIndexAtPoint(monitors, 1060, 500), 1); // 40 px from the right panel
    CHECK_EQ(monitorIndexAtPoint(monitors, 1050, 500), 1); // 50 vs 51: the nearer wins
}

TEST("target: a point outside every panel still lands on one")
{
    const std::vector<MonitorInfo> monitors = twoPanels();
    CHECK_EQ(monitorIndexAtPoint(monitors, -500, -500), 0);
    CHECK_EQ(monitorIndexAtPoint(monitors, 9000, 500), 1);
}

TEST("target: an unadjustable panel under the pointer is skipped")
{
    std::vector<MonitorInfo> monitors = twoPanels();
    monitors[1].adjustable = false; // a TV through a receiver, a KVM
    CHECK_EQ(monitorIndexAtPoint(monitors, 2000, 500), 0); // the only one left
}

TEST("target: nothing adjustable yields -1")
{
    std::vector<MonitorInfo> monitors = twoPanels();
    monitors[0].adjustable = false;
    monitors[1].adjustable = false;
    CHECK_EQ(monitorIndexAtPoint(monitors, 100, 100), -1);
    CHECK_EQ(monitorIndexAtPoint({}, 0, 0), -1);
}

TEST("settings: gestureTarget defaults to the panel under the pointer")
{
    const Settings defaults = parseSettings(winshun::Json(winshun::Json::Object {}));
    CHECK_EQ(defaults.gestureTarget, GestureTarget::MonitorUnderMouse);

    winshun::Json all(winshun::Json::Object {});
    all.set("gestureTarget", "all");
    CHECK_EQ(parseSettings(all).gestureTarget, GestureTarget::All);

    winshun::Json unknown(winshun::Json::Object {});
    unknown.set("gestureTarget", "nope");
    CHECK_EQ(parseSettings(unknown).gestureTarget, GestureTarget::MonitorUnderMouse);
}

TEST("settings: the three step hotkeys are read and empty stays empty")
{
    winshun::Json s(winshun::Json::Object {});
    s.set("hotkeyUp", "Ctrl+Alt+F1");
    s.set("hotkeyDown", "Ctrl+Alt+F2");
    const Settings out = parseSettings(s);
    CHECK(out.hotkeyUp == L"Ctrl+Alt+F1");
    CHECK(out.hotkeyDown == L"Ctrl+Alt+F2");
    CHECK(out.hotkeyMax.empty()); // not set: nothing registered
}

// ---- software dimming ----------------------------------------------------------

TEST("dim: full brightness paints nothing, zero paints the cap")
{
    CHECK_EQ(dimAlpha(100, 60), 0);
    CHECK_EQ(dimAlpha(101, 60), 0); // clamped
    // 60% of 255 is 153, and 0% brightness uses all of it.
    CHECK_EQ(dimAlpha(0, 60), 153);
    CHECK_EQ(dimAlpha(50, 60), 77); // half of the cap, rounded half away from zero
}

TEST("dim: the cap is clamped to 0..90 and monotonic in brightness")
{
    CHECK_EQ(dimAlpha(0, -5), 0);
    CHECK_EQ(dimAlpha(0, 500), static_cast<int>(std::lround(90 * 255.0 / 100.0)));
    int previous = 256;
    for (int percent = 0; percent <= 100; ++percent) {
        const int alpha = dimAlpha(percent, 60);
        CHECK(alpha <= previous); // darker as the percent falls
        previous = alpha;
    }
    CHECK_EQ(dimAlpha(100, 90), 0);
}

TEST("dim: softwareMaxDark outside 0..90 falls back to the default")
{
    winshun::Json high(winshun::Json::Object {});
    high.set("softwareMaxDark", 120);
    CHECK_EQ(parseSettings(high).softwareMaxDark, 60);

    winshun::Json low(winshun::Json::Object {});
    low.set("softwareMaxDark", -1);
    CHECK_EQ(parseSettings(low).softwareMaxDark, 60);

    winshun::Json edge(winshun::Json::Object {});
    edge.set("softwareMaxDark", 90);
    CHECK_EQ(parseSettings(edge).softwareMaxDark, 90);
    edge.set("softwareMaxDark", 0);
    CHECK_EQ(parseSettings(edge).softwareMaxDark, 0);
}

TEST("settings: softwareBrightness defaults to off and reads the three modes")
{
    CHECK_EQ(parseSettings(winshun::Json(winshun::Json::Object {})).softwareBrightness,
        SoftwareDim::Off);

    const struct {
        const char* value;
        SoftwareDim mode;
    } cases[] = {
        { "off", SoftwareDim::Off },
        { "unsupported-only", SoftwareDim::UnsupportedOnly },
        { "always", SoftwareDim::Always },
        { "nonsense", SoftwareDim::Off },
    };
    for (const auto& c : cases) {
        winshun::Json s(winshun::Json::Object {});
        s.set("softwareBrightness", c.value);
        CHECK_EQ(parseSettings(s).softwareBrightness, c.mode);
    }
}

TEST("settings: the egg switch defaults to on and can be turned off")
{
    CHECK(parseSettings(winshun::Json(winshun::Json::Object {})).mushroomEgg);
    winshun::Json off(winshun::Json::Object {});
    off.set("mushroomEgg", false);
    CHECK(!parseSettings(off).mushroomEgg);
}

TEST("settings: an undiscovered egg honors host setting; discovered egg honors persisted toggle")
{
    CHECK(effectiveEggEnabled(false, true, false)); // first-run trigger remains enabled
    CHECK(!effectiveEggEnabled(false, false, true)); // user may disable before discovery
    CHECK(effectiveEggEnabled(true, false, true)); // persisted enable survives host refresh
    CHECK(!effectiveEggEnabled(true, true, false)); // persisted disable survives restart
}

TEST("settings: remoteDim defaults to on and can be turned off")
{
    // The layer is the only working dimmer in a remote session, so the
    // default keeps it on there even with softwareBrightness off.
    CHECK(parseSettings(winshun::Json(winshun::Json::Object {})).remoteDim);
    winshun::Json off(winshun::Json::Object {});
    off.set("remoteDim", false);
    CHECK(!parseSettings(off).remoteDim);
    winshun::Json on(winshun::Json::Object {});
    on.set("remoteDim", true);
    CHECK(parseSettings(on).remoteDim);
}

// ---- the night window ----------------------------------------------------------

TEST("night: 22:00 to 05:59 is night, 06:00 and daytime are not")
{
    CHECK(isNight(21 * 60 + 59) == false);
    CHECK(isNight(22 * 60) == true);      // 22:00 counts
    CHECK(isNight(23 * 60 + 59) == true);
    CHECK(isNight(0) == true);            // midnight
    CHECK(isNight(5 * 60 + 59) == true);
    CHECK(isNight(6 * 60) == false);      // 06:00 does not
    CHECK(isNight(12 * 60) == false);
    CHECK(isNight(21 * 60 + 59) == false);
}

TEST("night: out-of-range minutes of day wrap instead of misbehaving")
{
    CHECK(isNight(1440 + 23 * 60) == true); // 23:00 next day
    CHECK(isNight(-60) == true);            // 23:00
    CHECK(isNight(1440 + 12 * 60) == false);
}

// ---- the egg tracker -----------------------------------------------------------

namespace {

EggTracker::Context eggContext(int percent, bool night = true, bool enabled = true,
    bool sameMonitor = true)
{
    EggTracker::Context c;
    c.percent = percent;
    c.minutesOfDay = night ? 23 * 60 : 12 * 60;
    c.night = night;
    c.eggEnabled = enabled;
    c.sameMonitor = sameMonitor;
    return c;
}

} // namespace

TEST("egg: sustained upward scrolling fires once")
{
    EggTracker tracker;
    const EggTracker::Context context = eggContext(100);
    bool fired = false;
    // One step every half second until the elapsed time reaches the threshold.
    for (int ms = 0; ms <= kEggUpCountMs; ms += 500) {
        if (tracker.feed(1, context, ms))
            fired = true;
    }
    CHECK(fired);
    CHECK(!tracker.counting()); // consumed: the next one starts over
}

TEST("egg: the count needs a full four seconds")
{
    EggTracker tracker;
    const EggTracker::Context context = eggContext(100);
    bool fired = false;
    for (int ms = 0; ms < kEggUpCountMs; ms += 500)
        fired = fired || tracker.feed(1, context, ms);
    CHECK(!fired);
    CHECK(tracker.counting());
}

TEST("egg: a ten-second gap is accepted, a longer gap restarts")
{
    const EggTracker::Context context = eggContext(35, false);
    EggTracker boundary;
    boundary.feed(1, context, 0);
    boundary.feed(1, context, 1000);
    CHECK(boundary.feed(1, context, 1000 + kEggGapMs));
    CHECK(!boundary.counting());

    EggTracker restarted;
    restarted.feed(1, context, 0);
    restarted.feed(1, context, 1000);
    CHECK(!restarted.feed(1, context, 1000 + kEggGapMs + 1));
    CHECK(restarted.counting());
    CHECK_EQ(restarted.elapsedMs(1000 + kEggGapMs + 1), 0);
}

TEST("egg: a downward step clears the count")
{
    EggTracker tracker;
    const EggTracker::Context context = eggContext(100);
    tracker.feed(1, context, 0);
    CHECK(tracker.counting());
    CHECK(!tracker.feed(-1, context, 500));
    CHECK(!tracker.counting());
}

TEST("egg: brightness and time of day do not restrict the trigger")
{
    EggTracker tracker;
    bool fired = false;
    for (int ms = 0; ms <= kEggUpCountMs; ms += 500)
        fired = fired || tracker.feed(1, eggContext(35, false), ms);
    CHECK(fired);
}

TEST("egg: a disabled setting and another monitor refuse the trigger")
{
    EggTracker disabled;
    bool fired = false;
    for (int ms = 0; ms <= kEggUpCountMs; ms += 500)
        fired = fired || disabled.feed(1, eggContext(35, false, false), ms);
    CHECK(!fired);

    EggTracker moved;
    fired = false;
    for (int ms = 0; ms <= kEggUpCountMs; ms += 500)
        fired = fired || moved.feed(1, eggContext(35, false, true, false), ms);
    CHECK(!fired);
}

TEST("egg: reset forgets a run in progress")
{
    EggTracker tracker;
    tracker.feed(1, eggContext(100), 0);
    CHECK(tracker.counting());
    tracker.reset();
    CHECK(!tracker.counting());
    CHECK_EQ(tracker.elapsedMs(1000), 0);
}

TEST("egg: a context that says the monitor changed clears the run")
{
    EggTracker tracker;
    CHECK(!tracker.feed(1, eggContext(100), 0));
    CHECK(tracker.counting());

    // The worker reports sameMonitor=false as soon as the pointer lands on
    // another panel; that must abandon the count, not pause it.
    CHECK(!tracker.feed(1, eggContext(100, true, true, false), 500));
    CHECK(!tracker.counting());

    // Back on the original panel, the count starts from zero: ten more seconds.
    bool fired = false;
    for (int ms = 1000; ms <= 1000 + kEggUpCountMs; ms += 500)
        fired = fired || tracker.feed(1, eggContext(100), ms);
    CHECK(fired);
}

TEST("egg: persistent fullscreen effect uses a 3.8-second concept cycle")
{
    CHECK_EQ(kEggEffectMs, 3800);
}

// ---- the six easter-egg effects -------------------------------------------------

TEST("egg: sustained input tolerates slow worker processing timestamps")
{
    EggTracker tracker;
    const EggTracker::Context context = eggContext(35, false);
    // Worker receives these events much later, but the hook's event times stay
    // regular. The fourth second still triggers at its original event time.
    CHECK(!tracker.feed(1, context, 1000));
    CHECK(!tracker.feed(1, context, 2500));
    CHECK(!tracker.feed(1, context, 3500));
    CHECK(tracker.feed(1, context, 5100));
}

TEST("egg: logical canvas center maps to monitor center")
{
    const EggPoint wide = eggPointOnMonitor(kEggEffectSpace / 2.0,
        kEggEffectSpace / 2.0, 2560, 1440);
    CHECK(std::abs(wide.x - 1280.0) < 0.001);
    CHECK(std::abs(wide.y - 720.0) < 0.001);

    const EggPoint leftMonitor = eggPointOnMonitor(kEggEffectSpace / 2.0,
        kEggEffectSpace / 2.0, 1920, 1080);
    CHECK(std::abs(leftMonitor.x - 960.0) < 0.001);
    CHECK(std::abs(leftMonitor.y - 540.0) < 0.001);
    CHECK_EQ(eggPointOnMonitor(128.0, 128.0, 0, 1080).x, 0.0);
}

TEST("egg effects: all five oversized fullscreen concepts render throughout their cycle")
{
    CHECK_EQ(kEggEffectCount, 5);
    CHECK_EQ(kEggEffectMs, 3800);
    CHECK_EQ(kEggMaxPrimitives, 240);
    for (int id = 0; id < kEggEffectCount; ++id) {
        bool produced = false;
        int maxFrame = 0;
        bool largeMushroom = false;
        bool largeStar = false;
        for (double t = 0.0; t < kEggEffectMs / 1000.0; t += 0.05) {
            EggPrimitive prims[kEggMaxPrimitives];
            const int n = eggEffectFrame(id, t, prims, kEggMaxPrimitives);
            CHECK(n >= 0);
            CHECK(n <= kEggMaxPrimitives);
            maxFrame = std::max(maxFrame, n);
            produced = produced || n > 0;
            for (int i = 0; i < n; ++i) {
                const EggPrimitive& p = prims[i];
                CHECK(p.alpha >= 0.0 && p.alpha <= 1.0);
                CHECK(p.x > -800.0 && p.x < 1000.0); // fullscreen canvas plus margin
                CHECK(p.y > -800.0 && p.y < 1000.0);
                if (p.kind == EggPrim::Emoji) {
                    CHECK(p.glyph[0] != L'\0');
                    if (p.glyph[0] == 0xD83C && p.glyph[1] == 0xDF44)
                        largeMushroom = largeMushroom || p.size >= 60.0;
                    if (p.glyph[0] == 0x2728)
                        largeStar = largeStar || p.size >= 30.0;
                }
            }
        }
        CHECK(produced);
        CHECK(maxFrame > 40); // dense enough to read as a fullscreen burst/field
        CHECK(largeMushroom);
        CHECK(largeStar);
        CHECK_EQ(eggEffectFrame(id, kEggEffectMs / 1000.0 + 0.01, nullptr, 0), 0);
        CHECK_EQ(eggEffectFrame(id, -0.01, nullptr, 0), 0);
    }
}

TEST("egg effects: every selected concept loops without a phase reset")
{
    for (int id = 0; id < kEggEffectCount; ++id) {
        EggPrimitive first[kEggMaxPrimitives];
        EggPrimitive last[kEggMaxPrimitives];
        const int firstCount = eggEffectFrame(id, 0.0, first, kEggMaxPrimitives);
        const int lastCount = eggEffectFrame(id, kEggEffectMs / 1000.0 - 0.00001,
            last, kEggMaxPrimitives);
        // Only shapes inside the visible canvas can break the illusion: a
        // particle that wraps around does so off-screen.
        auto visible = [](const EggPrimitive& p) {
            // A margin keeps shapes that merely graze the screen edge out of
            // the comparison: crossing that line is invisible.
            constexpr double margin = 16.0;
            return p.x >= margin && p.x <= kEggEffectSpace - margin
                && p.y >= margin && p.y <= kEggEffectSpace - margin;
        };
        int firstVisible = 0;
        int lastVisible = 0;
        for (int i = 0; i < firstCount; ++i)
            firstVisible += visible(first[i]) ? 1 : 0;
        for (int i = 0; i < lastCount; ++i)
            lastVisible += visible(last[i]) ? 1 : 0;
        CHECK_EQ(firstVisible, lastVisible);
        for (int i = 0; i < firstCount && i < lastCount; ++i) {
            if (!visible(first[i]) || !visible(last[i]))
                continue;
            CHECK_EQ(first[i].kind, last[i].kind);
            CHECK(std::abs(first[i].x - last[i].x) < 0.02);
            CHECK(std::abs(first[i].y - last[i].y) < 0.02);
            CHECK(std::abs(first[i].size - last[i].size) < 0.02);
            CHECK(std::abs(first[i].alpha - last[i].alpha) < 0.02);
        }
    }
}

TEST("egg: the trigger takes four seconds")
{
    EggTracker tracker;
    const EggTracker::Context context = eggContext(100);
    bool fired = false;
    for (int ms = 0; ms <= kEggUpCountMs; ms += 250)
        fired = fired || tracker.feed(1, context, ms);
    CHECK(fired);

    EggTracker tight;
    bool early = false;
    for (int ms = 0; ms < kEggUpCountMs; ms += 250)
        early = early || tight.feed(1, context, ms);
    CHECK(!early);
    CHECK(tight.counting());
}
