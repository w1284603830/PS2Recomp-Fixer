#pragma once
// =====================================================================================================================
// F3 trainer (修改器) - OPTIONAL, OFF BY DEFAULT.
// 選用功能，預設為屏蔽（關閉）狀態；如有需要可以使用：CMake 設定 -DPS2X_ENABLE_TRAINER=ON 後，遊戲中按 F3 開啟。
// =====================================================================================================================
// The shared base contains no game's trainer. A port that wants one:
//   1. configures the runtime with -DPS2X_ENABLE_TRAINER=ON (CMake option, default OFF; build_all.ps1 -Trainer),
//   2. registers its switches / choices once, before the game runs (for example in its runner's main),
//   3. reads them in its own hand-patched generated functions or runner hooks, keeping the original code path when a
//      switch is off. Example (a game's damage store, in the port's own generated file):
//          static const ps2trainer::Id kNoDamage = ps2trainer::addSwitch("No damage", "Player HP never drops");
//          if (!ps2trainer::on(kNoDamage)) { /* the original store */ }
//
// With the option OFF (the default) every query below is a compile-time constant (false / option 0), the F3 key does
// nothing, no panel is registered and no guest memory is touched, so a port's hooks compile back to its original code.
// 預設關閉時：所有查詢都是編譯期常數（false／第 0 項），F3 不作用、不註冊面板、不改任何遊戲記憶體。
//
// Invariant that keeps "off" and "on but untouched" identical: every switch starts off and every choice starts at
// option 0 at each launch, and option 0 must be the game's original behaviour. See docs/TRAINER.md.
#include <cstdint>

#ifndef PS2X_TRAINER
#define PS2X_TRAINER 0
#endif

namespace ps2trainer
{
    // True only in a build configured with PS2X_ENABLE_TRAINER=ON.
    constexpr bool kEnabled = PS2X_TRAINER != 0;

    using Id = int; // -1: not registered (always reads as off / option 0)

#if PS2X_TRAINER
    // Registration: any thread, at any time (later entries appear at the end of the panel). At most 256 entries.
    Id addSwitch(const char *label, const char *help = nullptr);
    // 2..32 options; option 0 must be the original behaviour (it is the start value and the disabled build's value).
    Id addChoice(const char *label, const char *const *options, int count, const char *help = nullptr);
    void addSection(const char *title);

    // Queries: lock-free, any thread (the EE thread's generated code included). A value can change between two calls;
    // a hook that needs one value for a whole guest frame reads it once at its own frame boundary.
    bool on(Id id) noexcept;
    int choice(Id id) noexcept;
    // A port's own hotkey or test harness may set a switch (registered switches only).
    void set(Id id, bool value) noexcept;
#else
    inline Id addSwitch(const char *, const char * = nullptr) noexcept { return -1; }
    inline Id addChoice(const char *, const char *const *, int, const char * = nullptr) noexcept { return -1; }
    inline void addSection(const char *) noexcept {}
    constexpr bool on(Id) noexcept { return false; }
    constexpr int choice(Id) noexcept { return 0; }
    inline void set(Id, bool) noexcept {}
#endif
}

// Runtime side: ps2_trainer.cpp when enabled, ps2_trainer_disabled.cpp (no-ops) by default.
// Window thread, once per host frame after the F2 cheat menu's tick: F3 opens / closes the panel.
void ps2TrainerHostTick();
// While the panel is open the game gets a released pad (ps2_pad.cpp), as with the F2 menu.
bool ps2TrainerMenuOpen();
// This frame's host pad buttons (active low, PS2 order) and whether they came from a controller: menu navigation.
void ps2TrainerSetHostPad(uint16_t buttons, bool gamepad);
