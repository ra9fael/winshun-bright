# DisplayBrightness — Win顺 screen brightness plugin

Ctrl+Alt+wheel anywhere adjusts **all adjustable monitors** at once (built-in
panels over WMI, external monitors over DDC/CI); Ctrl+Alt+click toggles a
**warm color filter**. The `bright` keyword does the same from the keyboard,
per monitor. Overlay and gesture architecture shared with
[winshun-volume](https://github.com/ra9fael/winshun-volume).

English code/comments per project convention; user-facing strings follow the
host locale (zh/en).

## Usage

Mouse (only Ctrl+Alt is recognized — plain Ctrl belongs to the volume
plugin; Alt/Shift/Win combos, simulated input and everything else pass
through untouched):

| Gesture | Effect |
| --- | --- |
| Ctrl+Alt+wheel | all adjustable monitors ±N%; the overlay shows the average |
| Ctrl+Alt+left-click | warm filter on/off (needs `warmStrength` > 0), overlay confirms |
| Recognized, no target | event swallowed, overlay explains why |
| Anything else | passthrough |

Keyboard: type `bright` (or `亮度`). An empty query lists every monitor with
its type, channel (WMI / DDC/CI) and current brightness; `bright 70` sets
every adjustable monitor to 70% (out-of-range input is clamped and the
result row says so). Per-monitor actions on the selected row: Enter +10%,
Ctrl+Enter −10%, Shift+Enter 100% — read-only until an action runs.

A small non-activating overlay shows the new value with a bar while you
adjust, positioned by the `overlayAnchor` setting (bottom-center by default)
and `overlayOffsetX/Y`; it disappears automatically.

## Settings

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `percentPerStep` | number | 5 | wheel step size, 1–10 |
| `gestureEnabled` | bool | true | Ctrl+Alt gestures on/off |
| `showOverlay` | bool | true | overlay window on/off |
| `overlayMs` | number | 600 | overlay linger, 300–3000 ms |
| `overlayAnchor` | choice | bottom-center | nine-grid anchor, or `monitor-under-mouse` |
| `overlayOffsetX` / `overlayOffsetY` | number | 0 | pixel nudges (±4000) |
| `warmStrength` | number | 0 | warm filter depth 0–100; 0 disables it |
| `warmRemember` | bool | false | persist the filter state across restarts |

Invalid values fall back to defaults, never errors.

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

- No WMI brightness class → the monitor falls back to DDC/CI; still not
  answering → listed as 不支持 and gestures explain instead of pretending.
- `bright 70` with nothing adjustable → a hint row, not fake success.
- A failed `SetVCPFeature` → reported, never papered over.
- DDC settle delay: the overlay first shows the predicted value; a ~800 ms
  read-back recalibrates it when off by more than 2%.
- Hot-plug / display change → the monitor list and DDC handles rebuild and
  the warm ramp re-applies.
- The gamma ramp taken over by f.lux-class software → last writer wins; the
  plugin does not fight over it.
- The low-level hook removed by the system (timeout) → rebuilt on the next
  gesture, disabled with a notice after repeated failures.

## Testing

```powershell
ctest --test-dir build          # unit tests (pure logic, 52 cases)
```

Layered suite: 52 pure-logic unit tests (gesture decisions, wheel
accumulation, percent/VCP math, warm LUT, monitor normalization, query
parsing, settings validation); protocol round-trips verified against the
real process (initialize / query / actions / settingsChanged / shutdown
over JSON-RPC); installed in a real Win顺 host and answering queries end to
end. The test machine has a single non-adjustable monitor, so per-monitor
brightness, the overlay during gestures and the warm filter still need the
manual E2E list in [docs/tests.md](docs/tests.md) on a dual-monitor machine.

## License

MIT — see [LICENSE](LICENSE).
