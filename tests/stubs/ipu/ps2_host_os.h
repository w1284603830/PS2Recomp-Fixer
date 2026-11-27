#pragma once
// Recording stand-in for the runtime's ps2_host_os.h: the generic runner installs the crash handler at start-up.

namespace ps2host
{
    void installCrashHandler();
}
