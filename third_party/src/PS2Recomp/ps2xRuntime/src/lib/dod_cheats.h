#pragma once

#include <cstdint>

// [DoD] In-game cheat menu (F2, dod_cheats.cpp). The switches are read by the generated functions themselves
// (tools/scripts/apply_dod_cheats.py adds `g_dodCheats[n]` at the patched instructions), so the indexes below are fixed:
// the script uses the same numbers.
enum DodCheat : unsigned
{
    kDodCheatChainCount = 0,      // 2B3D50 2B3E80: chain Issen without a count limit
    kDodCheatChainRange = 1,      // 2B26FC: chain Issen finds enemies at any distance
    kDodCheatChainSameEnemy = 2,  // 2B26B4: chain Issen may hit the same enemy again
    kDodCheatOtherIssenRange = 3, // 26010C 260190 26029C: the other Issen kinds find enemies at any distance
    kDodCheatBassari = 4,         // 2B3694: the "slash feel" helper (the chain counter does not count up)
    kDodCheatDeflectIssen = 5,    // 26005C: lock on (R1), then an attack is a deflect Issen
    kDodCheatSpirit = 6,          // 25F14C: spirit gauge always refills
    kDodCheatOniPower = 7,        // 25F114: Oni power refills at once
    kDodCheatOugi = 8,            // 25F2C8: ougi gauge always full
    kDodCheatItems = 9,           // RAM 0x8D729C + 8 n (n < 10): item counts held at 99
    kDodCheatMoneyMax = 10,       // 2F22C8 2F231C: money goes to the maximum when it changes
    kDodCheatKibushiMax = 11,     // 3345B4: Kibushi points go to the maximum when they change
    kDodCheatSoulsKeep = 12,      // 362AF8: upgrading weapons does not use souls
    kDodCheatUpgradeOnce = 13,    // 362B0C: one soul allotment levels a weapon / armour up
    kDodCheatSkill99 = 14,        // 329940: skill upgrades give 99 points (the code's author: may black out)
    kDodCheatCount = 15,
};

extern "C" volatile unsigned char g_dodCheats[32];

// Window thread, once per host frame after the pad poll: F2 opens / closes the menu, its input goes to the GS while it
// is open, the RAM cheats are written. rdram = the guest's 32 MB.
void dodCheatsHostTick(uint8_t *rdram);
// While the menu is open the game gets a released pad (ps2_pad.cpp).
bool dodCheatsMenuOpen();
// The pad buttons the host read this frame (active low, PS2 order) and whether they came from a controller: the
// menu's controller navigation (ps2_pad.cpp publishes them before it blanks the game's copy).
void dodCheatsSetHostPad(uint16_t buttons, bool gamepad);
