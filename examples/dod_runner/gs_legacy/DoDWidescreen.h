#pragma once
// DoD only: source aspect follows the content of each display buffer, not the
// timing of the CPU or the previous present. A movie host upload resets its
// buffer to 4:3; actual projected geometry marks it as anamorphic 16:9.
extern bool g_dodWidescreen;
extern unsigned long long g_dodGsField;
namespace DoDWide
{
inline bool world[512]{};
inline bool display(unsigned bp, unsigned bw, unsigned psm)
{
    return (bp == 0 || bp == 0x40) && bw == 8 && psm == 0;
}
inline void reset() { for (bool& b : world) b = false; }
}
