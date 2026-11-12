# F3 trainer panel (optional, off by default)

> **繁體中文摘要：** F3 修改器是**選用功能，預設為屏蔽（關閉）狀態**。如有需要可以使用：CMake 加
> `-DPS2X_ENABLE_TRAINER=ON`（或 `build_all.ps1 -Trainer`），遊戲中按 F3 開啟。本倉庫不附任何遊戲的修改器；
> 各移植在自己的產生碼或 runner 裡登錄開關並讀取。預設關閉時，所有查詢都是編譯期常數（關／第 0 項），F3 不作用，
> 也不會改動任何遊戲記憶體。

The F2 cheat menu ([CHEATS.md](CHEATS.md)) writes RAM values. Some changes cannot be made that way: a recompiled
function does not read its instructions from RAM, so "never lose HP" or "always allow a special move" has to be a
branch in the port's own generated source. The F3 trainer is the shared half of that: a registry of named switches
and choices, a panel to flip them, and queries that compile to the original code path when the tool is disabled.
The base contains no game's trainer.

## Turning it on

| Build | F3 | Queries in the port's code |
|---|---|---|
| default (`PS2X_ENABLE_TRAINER=OFF`) | does nothing; no panel is registered | constant `false` / option `0`: the original code |
| `-DPS2X_ENABLE_TRAINER=ON` / `build_all.ps1 -Trainer` | opens the panel at the right edge | lock-free reads of the panel's values |

`PS2X_TRAINER` (`0` or `1`) is a public compile definition of `ps2_runtime`, so a port's runner and generated code see
the same value. Turning the tool on changes nothing until the user flips a switch: every switch starts off and every
choice starts at option 0 at each launch.

## Adding switches in a port

```cpp
#include "../lib/ps2_trainer.h"   // from a file in ps2xRuntime/src/runner

// Register once, before the game runs (the runner's main, or a static in the hooked file).
static const ps2trainer::Id kNoDamage = ps2trainer::addSwitch("No damage", "Player HP never drops");
static const char *const kSpeeds[] = {"x1 (original)", "x2", "x4"};
static const ps2trainer::Id kSpeed = ps2trainer::addChoice("Move speed", kSpeeds, 3);

// In the hand-patched generated function: keep the original path when the switch is off.
if (!ps2trainer::on(kNoDamage)) {
    /* the original HP store */
}
const float scale = 1.0f + ps2trainer::choice(kSpeed) * 1.0f; // option 0 = the original value
```

Rules:

* **Option 0 is the original behaviour.** It is the start value and the only value a disabled build returns, so
  "disabled" and "enabled but untouched" behave identically.
* Queries (`on`, `choice`) are lock-free and may run on the EE thread at any time. A value can change between two
  queries; a hook that needs one consistent value for a whole guest frame reads it once at its own frame boundary.
* `addSection(title)` adds a heading. At most 256 entries; a choice has 2 to 32 options. Invalid registrations return
  `-1`, which always reads as off / option 0.
* `ps2trainer::set(id, value)` lets a port's own hotkey or test harness change a switch.
* Keep the hooks in the port's generated files or runner. Never add a game's addresses or switches to the shared
  runtime ([AGENTS.md](../AGENTS.md)).
* Regenerating code (`ps2_recomp`) replaces the generated files; re-apply hand patches with the port's own tooling
  (for example `tools/sync_generated_preserve.py`) before building.

## Panel input

F3 opens and closes the panel (Esc also closes it). Mouse, arrow keys + space, or the controller's D-pad + A. While it
is open the game gets a released pad, as with the F2 menu. The two panels share the GS menu: F3 does nothing while
the F2 panel is open, and F2 pressed while the F3 panel is open switches over to the F2 panel.

## Testing

* `PS2_TRAINER=all` or `PS2_TRAINER=0,2` turns switches on at the first host frame (numbers count switches from 0 in
  registration order; sections and choices do not count). An invalid value is reported and ignored.
* `PS2_TRAINER_MENU_AT=<seconds>` opens the panel after that time.
* Headless tests: `tests/test_ps2_trainer.py` (registry, limits, hotkeys, shared menu slot, panel controls, the
  constant-off default build, and the CMake / build-script defaults).
