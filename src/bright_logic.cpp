// bright_logic.cpp — see bright_logic.h.
#include "bright_logic.h"

#include <algorithm>
#include <cmath>

namespace br {

namespace {

std::string trimText(const std::string& s)
{
    std::size_t a = 0;
    std::size_t b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n'))
        ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n'))
        --b;
    return s.substr(a, b - a);
}

std::string lowerText(const std::string& s)
{
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

} // namespace

long long Rect::distanceSquared(int x, int y) const
{
    if (empty())
        return -1;
    long long dx = 0;
    if (x < left)
        dx = left - x;
    else if (x >= right)
        dx = x - (right - 1);
    long long dy = 0;
    if (y < top)
        dy = top - y;
    else if (y >= bottom)
        dy = y - (bottom - 1);
    return dx * dx + dy * dy;
}

std::vector<MonitorInfo> normalizeMonitors(std::vector<MonitorInfo> in)
{
    std::vector<MonitorInfo> out;
    out.reserve(in.size());
    for (MonitorInfo& m : in) {
        const bool duplicate = std::any_of(out.begin(), out.end(),
            [&m](const MonitorInfo& o) { return o.id == m.id; });
        if (!duplicate)
            out.push_back(std::move(m));
    }
    std::stable_sort(out.begin(), out.end(), [](const MonitorInfo& a, const MonitorInfo& b) {
        if (a.internal != b.internal)
            return a.internal; // internal panels first
        return a.id < b.id;
    });
    return out;
}

int monitorIndexAtPoint(const std::vector<MonitorInfo>& monitors, int x, int y, bool adjustableOnly)
{
    int inside = -1;
    int nearest = -1;
    long long nearestDistance = 0;
    for (std::size_t i = 0; i < monitors.size(); ++i) {
        const MonitorInfo& m = monitors[i];
        if (adjustableOnly && !m.adjustable)
            continue;
        if (m.bounds.contains(x, y)) {
            // Overlapping mirrors: the first region wins, the rest were dropped
            // by normalizeMonitors anyway.
            inside = static_cast<int>(i);
            break;
        }
        const long long distance = m.bounds.distanceSquared(x, y);
        if (distance >= 0 && (nearest < 0 || distance < nearestDistance)) {
            nearest = static_cast<int>(i);
            nearestDistance = distance;
        }
    }
    return inside >= 0 ? inside : nearest;
}

GestureDecision decideGesture(const GestureEvent& e, const GestureModifiers& chord)
{
    // A real physical left click is also the stop signal for the persistent
    // easter egg. It is explicitly non-swallowing; normal click behaviour is
    // preserved even when no brightness chord is held.
    bool stopEgg = false;
    if (e.leftDown && !e.injected)
        stopEgg = true;
    // Simulated input is never ours: swallowing it would loop the gesture.
    if (e.injected)
        return GestureDecision::pass();
    // The configured chord exactly; Ctrl-only (the volume plugin's gesture),
    // Alt-only, Shift/Win combos and any extra modifier pass through.
    if (!chord.matches(e.ctrl, e.alt, e.shift, e.win)) {
        GestureDecision d = GestureDecision::pass();
        d.stopEgg = stopEgg;
        d.x = e.x;
        d.y = e.y;
        return d;
    }

    if (e.wheel) {
        if (e.wheelDelta == 0) {
            GestureDecision d = GestureDecision::pass();
            d.stopEgg = stopEgg;
            d.x = e.x;
            d.y = e.y;
            return d;
        }
        GestureDecision d;
        d.stopEgg = stopEgg;
        d.swallow = true;
        d.kind = GestureKind::Wheel;
        d.steps = e.wheelDelta > 0 ? 1 : -1;
        d.x = e.x;
        d.y = e.y;
        d.eventTimeMs = e.eventTimeMs;
        return d;
    }
    if (e.leftDown) {
        GestureDecision d;
        d.stopEgg = stopEgg;
        d.swallow = true;
        d.kind = GestureKind::Click;
        d.x = e.x;
        d.y = e.y;
        return d;
    }
    return GestureDecision::pass();
}

GestureModifiers GestureModifiers::parse(const std::string& text)
{
    const std::string trimmed = trimText(text);
    if (trimmed.empty())
        return ctrlAlt();
    GestureModifiers out;
    out.ctrl = false;
    out.alt = false;
    out.shift = false;
    out.win = false;
    bool any = false;
    std::size_t start = 0;
    while (start <= trimmed.size()) {
        const std::size_t plus = trimmed.find('+', start);
        const std::string part = lowerText(trimText(trimmed.substr(start,
            plus == std::string::npos ? std::string::npos : plus - start)));
        if (part == "ctrl" || part == "control")
            out.ctrl = true;
        else if (part == "alt")
            out.alt = true;
        else if (part == "shift")
            out.shift = true;
        else if (part == "win" || part == "meta")
            out.win = true;
        else if (!part.empty())
            return ctrlAlt(); // an unknown name: keep the default rather than a weird chord
        any = any || !part.empty();
        if (plus == std::string::npos)
            break;
        start = plus + 1;
    }
    // A chord with no modifier would swallow every plain wheel and click.
    if (!any || (!out.ctrl && !out.alt && !out.shift && !out.win))
        return ctrlAlt();
    return out;
}

std::string GestureModifiers::text() const
{
    std::string out;
    const auto add = [&out](const char* name) {
        if (!out.empty())
            out += '+';
        out += name;
    };
    if (win)
        add("win");
    if (ctrl)
        add("ctrl");
    if (alt)
        add("alt");
    if (shift)
        add("shift");
    return out;
}

bool GestureModifiers::matches(bool ctrlDown, bool altDown, bool shiftDown, bool winDown) const
{
    if (!ctrl && !alt && !shift && !win)
        return false; // an empty chord is not a gesture
    return ctrl == ctrlDown && alt == altDown && shift == shiftDown && win == winDown;
}


int WheelAccumulator::add(int delta)
{
    if (delta == 0)
        return 0;
    if (m_remainder != 0 && ((delta < 0) != (m_remainder < 0)))
        m_remainder = 0; // direction reversed: the old remainder is stale
    m_remainder += delta;
    int steps = 0;
    while (m_remainder >= kWheelDelta) {
        ++steps;
        m_remainder -= kWheelDelta;
    }
    while (m_remainder <= -kWheelDelta) {
        --steps;
        m_remainder += kWheelDelta;
    }
    return steps;
}

int clampPercent(int v)
{
    if (v < 0)
        return 0;
    if (v > 100)
        return 100;
    return v;
}

int steppedPercent(int current, int steps, int percentPerStep)
{
    if (percentPerStep < 1)
        percentPerStep = 1;
    if (percentPerStep > 10)
        percentPerStep = 10;
    return clampPercent(current + steps * percentPerStep);
}

int dimAlpha(int percent, int maxDark)
{
    // 90% black is the hard ceiling: more than that and the user cannot find
    // the pointer any more.
    if (maxDark < 0)
        maxDark = 0;
    if (maxDark > 90)
        maxDark = 90;
    const int p = clampPercent(percent);
    if (p >= 100)
        return 0; // fully bright: no layer at all
    const int peak = static_cast<int>(std::lround(maxDark * 255.0 / 100.0));
    // 0% brightness paints the whole cap, 100% paints nothing.
    return static_cast<int>(std::lround(peak * (100 - p) / 100.0));
}

bool isNight(int minutesOfDay)
{
    if (minutesOfDay < 0)
        minutesOfDay += 1440;
    minutesOfDay %= 1440;
    constexpr int kNightStart = 22 * 60; // 22:00
    constexpr int kNightEnd = 6 * 60;    // 06:00
    return minutesOfDay >= kNightStart || minutesOfDay < kNightEnd;
}

void EggTracker::reset()
{
    m_startedAt = -1;
    m_lastUpAt = -1;
}

int EggTracker::elapsedMs(long long nowMs) const
{
    if (m_startedAt < 0)
        return 0;
    return static_cast<int>(nowMs - m_startedAt);
}

bool EggTracker::feed(int steps, const Context& context, long long nowMs)
{
    // Any sustained upward scroll on the same monitor may reveal the egg.
    const bool eligible = context.eggEnabled && context.sameMonitor && steps > 0;
    if (!eligible) {
        reset();
        return false;
    }
    if (m_startedAt >= 0 && nowMs - m_lastUpAt > kEggGapMs) {
        // A pause longer than the gap is not "continuous" any more; this step
        // starts a new run rather than continuing the old one.
        reset();
    }
    if (m_startedAt < 0)
        m_startedAt = nowMs;
    m_lastUpAt = nowMs;
    if (nowMs - m_startedAt >= kEggUpCountMs) {
        reset(); // consumed: the next one needs a fresh 10 seconds
        return true;
    }
    return false;
}

int vcpToPercent(int value, int max)
{
    if (max <= 0)
        return 0;
    if (value < 0)
        value = 0;
    if (value > max)
        value = max;
    return static_cast<int>(std::lround(value * 100.0 / max));
}

int percentToVcp(int percent, int max)
{
    if (max <= 0)
        return 0;
    const int p = clampPercent(percent);
    int v = static_cast<int>(std::lround(p * max / 100.0));
    if (v < 0)
        v = 0;
    if (v > max)
        v = max;
    return v;
}

int averagePercent(const std::vector<int>& values)
{
    if (values.empty())
        return -1;
    long long sum = 0;
    for (const int v : values)
        sum += v;
    return static_cast<int>(std::lround(static_cast<double>(sum) / values.size()));
}

void buildWarmLut(int strength, std::uint16_t lut[3][256])
{
    int s = strength;
    if (s < 0)
        s = 0;
    if (s > 100)
        s = 100;
    const double t = s / 100.0;
    // 6500K -> ~3400K approximation: red stays, green falls a little, blue
    // falls the most. Subjective comfort over colourimetric accuracy.
    const double greenFactor = 1.0 - 0.28 * t;
    const double blueFactor = 1.0 - 0.55 * t;
    for (int i = 0; i < 256; ++i) {
        const double base = i * 257.0; // 0..65535
        lut[0][i] = static_cast<std::uint16_t>(std::lround(base));
        lut[1][i] = static_cast<std::uint16_t>(std::lround(base * greenFactor));
        lut[2][i] = static_cast<std::uint16_t>(std::lround(base * blueFactor));
    }
}

BrightnessQuery parseBrightnessQuery(const std::string& text)
{
    BrightnessQuery q;
    std::string t = trimText(text);
    if (t.empty())
        return q; // List

    if (t.back() == '%')
        t = trimText(t.substr(0, t.size() - 1));
    if (t.empty()) {
        q.kind = BrightnessQuery::Kind::Invalid;
        return q;
    }

    bool negative = false;
    std::size_t i = 0;
    if (t[0] == '+' || t[0] == '-') {
        negative = t[0] == '-';
        i = 1;
    }
    if (i >= t.size()) {
        q.kind = BrightnessQuery::Kind::Invalid;
        return q;
    }

    long long value = 0;
    for (; i < t.size(); ++i) {
        if (t[i] < '0' || t[i] > '9') {
            q.kind = BrightnessQuery::Kind::Invalid;
            return q;
        }
        value = value * 10 + (t[i] - '0');
        if (value > 100000)
            value = 100000; // saturate instead of overflowing
    }
    if (negative)
        value = -value;
    q.kind = BrightnessQuery::Kind::Set;
    q.percent = static_cast<int>(value);
    return q;
}

} // namespace br
