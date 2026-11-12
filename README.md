# DisplayBrightness — Win顺 screen brightness plugin

Ctrl+Alt+wheel changes the brightness of the **display under the pointer**
(WMI for built-in panels, DDC/CI for external monitors); the chord and the
target ("only this display" / "all displays") are settings. Ctrl+Alt+click
toggles a **warm color filter**. The `bright` keyword does the same from the
keyboard, per monitor. Overlay and gesture architecture shared with
[winshun-volume](https://github.com/ra9fael/winshun-volume).

English code/comments per project convention; user-facing strings follow the
host locale (zh/en).

## Remote sessions

In an RDP / mstsc session Windows exposes a single virtual display: WMI
brightness, DDC/CI and the gamma ramp all stop at the virtual driver and never
reach the physical monitor. The plugin detects this and says so in the
`bright` list instead of reporting the panel as unsupported. The dim layer
follows remote sessions by default (`remoteDim`), so the session is dimmable
out of the box; turn that setting off if you find the layer intrusive over
RDP.

## Usage

Mouse (the chord is a setting — plain Ctrl belongs to the volume plugin;
other combinations, simulated input and everything else pass through
untouched):

| Gesture | Effect |
| --- | --- |
| chord+wheel | the display under the pointer ±N% (or all, per `gestureTarget`); the overlay shows the value |
| chord+left-click | warm filter on/off (needs `warmStrength` > 0), overlay confirms |
| Recognized, no target | event swallowed, overlay explains why |
| Anything else | passthrough |

Keyboard: type `bright` (or `亮度`). An empty query lists every monitor with
its type, channel (WMI / DDC/CI) and current brightness; `bright 70` sets
every adjustable monitor to 70% (out-of-range input is clamped and the
result row says so). Per-monitor actions on the selected row: Enter +10%,
Ctrl+Enter −10%, Shift+Enter 100% — read-only until an action runs.

Three optional global hotkeys (`hotkeyUp` / `hotkeyDown` / `hotkeyMax`) do the
same without the launcher: registered by the host when it supports
`register_hotkeys`, otherwise by the plugin itself.

A small non-activating overlay shows the new value with a bar while you
adjust, positioned by the `overlayAnchor` setting (bottom-center by default)
and `overlayOffsetX/Y`; it disappears automatically.

## Settings

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `percentPerStep` | number | 5 | wheel step size, 1–10 |
| `gestureEnabled` | bool | true | gestures on/off |
| `gestureModifiers` | choice | ctrl+alt | chord held with the wheel / click |
| `gestureTarget` | choice | monitor-under-mouse | the pointer's display, or all displays |
| `softwareBrightness` | choice | off | `off` / `unsupported-only` / `always`: dim panels the hardware cannot drive with a translucent, click-through layer |
| `remoteDim` | bool | true | the layer follows the remote session: in RDP the hardware channels all fail, so the layer is the only working dimmer there; off makes brightness unavailable remotely |
| `softwareMaxDark` | number | 60 | the layer's darkness cap at 0%, 0–90 |
| `mushroomEgg` | bool | true | easter egg on/off |
| `hotkeyUp` / `hotkeyDown` / `hotkeyMax` | hotkey | empty | optional global step / 100% keys |
| `showOverlay` | bool | true | overlay window on/off |
| `overlayMs` | number | 600 | overlay linger, 300–3000 ms |
| `overlayAnchor` | choice | bottom-center | nine-grid anchor, `monitor-under-mouse`, or `target-monitor` |
| `overlayOffsetX` / `overlayOffsetY` | number | 0 | pixel nudges (±4000) |
| `warmStrength` | number | 0 | warm filter depth 0–100; 0 disables it |
| `warmRemember` | bool | false | persist the filter state across restarts |
| `gestureDebug` | bool | false | log hook installation, gesture coordinates and per-display failures |

Invalid values fall back to defaults, never errors.

## Software brightness

A panel no hardware channel can drive (a TV through an AV receiver, a
projector, an RDP session's virtual display) can still be dimmed: with
`softwareBrightness` on, a click-through translucent layer covers it and the
gestures step its opacity instead of the display. It never takes focus, never
shows in the taskbar or Alt+Tab, and disappears with the process.

The easter egg can be disabled from the plugin settings.

## Requirements

- Windows 10/11.
- Win顺 0.22.0 or newer: this is a community native package (`exe`) and the
  plugin library lists it as `unreviewed-native`.
- The Win顺 host running this resident plugin (`bright.exe` must sit next to
  `plugin.json`).
- Hardware: built-in panels need a driver exposing the WMI brightness
  classes (`WmiMonitorBrightness`); external monitors need DDC/CI enabled
  and a cable/dock that carries it (monitors that answer neither are listed
  as 不支持).
- Build from source with CMake + a C++17 compiler (MinGW-w64 or MSVC):

  ```powershell
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
  cmake --build build
  ctest --test-dir build
  ```

  `bright.exe` is placed in the repository root next to `plugin.json`;
  `brb_tests.exe` lands in the build tree.

## Release and provenance

`v0.1.0` is the source tag for the package attached to this repository's
release. It is built from this repository at that tag with CMake 3.29,
Ninja and MinGW-w64 GCC 13.2:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The exe is statically linked (the `-static` link option in
[CMakeLists.txt](CMakeLists.txt)), so `bright.exe` does not need
libstdc++/libgcc/winpthread DLLs on the target machine. The release archive
`ra9fael.bright-0.1.0.wsplugin` contains `plugin.json`, `bright.exe`, this
README and the LICENSE; its SHA-256 is in the release notes.

This is a community native plugin: Win顺 0.22.0 or newer, and the plugin
library marks it `unreviewed-native`. Installing and every update require
the user's confirmation; the public source, build command and SHA-256
document provenance but are not a code review.

## Permissions

`permissions: ["ui", "system:settings"]` — that is what this plugin needs
and all it gets:

- A global low-level mouse hook and the overlay window (`ui`).
- Brightness sits on the system-settings surface: WMI, DDC/CI and the gamma
  ramp need no elevation (`system:settings`).

It deliberately does **not** request `elevated`, `processes`, `network` or
clipboard permissions, and it never injects input.

## Data location

Nothing is persisted by default. With `warmRemember` on, the filter state
and strength live in `WS_DATA_DIR\state.json`. No keys, no network, no
telemetry.

## Failure handling

- Remote session (mstsc/RDP) → the list says the session cannot reach the
  physical panel and points at `softwareBrightness`; the layer itself works.
- No WMI brightness class → the monitor falls back to DDC/CI; still not
  answering → the first gesture re-probes it, and the overlay names the reason
  (no handle / VCP read / VCP write / WMI write). With
  `softwareBrightness` on, such a panel is dimmed by the layer instead.
- `bright 70` with nothing adjustable → a hint row, not fake success.
- A failed `SetVCPFeature` → reported, never papered over.
- DDC settle delay: the overlay first shows the predicted value; a ~800 ms
  read-back recalibrates it when off by more than 2% (only the displays that
  write touched).
- Hot-plug / display change → the monitor list and DDC handles rebuild and
  the warm ramp re-applies.
- The gamma ramp taken over by f.lux-class software → last writer wins; the
  plugin does not fight over it.
- The low-level hook removed by the system (timeout) → re-installed on a
  10-second health timer; a failure to re-install is logged.

## Testing

```powershell
ctest --test-dir build          # unit tests (pure logic, 84 cases)
```

Layered suite: 84 pure-logic unit tests (gesture decisions, chord parsing and
matching, per-display targeting, rectangle hit testing, wheel accumulation,
percent/VCP math, the software dim alpha curve, the night window, the hidden
effect's trigger tracker and animation, warm LUT, monitor normalization, query
parsing, settings validation); protocol round-trips verified against the real
process (initialize / query / settingsChanged / hotkey / shutdown over
JSON-RPC, including the `register_hotkeys` and the no-host fallback paths);
the dim layer verified on screen (window class, rectangle, `LWA_ALPHA` value).
Per-monitor hardware brightness, the warm filter's look and the hidden effect
on a real panel still need the manual E2E list in [docs/tests.md](docs/tests.md)
on a console session with a dual-monitor machine.

## License

MIT — see [LICENSE](LICENSE).
