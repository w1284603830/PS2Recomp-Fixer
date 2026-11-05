#pragma once

#include <cstdint>

// Generic in-game cheat menu (F2). The runtime ships no cheats of any game: the entries come from a text file the user
// (or the port's author) writes for their own game, `cheats.ini`, looked up in this order:
//   1. the path in the PS2_CHEATS_FILE environment variable
//   2. cheats.ini next to the executable
//   3. cheats.ini in the working directory
// With no file F2 opens a panel that says so. See docs/CHEATS.md for the format. Everything starts off at each launch.

// Window thread, once per host frame before publishing pad input: F2 opens / closes the menu, its input goes to the GS while it
// is open, the enabled RAM writes are applied. rdram = the guest's 32 MB.
void ps2CheatsHostTick(uint8_t *rdram);
// While the menu is open the game gets a released pad (ps2_pad.cpp).
bool ps2CheatsMenuOpen();
// The pad buttons the host read this frame (active low, PS2 order) and whether they came from a controller: the
// menu's controller navigation (ps2_pad.cpp publishes them before it blanks the game's copy).
void ps2CheatsSetHostPad(uint16_t buttons, bool gamepad);
