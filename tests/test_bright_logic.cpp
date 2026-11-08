// test_bright_logic.cpp — exhaustive tests for the pure core: gesture
// decisions, wheel accumulation, percent/VCP math, the warm LUT, monitor list
// normalization and the `bright` query parser. No display, WMI or DDC access.
#include "bright_logic.h"
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

TEST("gesture: ctrl+alt+left-down toggles the warm filter")
{
    GestureEvent e;
    e.ctrl = true;
    e.alt = true;
    e.leftDown = true;
    const GestureDecision d = decideGesture(e);
    CHECK(d.swallow);
    CHECK_EQ(d.kind, GestureKind::Click);
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
