// egg_effects.h — pure frame math for five fullscreen, oversized easter eggs.
// Nothing here touches Win32: a frame call returns emoji/particle primitives
// in a centered virtual screen space and can be unit-tested.
#pragma once

#include <cstdint>

namespace br {

constexpr int kEggEffectCount = 5;    // giant burst, impact, meteor rain, supernova, mushroom wave
constexpr int kEggEffectMs = 3800;    // maximum concept duration; individual concepts use this full cycle
constexpr int kEggMaxPrimitives = 240; // dense fullscreen particle fields
constexpr double kEggMushroomScale = 1.40; // user-selected default from the concept
constexpr double kEggStarScale = 2.30;
constexpr double kEggParticleDensity = 0.55;
constexpr double kEggEffectSpace = 256.0; // virtual canvas mapped across the display
constexpr double kEggDesignSpace = 112.0; // shared HTML/native authoring grid

// Maps logical effect coordinates to monitor pixels; the effect-space center
// is always the exact center of the selected monitor.
struct EggPoint { double x = 0.0; double y = 0.0; };
EggPoint eggPointOnMonitor(double x, double y, int width, int height);

enum class EggPrim { Emoji, Dot, Ring, Glow };

struct EggPrimitive {
    EggPrim kind = EggPrim::Emoji;
    wchar_t glyph[4] = L""; // Emoji only: UTF-16 "🍄" (2 units) or "✨" (1)
    double x = 0;           // card space, 0..112; glyph/primitive centre
    double y = 0;
    double size = 0;        // emoji font px, dot radius, glow radius
    double alpha = 1.0;     // 0..1
    double rotDeg = 0;      // Emoji only, clockwise
    double sy = 1.0;        // Emoji vertical squash (1 = none)
    double lw = 2.0;        // Ring stroke width
};

// Fills `out` with one frame of effect `id` (0..kEggEffectCount-1) at
// `tSeconds` (0..1.8). Later primitives draw on top. Returns the count; 0 for
// invalid arguments or when nothing is visible.
int eggEffectFrame(int id, double tSeconds, EggPrimitive* out, int maxOut);

} // namespace br
