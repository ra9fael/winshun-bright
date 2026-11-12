# Changelog

[中文](CHANGELOG.md)

## [Unreleased]

## [0.9.0] - 2026-11-12

### Fixed

- Fixed the effect being effectively invisible: `D2D1::ColorF(0, 0.0f)` resolved to opaque black (a full-screen black overlay), the glyph size missed the screen scale, and layout rectangles are device-space so every emoji was drawn into the top-left corner and clipped.
- Fixed WIC sprite creation failing because the render thread had no COM apartment.

### Changed

- One concept is now chosen at trigger time and loops seamlessly (3.8 s cycle with matching first/last phase) until the user clicks; no more rotating between concepts.
- Mushrooms and stars are pre-rasterised into cached bitmaps and only scaled per frame, removing the colour-emoji re-rasterisation that caused the stutter.
- Every concept now has a hero mushroom (~17% of the short edge) plus a denser mushroom/star field; the supernova gained an orbiting mushroom ring and the meteor rain a bouncing hero.
- The frame interval adapts to the measured render cost (33–50 ms) instead of piling up timer messages.
- The HTML concept page and the plugin now share the same coordinates, sizes and timing (verified primitive by primitive).

## [0.8.0] - 2026-11-11

### Changed

- Replaced the egg library with five oversized fullscreen concepts: Giant Burst, Planet Impact, Mushroom Meteor Rain, Supernova, and Mushroom Wave. They loop and randomly change every 3.8 seconds.
- Applied the concept defaults: mushrooms at 140%, stars at 230%, with denser particle fields centered on the target display.

## [0.7.2] - 2026-11-11

### Changed

- Enter on the discovered “Mushroom easter egg” result now explicitly plays the effect; enable/disable remain separate actions. Trying to play while disabled explains that it must be enabled first.
- Mushroom emoji now pulse in size during playback.

## [0.7.1] - 2026-11-11

### Fixed

- Fixed the 0.7.0 trigger regression: settings refreshes no longer force an undiscovered easter egg off, so the first trigger can fire normally.

## [0.7.0] - 2026-11-11

### Changed

- Persist `easterEggDiscovered` after the first successful trigger. Once discovered, the `bright` search results expose an enable/disable action backed by `state.json`.
- Freeze brightness changes from wheel, hotkeys, search submission, and monitor-row actions while the persistent egg is running. Controls resume after the user stops it.
- The settings toggle is ignored until discovery; search actions and settings share the persisted enabled state.

## [0.6.1] - 2026-11-11

### Changed

- The persistent fullscreen random effect is centered on the pointer's display and loops until a physical left click.
- Trigger progress is tracked per display and uses the original wheel-event timestamps, making it resilient to slow DDC calls and brief pointer movement between monitors. The accepted event gap is now 10 seconds.

## [0.6.0] - 2026-11-11

### Changed

- Full-screen easter egg: a randomly selected Unicode emoji effect spreads particles across the pointer's display. Emoji glyphs retain their aspect ratio and rings stay circular. The layered window is per-pixel transparent and click-through; the desktop itself is not dimmed or altered.

### Fixed

- Fixed `no-handle` on dual physical monitors: when one display enumerates multiple physical nodes, probe each and cache the handle that answers VCP 0x10.

## [0.5.0] - 2026-11-11

### Changed

- The easter egg plays one of six Unicode emoji effects at random, matching the preview page.

## [0.4.2] - 2026-11-11

### Fixed

- The easter egg flashed by: it shared a window with the brightness indicator, so the next scroll (or the 800 ms read-back calibration) replaced it with a percentage panel. It now has its own window and timers, which the brightness overlay no longer touches.

## [0.4.1] - 2026-11-11

### Fixed

- Fixed overly narrow easter egg trigger conditions: sustained upward scrolling on one display for about four seconds now triggers it regardless of time of day, brightness, or the all-displays target.

## [0.4.0] - 2026-11-11

### Changed

- The hidden effect is snappier: the trigger dropped from ten seconds to four, the poison runs eight (was ten), and the fade 1.2 (was 1.5).
- Version 0.4.0 briefly added full-screen warping and dimming to the mushroom easter egg; Unreleased replaces it with a local overlay animation.

## [0.3.2] - 2026-11-11

### Added

- `remoteDim` (default on): in a remote session the hardware channels all fail and the layer is the only working dimmer, so it is enabled there by default; the setting turns it off, and the `bright` list then says brightness is unavailable remotely.
- `softwareBrightness` is back to defaulting to `off` — the local-session layer is opt-in again, remote behaviour is controlled by `remoteDim` alone.

### Fixed

- Settings edits could sit unread for a long time: the worker loop sleeps in GetMessage, so an edit waited for the next timer or gesture. applySettings now wakes the worker, and the layer/overlay follow the settings immediately.

## [0.3.1] - 2026-11-10

### Added

- A more dramatic hidden effect: two black flashes up front, a darkness rush to ~88% in the first two seconds (creeping to ~97%), a 2 Hz heartbeat pulse, the colour turning sick green as the poison deepens, and the countdown in big poison-green.

### Fixed

- Gestures kept reporting "unsupported" after an RDP → console session switch: the worker polls the session state every 3 seconds and rebuilds the whole display backend on a change (the old WMI connection and DDC handles die with the session).

## [0.3.0] - 2026-11-10

### Added

- Software brightness simulation (`softwareBrightness`): a click-through translucent layer dims a display neither WMI nor DDC can drive; "unsupported displays only" or "always" (stacked on the hardware brightness).
- `softwareMaxDark` (0–90, default 60) caps how dark the layer goes.
- Some operations trigger hidden effects. They are deliberately undocumented; `mushroomEgg: false` turns every hidden effect off.
- The `bright` list marks simulated panels as 软件模拟 / software and reports `data.simulated`; the remote-session row now points at enabling the layer.

### Fixed

- A remote session is no longer reported as "unsupported": the row says why and what can be done.
- The layer never takes focus, never appears in the taskbar or Alt+Tab, and never intercepts a mouse event; it is rebuilt after a resolution change or hot-plug and destroyed with the process.

## [0.2.0] - 2026-11-10

### Added

- The gesture changes the display under the pointer by default (`gestureTarget`), with "all displays" still available.
- The gesture chord is configurable (`gestureModifiers`: ctrl+alt / alt+shift / win+alt / ctrl+shift+alt) instead of hard-wired Ctrl+Alt.
- Three recordable hotkeys `hotkeyUp` / `hotkeyDown` / `hotkeyMax`: registered through `register_hotkeys` when the host offers `hostHotkeys`, else by a `RegisterHotKey` thread of our own.
- `gestureDebug`: hook installation, gesture coordinates and per-display failures go to plugins.log.
- Remote desktop detection (mstsc/RDP): the `bright` list and the overlay say that RDP exposes only a virtual display, instead of reporting the physical panel as unsupported.

### Fixed

- The host no longer retires the plugin after 10 idle minutes (`idleExitSec: 0`), so the low-level mouse hook and the overlay stay alive.
- The gesture is no longer "taskbar only": the hook re-installs itself every 10 seconds, recovering when the system drops it.
- A single adjustable display in a multi-monitor setup: the DDC probe retries, a panel that failed once is no longer written off (it is re-bound on first use), and the failure reason (no-handle / vcp-read / vcp-write / wmi-write) reaches the overlay and the log.
- The overlay's `monitor-under-mouse` option was the same branch as `center` (a no-op); a new `target-monitor` puts it on the panel that was changed.
- The 800 ms DDC read-back calibration now compares only the displays this write touched.
- `dispatchGesture` read the settings without the lock on the hook thread (data race).

### Notes

- The development machine is reached over mstsc, so physical brightness cannot be verified in a remote session; the local-session E2E list in docs/tests.md §3 is still required before release.


## [0.1.1] - 2026-11-09

### Fixed

- Embed the executable icon and include the ICO and resource script in the package. Use the static MSVC runtime for the plugin executable.

### Notes

- Local tag and package only. See docs/icon-validation.en.md for the measured coverage; no GitHub release. Restart the plugin process after upgrading.

## [0.1.0]

- Preserve the current plugin behavior and add a reproducible tag/manual build, test and package workflow.
