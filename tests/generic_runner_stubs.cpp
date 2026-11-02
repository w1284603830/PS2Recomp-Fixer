#include "Common.h"
bool dodKzvuInstall(PS2Runtime &, const char *) { return true; }
void dodConfigureGamepad(bool) {}
void dodRequestPadRumble(int, uint8_t, uint8_t) {}
bool dodPeekHostPadInput(uint8_t *, size_t) { return false; }
namespace ps2x::iop
{
    void (*g_dodPadRumbleOutput)(int, uint8_t, uint8_t) = nullptr;
    bool (*g_dodPadStateProvider)(int, uint8_t *) = nullptr;
}
namespace ps2_stubs { void setGsGParamAddress(uint32_t) {} }
namespace dodpcsx2 { void configure(const char *, int, int, const char *) {} }
