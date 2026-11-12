# In-game cheat menu (F2, optional, off by default)

> **繁體中文摘要：** F2 金手指是**選用功能，預設為屏蔽（關閉）狀態**。如有需要可以使用：CMake 加
> `-DPS2X_ENABLE_CHEAT_MENU=ON`（或 `build_all.ps1 -CheatMenu`），遊戲中按 F2 開啟，金手指由各遊戲自己的
> `cheats.ini` 提供。預設關閉時編入的是空殼 `ps2_cheats_disabled.cpp`：不讀 F2、不讀 `cheats.ini`、不寫遊戲記憶體。
> 需要改遊戲程式碼的金手指請用 F3 修改器（[TRAINER.md](TRAINER.md)）。

## Turning it on

| Build | F2 | `cheats.ini`, `PS2_CHEATS*` | Guest RAM |
|---|---|---|---|
| default (`PS2X_ENABLE_CHEAT_MENU=OFF`) | does nothing | not read | never written by a cheat |
| `-DPS2X_ENABLE_CHEAT_MENU=ON` / `build_all.ps1 -CheatMenu` | opens the panel at the left edge | read as below | the enabled entries' writes |

`PS2X_CHEAT_MENU` (`0` or `1`) is a public compile definition of `ps2_runtime`. A port that wants the menu for its
players builds with the option on; a port that ships without cheats keeps the default.

## The panel

The runtime has a small cheat panel, but it ships **no cheats of any game**. You write the cheats for your own game in a
text file, `cheats.ini`, and the panel lists them as checkboxes. Everything starts off at each launch.

## Where the file is

1. the path in the `PS2_CHEATS_FILE` environment variable, else
2. `cheats.ini` next to the executable, else
3. `cheats.ini` in the working directory.

Without a file, F2 still opens the panel and tells you so. See `examples/cheats.ini.example`.

## Format

UTF-8 text (a BOM is fine). Lines starting with `;` or `#` are comments.
Section/checkbox headers and `write` lines also accept trailing comments (put a space or tab before `;` or `#`
on a `write` line). `help` and `note` keep these characters as literal text.

```ini
[section:Player]            ; a title line in the panel, not a checkbox

[Infinite health]           ; a checkbox; the text is its label
help=Shown as a tooltip.
note=Shown in orange under the checkbox (a warning).
write=0x00123456,u16,999    ; ADDRESS,SIZE,VALUE

[Items stay at 99]
write=0x00200000,u8,99,count=10,stride=8,skip_zero
```

`write=ADDRESS,SIZE,VALUE[,count=N][,stride=S][,skip_zero]`

| Field | Meaning |
|---|---|
| `ADDRESS` | EE address in the 32 MB of guest RAM, decimal or `0x` hex (the 0x20000000 / 0x80000000 mirrors map to the same RAM) |
| `SIZE` | `u8`, `u16` or `u32` (also `1`, `2`, `4`), little endian |
| `VALUE` | the value written |
| `count=N` | repeat N times, `stride` bytes apart (an inventory table) |
| `stride=S` | bytes between repeats |
| `skip_zero` | leave a slot alone while it holds 0 (an empty slot stays empty) |

Numbers are unsigned 32-bit integers in decimal (including leading zeros) or `0x` hexadecimal. `VALUE` must fit
the selected size; `count` must be positive, and the complete repeated range must fit in RAM. Negative numbers,
overflow, missing values and trailing junk are rejected instead of silently truncated. A zero/omitted stride
repeats the same address, so it is applied just once per frame.

While a checkbox is on, its writes are applied once per host frame, the way an emulator applies a RAM cheat code. Cheats
that change game *code* are not part of this file: the recompiled functions do not read their instructions from RAM, so
such a cheat has to be a switch you add to the generated source of your own port: the optional F3 trainer
([TRAINER.md](TRAINER.md)) provides the registry, panel and constant-off default for those switches.

Bad lines are skipped and reported on stderr as `[cheats] file:line: ...`.

## Menu input

F2 opens and closes the panel (Esc also closes it). Mouse, arrow keys + space, or the controller's D-pad + A. While it is
open the game gets a released pad. F2 pressed while the F3 trainer panel is open switches over to the F2 panel.

## Testing

* `PS2_CHEATS=all` or `PS2_CHEATS=0,2` turns entries on at launch (numbers count the checkboxes from 0; section titles do
  not count).
* `PS2_CHEAT_MENU_AT=<seconds>` opens the panel after that time.
