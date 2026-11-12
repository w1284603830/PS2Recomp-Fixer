// F3 trainer (修改器) DISABLED - the default build. 預設屏蔽：此檔取代 ps2_trainer.cpp。
// CMake PS2X_ENABLE_TRAINER=OFF (the default) builds this file instead of ps2_trainer.cpp: F3 is not read, no panel is
// registered with the GS, and ps2_trainer.h turns every switch a port queries into a constant "off" / option 0, so the
// port's hooks compile back to the original game code. 如有需要可以使用：-DPS2X_ENABLE_TRAINER=ON（docs/TRAINER.md）。
#include "ps2_trainer.h"

#if PS2X_TRAINER
#error "ps2_trainer_disabled.cpp is the disabled F3 trainer: build ps2_trainer.cpp when PS2X_TRAINER=1"
#endif

void ps2TrainerHostTick() {}
bool ps2TrainerMenuOpen() { return false; }
void ps2TrainerSetHostPad(uint16_t, bool) {}
