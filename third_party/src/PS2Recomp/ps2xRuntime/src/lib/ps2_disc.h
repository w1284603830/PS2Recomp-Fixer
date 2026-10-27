#pragma once

#include <filesystem>

// [DoD] Disc change: switches PS2Runtime's CdImage path while the IOP (which may run on its own thread and reads that
// path) is held, and closes the IOP cdvd's open image. Defined in ps2_runtime.cpp; used by Kernel/Stubs/CD.cpp, which
// also drops the EE-side file table of the old disc.
void ps2ChangeCdImage(const std::filesystem::path &image);
