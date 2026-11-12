// egg_effects.cpp — see egg_effects.h. Math only, ported from the preview page.
#include "egg_effects.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace br {

EggPoint eggPointOnMonitor(double x, double y, int width, int height)
{
    if (width <= 0 || height <= 0)
        return {};
    return EggPoint {
        (x / kEggEffectSpace) * width,
        (y / kEggEffectSpace) * height
    };
}

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kCard = kEggEffectSpace;
constexpr double kDesignScale = kEggEffectSpace / kEggDesignSpace;
double canvas(double v) { return kEggEffectSpace / 2.0 + (v - kEggDesignSpace / 2.0) * kDesignScale; }

double clampd(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

double easeOutCubic(double t)
{
    t = clampd(t, 0.0, 1.0);
    return 1.0 - std::pow(1.0 - t, 3.0);
}

double easeOutBack(double t)
{
    t = clampd(t, 0.0, 1.0);
    constexpr double c = 1.70158;
    return 1.0 + (c + 1.0) * std::pow(t - 1.0, 3.0) + c * std::pow(t - 1.0, 2.0);
}

// Exact mulberry32 from tools/egg-previews.html. Unsigned arithmetic wraps
// modulo 2^32, matching JavaScript Math.imul/Uint32 conversion.
double rand01(std::uint32_t& state)
{
    state += 0x6D2B79F5u;
    std::uint32_t t = state;
    t = (t ^ (t >> 15)) * (t | 1u);
    t ^= t + ((t ^ (t >> 7)) * (t | 61u));
    const std::uint32_t result = t ^ (t >> 14);
    return static_cast<double>(result) / 4294967296.0;
}

struct Sink {
    EggPrimitive* out;
    int max;
    int n = 0;

    void emoji(const wchar_t* glyph, double x, double y, double size,
        double alpha, double rotDeg = 0, double sy = 1.0)
    {
        if (n >= max || alpha <= 0.0 || x < -300 || x > kEggDesignSpace + 300
            || y < -300 || y > kEggDesignSpace + 300)
            return;
        EggPrimitive& p = out[n++];
        p.kind = EggPrim::Emoji;
        p.glyph[0] = glyph[0];
        p.glyph[1] = glyph[1];
        p.glyph[2] = L'\0';
        p.x = canvas(x);
        p.y = canvas(y);
        const bool mushroom = glyph[0] == L'\xD83C' && glyph[1] == L'\xDF44';
        const bool star = glyph[0] == L'\x2728';
        p.size = canvas(size) * (mushroom ? kEggMushroomScale : (star ? kEggStarScale : 1.0));
        p.alpha = clampd(alpha, 0.0, 1.0);
        p.rotDeg = rotDeg;
        p.sy = sy;
        p.lw = 0;
    }
    void dot(double x, double y, double r, double alpha)
    {
        if (n >= max || alpha <= 0.0)
            return;
        EggPrimitive& p = out[n++];
        p.kind = EggPrim::Dot;
        p.glyph[0] = L'\0';
        p.x = canvas(x);
        p.y = canvas(y);
        p.size = canvas(r);
        p.alpha = clampd(alpha, 0.0, 1.0);
        p.rotDeg = 0;
        p.sy = 1;
        p.lw = 0;
    }
    void ring(double x, double y, double r, double alpha, double lw = 2.0)
    {
        if (n >= max || alpha <= 0.0 || r <= 0.1)
            return;
        EggPrimitive& p = out[n++];
        p.kind = EggPrim::Ring;
        p.glyph[0] = L'\0';
        p.x = canvas(x);
        p.y = canvas(y);
        p.size = canvas(r);
        p.alpha = clampd(alpha, 0.0, 1.0);
        p.rotDeg = 0;
        p.sy = 1;
        p.lw = lw;
    }
    void glow(double x, double y, double r, double alpha)
    {
        if (n >= max || alpha <= 0.0)
            return;
        EggPrimitive& p = out[n++];
        p.kind = EggPrim::Glow;
        p.glyph[0] = L'\0';
        p.x = canvas(x);
        p.y = canvas(y);
        p.size = canvas(r);
        p.alpha = clampd(alpha, 0.0, 1.0);
        p.rotDeg = 0;
        p.sy = 1;
        p.lw = 0;
    }
};

// ---- oversized fullscreen concepts --------------------------------------------
// Authored on the 112-unit design grid, then expanded around its center. Each
// concept is intentionally bold: huge mushrooms, huge stars, layered motion.

constexpr int kBurstCount = 56;
struct BurstSeed { double angle, speed, delay, size, spin; };
const std::array<BurstSeed, kBurstCount>& burstSeeds()
{
    static const std::array<BurstSeed, kBurstCount> seeds = [] {
        std::array<BurstSeed, kBurstCount> s {};
        std::uint32_t rng = 0xB17B00B5u;
        for (int i = 0; i < kBurstCount; ++i) {
            s[static_cast<std::size_t>(i)] = BurstSeed { rand01(rng) * 2.0 * kPi,
                28.0 + rand01(rng) * 78.0, rand01(rng) * 0.85,
                12.0 + rand01(rng) * 18.0, (rand01(rng) - 0.5) * 3.0 };
        }
        return s;
    }();
    return seeds;
}

void giantBurst(double t, Sink& sink)
{
    const double pop = easeOutBack(t / 0.75);
    const double fade = clampd(1.0 - std::max(0.0, t - 2.75) / 0.95, 0.0, 1.0);
    sink.glow(56, 56, 58 + 80.0 * easeOutCubic(t / 1.0), 0.9 * fade);
    for (int k = 0; k < 5; ++k) {
        const double q = t - k * 0.16;
        if (q >= 0.0)
            sink.ring(56, 56, 4.0 + 110.0 * easeOutCubic(q / 1.6),
                (1.0 - q / 1.6) * 0.85, 4.0 + 2.0 * (4 - k));
    }
    sink.emoji(L"\xD83C\xDF44", 56, 57, 74.0 * pop * (0.94 + 0.06 * std::sin(t * 11)), fade,
        std::sin(t * 3.2) * 0.10 * 180.0 / kPi);
    for (int i = 0; i < kBurstCount; ++i) {
        const BurstSeed& s = burstSeeds()[static_cast<std::size_t>(i)];
        const double q = t - s.delay;
        if (q < 0.0 || q > 2.5)
            continue;
        const double r = s.speed * q + 12.0 * q * q;
        const double x = 56.0 + std::cos(s.angle) * r;
        const double y = 56.0 + std::sin(s.angle) * r;
        const double a = std::sin(kPi * clampd(q / 2.5, 0.0, 1.0)) * fade;
        const bool star = i % 3 != 0;
        sink.emoji(star ? L"\x2728" : L"\xD83C\xDF44", x, y,
            star ? s.size * 1.2 : s.size * 1.45, a, s.spin * q * 180.0 / kPi);
    }
}

void planetImpact(double t, Sink& sink)
{
    const double impactT = t - 0.55;
    const double fade = clampd(1.0 - std::max(0.0, t - 2.9) / 0.9, 0.0, 1.0);
    if (t < 0.65) {
        const double q = clampd(t / 0.65, 0.0, 1.0);
        const double x = 56.0 - 88.0 * (1.0 - easeOutCubic(q));
        const double y = 8.0 + 78.0 * (1.0 - easeOutCubic(q));
        sink.emoji(L"\xD83C\xDF44", x, y, 66.0 * (0.75 + 0.25 * q), 1.0, -0.4);
        sink.emoji(L"\x2728", x - 18.0, y - 22.0, 42.0 * (1.0 - q * 0.3), 1.0 - q * 0.2);
        sink.emoji(L"\x2728", x - 28.0, y + 15.0, 28.0, 0.85);
        return;
    }
    const double q = std::max(0.0, impactT);
    sink.glow(56, 56, 45.0 + q * 125.0, 0.9 * fade);
    for (int j = 0; j < 8; ++j) {
        const double rt = q - j * 0.11;
        if (rt >= 0.0)
            sink.ring(56, 56, 12.0 + 155.0 * easeOutCubic(rt / 1.65),
                (1.0 - rt / 1.65) * 0.88, 7.0 - j * 0.45);
    }
    const double squash = q < 0.16 ? 0.55 + q * 2.8 : 1.0 + 0.13 * std::sin(q * 10.0);
    sink.emoji(L"\xD83C\xDF44", 56, 58, 83.0 * squash, fade, std::sin(q * 2.5) * 0.08);
    for (int i = 0; i < 54; ++i) {
        const double a = i * 2.399963229728653 + 0.22;
        const double speed = 34.0 + (i % 9) * 8.3;
        const double r = speed * q + 10.0 * q * q;
        const double alpha = std::sin(kPi * clampd(q / 2.9, 0.0, 1.0)) * fade;
        sink.emoji(i % 4 == 0 ? L"\xD83C\xDF44" : L"\x2728",
            56.0 + std::cos(a) * r, 56.0 + std::sin(a) * r,
            i % 4 == 0 ? 18.0 : 13.0, alpha, a + q * 2.4);
    }
}

void meteorRain(double t, Sink& sink)
{
    const double fade = clampd(1.0 - std::max(0.0, t - 3.15) / 0.65, 0.0, 1.0);
    sink.glow(56, 58, 165.0, 0.55 * fade);
    // Five staggered lanes of oversized mushrooms, with frequent starry impacts.
    for (int lane = 0; lane < 5; ++lane) {
        for (int j = 0; j < 5; ++j) {
            const double delay = lane * 0.16 + j * 0.43;
            const double q = t - delay;
            if (q < 0.0 || q > 2.0)
                continue;
            const double x0 = 12.0 + lane * 22.0 + ((j % 2) ? 8.0 : -3.0);
            const double y = -30.0 + q * 92.0 + q * q * 15.0;
            const double x = x0 + std::sin(q * 4.0 + lane) * 8.0;
            const double squash = y > 85.0 ? 0.72 + 0.28 * std::abs(std::sin((y - 85.0) * 0.45)) : 1.0;
            const double a = fade * clampd((190.0 - y) / 30.0, 0.0, 1.0);
            sink.emoji(L"\xD83C\xDF44", x, y, 30.0 + (j % 2) * 9.0, a,
                std::sin(q * 3.0 + lane) * 0.13, squash);
            if (y > 77.0 && y < 108.0) {
                const double p = (y - 77.0) / 31.0;
                for (int s = 0; s < 3; ++s)
                    sink.emoji(L"\x2728", x + (s - 1) * (18.0 + 25.0 * p), 92.0 - 20.0 * p,
                        10.0 + 14.0 * p, (1.0 - p) * fade, s * 0.7);
                sink.ring(x, 94.0, 8.0 + 50.0 * p, (1.0 - p) * 0.6, 4.0);
            }
        }
    }
    // Oversized foreground hero bounces into view halfway through the rain.
    const double heroT = t - 0.65;
    if (heroT > 0.0) {
        const double bounce = std::abs(std::sin(heroT * 4.2)) * std::exp(-heroT * 0.42);
        sink.emoji(L"\xD83C\xDF44", 56.0, 89.0 - bounce * 35.0, 68.0,
            clampd(1.0 - std::max(0.0, heroT - 2.7) / 0.6, 0.0, 1.0) * fade,
            std::sin(heroT * 2.0) * 0.06);
    }
}

void supernova(double t, Sink& sink)
{
    const double fade = clampd(1.0 - std::max(0.0, t - 2.9) / 0.9, 0.0, 1.0);
    const double grow = easeOutBack(t / 1.15);
    const double pulse = 0.84 + 0.16 * std::sin(t * 9.0);
    sink.glow(56, 56, 36.0 + grow * 160.0, 0.95 * fade);
    for (int i = 0; i < 7; ++i) {
        const double q = t - i * 0.12;
        if (q > 0.0)
            sink.ring(56, 56, 8.0 + 175.0 * easeOutCubic(q / 2.15),
                (1.0 - q / 2.15) * 0.9, 9.0 - i * 0.75);
    }
    // Core is deliberately huge; 140% is applied in the sink.
    sink.emoji(L"\xD83C\xDF44", 56.0, 56.0, 96.0 * grow * pulse, fade,
        std::sin(t * 1.8) * 0.035);
    for (int i = 0; i < 24; ++i) {
        const double a = i * kPi * 2.0 / 24.0 + t * 0.34;
        const double orbit = 56.0 + 48.0 * easeOutCubic(t / 1.35);
        const double starSize = (i % 4 == 0 ? 27.0 : 16.0) * (0.8 + 0.3 * std::sin(t * 10.0 + i));
        sink.emoji(L"\x2728", 56.0 + std::cos(a) * orbit, 56.0 + std::sin(a) * orbit,
            starSize, fade * (0.72 + 0.28 * std::sin(t * 7.0 + i)), a + t);
    }
    if (t < 0.13)
        sink.glow(56, 56, 190.0, (1.0 - t / 0.13) * 0.9);
}

void mushroomWave(double t, Sink& sink)
{
    const double fade = clampd(1.0 - std::max(0.0, t - 3.0) / 0.8, 0.0, 1.0);
    sink.glow(56, 82, 150.0, 0.65 * fade);
    // Layered arcing rows surge up from the bottom like a living wave.
    for (int row = 0; row < 4; ++row) {
        const double delay = row * 0.23;
        const double q = clampd((t - delay) / 2.45, 0.0, 1.0);
        const double front = easeOutCubic(q);
        for (int i = 0; i < 11; ++i) {
            const double x0 = 4.0 + i * 10.4 + (row % 2) * 4.8;
            const double phase = i * 0.74 + row * 0.62;
            const double arc = std::sin(phase + t * 2.9) * (12.0 + row * 4.0);
            const double y = 105.0 - front * (48.0 + (i % 4) * 11.0) + arc;
            const double x = x0 + std::sin(t * 2.2 + phase) * (4.0 + row * 2.5);
            const double size = (21.0 + (i % 3) * 8.0 + row * 2.0) * (0.86 + 0.14 * std::sin(t * 6.0 + phase));
            const double alpha = fade * clampd((t - delay) / 0.22, 0.0, 1.0)
                * clampd((3.8 - t) / 0.4, 0.0, 1.0);
            sink.emoji(L"\xD83C\xDF44", x, y, size, alpha, std::sin(phase + t) * 0.08);
            if ((i + row) % 2 == 0) {
                sink.emoji(L"\x2728", x + std::sin(t * 5.0 + phase) * 10.0, y - size * 0.9,
                    13.0 + 7.0 * std::sin(t * 7.0 + i), alpha * 0.86, phase);
            }
        }
    }
    for (int i = 0; i < 7; ++i) {
        const double q = t - i * 0.21;
        if (q > 0.0)
            sink.ring(56.0, 99.0, 10.0 + 94.0 * easeOutCubic(q / 1.6),
                (1.0 - q / 1.6) * 0.65 * fade, 5.0);
    }
    sink.emoji(L"\xD83C\xDF44", 56.0, 57.0 - 5.0 * std::sin(t * 4.4),
        76.0 * (0.94 + 0.06 * std::sin(t * 8.0)), fade, std::sin(t * 1.7) * 0.04);
}

void fullscreenFrame(int id, double t, Sink& sink)
{
    const double phase = t * (2.0 * kPi / (kEggEffectMs / 1000.0));
    switch (id) {
    case 0: { // Giant burst: hero breathes while a dense star field orbits.
        sink.glow(56, 56, 58.0 + 10.0 * std::sin(phase), 0.34);
        for (int ring = 0; ring < 4; ++ring) {
            const double p = phase + ring * kPi * 0.5;
            sink.ring(56, 56, 58.0 + 25.0 * std::sin(p),
                0.42 + 0.23 * std::sin(p + 0.7), 3.0 + ring);
        }
        sink.emoji(L"\xD83C\xDF44", 56.0, 56.0, 24.0 * (0.94 + 0.10 * std::sin(phase)),
            1.0, std::sin(phase) * 0.06);
        for (int i = 0; i < kBurstCount; ++i) {
            const BurstSeed& s = burstSeeds()[static_cast<std::size_t>(i)];
            const double p = phase + s.delay * 2.0 * kPi;
            const double r = 23.0 + 27.0 * (0.5 + 0.5 * std::sin(p));
            sink.emoji(i % 3 == 0 ? L"\xD83C\xDF44" : L"\x2728",
                56.0 + std::cos(s.angle + 0.10 * std::sin(phase)) * r,
                56.0 + std::sin(s.angle + 0.10 * std::sin(phase)) * r,
                i % 3 == 0 ? 10.0 : 6.0 + (i % 4) * 1.0,
                0.62 + 0.32 * std::sin(p), s.spin * std::sin(phase) * 0.5);
        }
        break;
    }
    case 1: { // Impact: a giant center mushroom, pulsing shockwaves and debris.
        const double pulse = 0.91 + 0.09 * std::sin(phase * 2.0);
        sink.glow(56, 57, 46.0 + 16.0 * std::sin(phase), 0.38);
        for (int ring = 0; ring < 6; ++ring) {
            const double p = phase + ring * (2.0 * kPi / 6.0);
            sink.ring(56, 57, 24.0 + 72.0 * (0.5 + 0.5 * std::sin(p)),
                0.48 + 0.27 * std::sin(p), 3.0 + (ring % 3) * 1.8);
        }
        sink.emoji(L"\xD83C\xDF44", 56.0, 57.0, 26.0 * pulse, 1.0,
            std::sin(phase) * 0.04);
        for (int i = 0; i < 54; ++i) {
            const double a = i * 2.399963229728653 + phase;
            const double r = 31.0 + 18.0 * std::sin(phase + i * 0.37);
            const double x = 56.0 + std::cos(a) * r;
            const double y = 56.0 + std::sin(a) * r;
            sink.emoji(i % 4 == 0 ? L"\xD83C\xDF44" : L"\x2728", x, y,
                i % 4 == 0 ? 9.0 : 6.0 + (i % 3) * 1.0,
                0.68 + 0.28 * std::sin(phase + i), a * 180.0 / kPi);
        }
        break;
    }
    case 2: { // Meteor rain: 25 mushrooms wrap beyond the viewport, not onscreen.
        sink.glow(56.0, 73.0, 62.0, 0.30 + 0.08 * std::sin(phase));
        for (int lane = 0; lane < 5; ++lane) {
            for (int j = 0; j < 5; ++j) {
                const double q = std::fmod(t + lane * 0.31 + j * 0.67,
                    kEggEffectMs / 1000.0) / (kEggEffectMs / 1000.0);
                const double x0 = 9.0 + lane * 21.0 + (j % 2) * 5.0;
                const double y = -32.0 + q * 174.0;
                const double x = x0 + std::sin(phase + lane + j * 0.6) * 5.0;
                const double bob = 0.92 + 0.08 * std::sin(phase * 2.0 + lane + j);
                sink.emoji(L"\xD83C\xDF44", x, y, (13.0 + (j % 2) * 3.5) * bob,
                    0.98, std::sin(phase + j) * 0.10);
                for (int s = 0; s < 2; ++s) {
                    const double side = (s == 0 ? -1.0 : 1.0);
                    sink.emoji(L"\x2728", x + side * (9.0 + 5.0 * std::sin(phase + j)),
                        y - 19.0 - s * 12.0, 5.0 + (s ? 2.0 : 0.0),
                        0.84 + 0.14 * std::sin(phase + lane + s), side * 0.25);
                }
            }
        }
        // A hero mushroom bouncing along the bottom keeps the effect anchored
        // by something big.
        sink.emoji(L"\xD83C\xDF44", 56.0, 92.0 - std::abs(std::sin(phase * 2.0)) * 9.0,
            24.0 * (0.95 + 0.05 * std::sin(phase * 2.0)), 1.0, std::sin(phase) * 0.05);
        break;
    }
    case 3: { // Supernova: huge breathing hero with an always-moving star halo.
        const double breathe = 0.89 + 0.11 * std::sin(phase * 2.0);
        sink.glow(56, 56, 60.0 + 14.0 * std::sin(phase), 0.40);
        for (int ring = 0; ring < 5; ++ring) {
            const double p = phase + ring * (2.0 * kPi / 5.0);
            sink.ring(56, 56, 20.0 + 86.0 * (0.5 + 0.5 * std::sin(p)),
                0.46 + 0.25 * std::sin(p), 3.0 + (ring % 2) * 2.0);
        }
        sink.emoji(L"\xD83C\xDF44", 56.0, 56.0, 26.0 * breathe, 1.0,
            std::sin(phase) * 0.035);
        for (int i = 0; i < 30; ++i) {
            const double a = i * (2.0 * kPi / 30.0) + phase * (i % 2 ? 1.0 : -1.0);
            const double r = 43.0 + 8.0 * std::sin(phase + i * 0.43);
            sink.emoji(L"\x2728", 56.0 + std::cos(a) * r, 56.0 + std::sin(a) * r,
                (i % 5 == 0 ? 9.0 : 6.0 + (i % 4) * 0.8),
                0.74 + 0.24 * std::sin(phase * 2.0 + i), a * 180.0 / kPi);
        }
        // A ring of smaller mushrooms keeps the scene dense around the hero.
        for (int i = 0; i < 16; ++i) {
            const double a = i * (2.0 * kPi / 16.0) - phase * 0.5;
            const double r = 76.0 + 6.0 * std::sin(phase * 2.0 + i);
            sink.emoji(L"\xD83C\xDF44", 56.0 + std::cos(a) * r, 56.0 + std::sin(a) * r,
                7.0 + (i % 3) * 1.2, 0.80 + 0.18 * std::sin(phase + i), a * 180.0 / kPi);
        }
        break;
    }
    case 4: { // Mushroom wave: dense rows rise and fall with staggered phases.
        sink.glow(56.0, 79.0, 66.0, 0.32 + 0.08 * std::sin(phase));
        for (int row = 0; row < 4; ++row) {
            for (int i = 0; i < 11; ++i) {
                const double lane = i * 0.72 + row * 0.83;
                const double wave = phase + lane;
                const double x = 4.0 + i * 10.4 + (row % 2) * 4.5
                    + 3.8 * std::sin(phase + lane);
                const double y = 53.0 + row * 13.0 + 22.0 * std::sin(wave);
                const double size = (13.0 + (i % 3) * 2.0) * (0.91 + 0.09 * std::sin(wave));
                sink.emoji(L"\xD83C\xDF44", x, y, size,
                    0.90 + 0.09 * std::sin(phase * 2.0 + lane), std::sin(wave) * 0.08);
                if ((i + row) % 2 == 0)
                    sink.emoji(L"\x2728", x + 4.0 * std::sin(phase * 2.0 + lane),
                        y - size * 0.72, 6.0 + (i % 3) * 1.0,
                        0.82 + 0.16 * std::sin(phase * 2.0 + lane), wave * 0.15);
            }
        }
        sink.emoji(L"\xD83C\xDF44", 28.0, 82.0, 22.0 * (0.93 + 0.07 * std::sin(phase)),
            0.98, std::sin(phase) * 0.04);
        sink.emoji(L"\xD83C\xDF44", 84.0, 82.0, 22.0 * (0.93 + 0.07 * std::sin(phase + kPi)),
            0.98, std::sin(phase + kPi) * 0.04);
        for (int i = 0; i < 6; ++i) {
            const double p = phase + i * (2.0 * kPi / 6.0);
            sink.ring(56.0, 95.0, 30.0 + 66.0 * (0.5 + 0.5 * std::sin(p)),
                0.42 + 0.22 * std::sin(p), 3.0);
        }
        break;
    }
    }
}

} // namespace

int eggEffectFrame(int id, double tSeconds, EggPrimitive* out, int maxOut)
{
    if (!out || maxOut <= 0 || id < 0 || id >= kEggEffectCount)
        return 0;
    if (tSeconds < 0.0 || tSeconds >= kEggEffectMs / 1000.0)
        return 0;
    Sink sink { out, maxOut, 0 };
    fullscreenFrame(id, tSeconds, sink);
    return sink.n;
}

} // namespace br
