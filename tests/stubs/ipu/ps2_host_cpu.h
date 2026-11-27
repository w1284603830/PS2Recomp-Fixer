#pragma once
// Recording stand-in for the runtime's ps2_host_cpu.h: the generic runner only asks whether the host meets the baseline.
#include <string>

namespace ps2host
{
    bool cpuMeetsBaseline(std::string *missing = nullptr);
}
