// The default build: PS2X_TRAINER and PS2X_CHEAT_MENU are 0. A port's trainer hooks must compile to constants that keep
// the original code path, and the disabled host entry points must never open a panel.
#include "../third_party/src/PS2Recomp/ps2xRuntime/src/lib/ps2_trainer_disabled.cpp"
#include "../third_party/src/PS2Recomp/ps2xRuntime/src/lib/ps2_cheats_disabled.cpp"

#include <cstdio>

static_assert(!ps2trainer::kEnabled, "PS2X_TRAINER must default to 0");
static_assert(!ps2trainer::on(0) && !ps2trainer::on(-1) && ps2trainer::choice(7) == 0, "disabled queries are constant");

// A typical hook in a port's generated code: the original store runs when the switch is off.
constexpr int hookedDamage(int hp, int damage)
{
    return ps2trainer::on(3) ? hp : hp - damage * (ps2trainer::choice(4) + 1);
}
static_assert(hookedDamage(100, 10) == 90, "a disabled trainer is the original behaviour");

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } } while (false)

int main()
{
    static const char *const options[] = {"a", "b"};
    CHECK(ps2trainer::addSwitch("x") == -1);
    CHECK(ps2trainer::addChoice("y", options, 2) == -1);
    ps2trainer::addSection("z");
    ps2trainer::set(0, true);
    CHECK(!ps2trainer::on(0));
    ps2TrainerSetHostPad(0u, true);
    for (int frame = 0; frame < 3; ++frame)
    {
        ps2TrainerHostTick();
        ps2CheatsHostTick(nullptr);
    }
    CHECK(!ps2TrainerMenuOpen());
    CHECK(!ps2CheatsMenuOpen());
    return 0;
}
