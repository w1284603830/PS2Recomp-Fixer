// gs_frontend.cpp asks the PCSX2 GS bridge (ps2xRuntime/src/lib/gs/dod_pcsx2_gs.cpp) whether it presents and reads
// its local-to-host FIFO from it. These suites test the runtime's own CPU GS backend, so the bridge is absent here.
#include "../../ps2xRuntime/src/lib/gs/dod_pcsx2_gs.h"

namespace dodpcsx2
{
    bool active() { return false; }
    void readFifo(uint8_t *, uint32_t) {}
}
