#pragma once
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "ps2_input_readers.h"

// Optional integration sketch. Call only after a successful loadELF and after
// this port has qualified its game/revision. Nothing includes this automatically.
struct PortHostPolicy
{
    ps2input::Reader input = 0;
    PortHostPolicy() = default;
    PortHostPolicy(const PortHostPolicy&) = delete;
    PortHostPolicy& operator=(const PortHostPolicy&) = delete;
    bool configure(PS2Runtime& runtime, bool qualifiedLatePresentation)
    {
        close();
        input = ps2input::registerReader();
        if (!input) return false;
        runtime.eeScheduler().configurePresentation(qualifiedLatePresentation ?
            ps2host::PresentationPhase::End : ps2host::PresentationPhase::Start);
        return true;
    }
    void movieState(PS2Runtime& runtime, bool active)
    {
        // EE thread: wire native/replacement/fallback begin, abort and end.
        runtime.eeScheduler().setPresentationMovieActive(active);
    }
    bool read(uint8_t (&snapshot)[32]) { return ps2input::readLatest(input, snapshot, sizeof(snapshot)); }
    void close() { if (input) ps2input::unregisterReader(input); input = 0; }
    // Keep this object on the runner's execution lifetime; explicit close must
    // precede shutdown. Validate the native ABI before copying snapshot fields.
};
