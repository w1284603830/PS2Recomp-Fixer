# In-game cheat menu (F2)

The runtime has a small cheat panel, but it ships **no cheats of any game**. You write the cheats for your own game in a
text file, `cheats.ini`, and the panel lists them as checkboxes. Everything starts off at each launch.

## Where the file is

1. the path in the `PS2_CHEATS_FILE` environment variable, else
2. `cheats.ini` next to the executable, else
3. `cheats.ini` in the working directory.

Without a file, F2 still opens the panel and tells you so. See `examples/cheats.ini.example`.

## Format

UTF-8 text (a BOM is fine). Lines starting with `;` or `#` are comments.

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

While a checkbox is on, its writes are applied once per host frame, the way an emulator applies a RAM cheat code. Cheats
that change game *code* are not part of this file: the recompiled functions do not read their instructions from RAM, so
such a cheat has to be a switch you add to the generated source of your own port.

Bad lines are skipped and reported on stderr as `[cheats] file:line: ...`.

## Menu input

F2 opens and closes the panel (Esc also closes it). Mouse, arrow keys + space, or the controller's D-pad + A. While it is
open the game gets a released pad.

## Testing

* `PS2_CHEATS=all` or `PS2_CHEATS=0,2` turns entries on at launch (numbers count the checkboxes from 0; section titles do
  not count).
* `PS2_CHEAT_MENU_AT=<seconds>` opens the panel after that time.
