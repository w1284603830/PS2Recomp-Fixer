#ifndef PS2_HOST_REPORT_H
#define PS2_HOST_REPORT_H

// [Fixer] Player-facing performance overlay and diagnostics package, game-neutral (generic host service).
//
// Overlay: once a second the window thread samples the fields presented, the game's own frames (DISPFB changes),
// and the CPU time of the EE and GS threads, and (when on) shows one line at the bottom left of the image through the
// GS's on-screen text: "30 fps (60 fields/s) | EE 48% | GS 10% | D3D11 3x". With it a player's screenshot tells
// whether a slowdown is the CPU (EE near 100 % of a core), the GPU/driver (GS high, EE low) or the game itself
// (frames halved, fields steady). Shown only by the PCSX2 GS (the CPU raster has no text layer).
// Diagnostics package: <exe>\logs\diagnostics-<time>.zip (stored, no compression) with summary.txt (CPU, OS,
// memory, GPU, renderer, the last samples, crash dumps), logs\last-run.log, logs\gpu.txt, the port's settings file
// and the two newest crash dumps. Saves and game data are never included. Written on a worker thread.
// PS2_PERF_OVERLAY=1 turns the overlay on at start (tests).

#include <string>

namespace ps2_host_report
{
    // Window thread: overlay on / off (the port's saved setting, the in-game menu).
    void setOverlay(bool on);
    bool overlay();
    // Window thread, once per host frame (after the menu's tick): samples, updates the overlay.
    void hostTick();
    // Any thread: the last sample as one line ("" before the first second).
    std::string lastSample();
    // Any thread: CPU / GPU / renderer lines for the menu's status page.
    std::string hostSummary();
    // Any thread: starts writing the package (ignored while one is being written); settingsFile is the port's
    // settings file name next to the EXE ("game.ini"; empty = none).
    void exportDiagnostics(const std::string &settingsFile);
    // Any thread: "" before any export, else a short user-facing status (busy / written to ... / failed).
    std::string exportStatus();
}

#endif
