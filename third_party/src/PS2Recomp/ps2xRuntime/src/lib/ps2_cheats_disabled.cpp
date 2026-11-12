// F2 cheat menu (金手指) DISABLED - the default build. 預設屏蔽：此檔取代 ps2_cheats.cpp。
// CMake PS2X_ENABLE_CHEAT_MENU=OFF (the default) builds this file instead of ps2_cheats.cpp. These entry points keep the
// shared input / presentation code unchanged without polling F2, reading cheats.ini or PS2_CHEATS* variables,
// registering a GS menu or writing guest RAM. (Pattern first used by a port that ships without cheats.)
// 如有需要可以使用：-DPS2X_ENABLE_CHEAT_MENU=ON 後按 F2 開啟，金手指由各遊戲自己的 cheats.ini 提供（docs/CHEATS.md）。
#include "ps2_cheats.h"

void ps2CheatsHostTick(uint8_t *) {}
bool ps2CheatsMenuOpen() { return false; }
void ps2CheatsSetHostPad(uint16_t, bool) {}
