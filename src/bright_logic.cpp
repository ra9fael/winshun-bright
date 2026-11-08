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

} // namespace

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

GestureDecision decideGesture(const GestureEvent& e)
{
    // Simulated input is never ours: swallowing it would loop the gesture.
    if (e.injected)
        return GestureDecision::pass();
    // Exactly Ctrl+Alt. Ctrl-only belongs to the volume plugin; Alt-only,
    // Shift and Win combos belong to the system.
    if (!e.ctrl || !e.alt || e.shift || e.win)
        return GestureDecision::pass();

    if (e.wheel) {
        if (e.wheelDelta == 0)
            return GestureDecision::pass();
        GestureDecision d;
        d.swallow = true;
        d.kind = GestureKind::Wheel;
        d.steps = e.wheelDelta > 0 ? 1 : -1;
        return d;
    }
    if (e.leftDown) {
        GestureDecision d;
        d.swallow = true;
        d.kind = GestureKind::Click;
        return d;
    }
    return GestureDecision::pass();
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
