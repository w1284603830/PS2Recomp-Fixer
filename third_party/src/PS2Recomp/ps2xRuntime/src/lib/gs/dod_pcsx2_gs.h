#pragma once

// [DoD] PCSX2's GS (hardware renderer, pcsx2gs.dll built from third_party/src/pcsx2gs, the Urban Reign port's
// wrapper) as the image source. The runtime keeps parsing the GIF stream for its own state (SIGNAL/FINISH/LABEL
// interrupts, transfers), but sends no draws to the CPU raster: the raw stream of each GIF path, the vsyncs and the
// GS resets go to PCSX2's GS, which presents into a child window over the game window.
//
// Threads: attach/hostTick/active are for the main (window) thread; gif/vsync/reset/readFifo must all come from the
// game thread. The DLL's calls are single-threaded: they run on a GS thread fed by those four in order (DOD_GS_THREAD=0
// keeps them on the game thread). requestSnapshot may be called from any thread.
// Not included by generated code: changing it rebuilds only the runtime files that use it.

#include <cstdint>

struct GSRegisters;

namespace dodpcsx2
{
    // Runner, before attach: dod.ini settings. renderer "pcsx2" (default) or "cpu" (the port's CPU raster);
    // scale = internal resolution multiplier (1..8); vsync 0 off / 1 FIFO / 2 mailbox; options as in
    // pcsx2gs_api.h ("key=value;..."; may be empty).
    void configure(const char *renderer, int scale, int vsync, const char *options);

    // Main thread, after the window exists and before the game thread starts: loads pcsx2gs.dll and creates the
    // child window. False when the CPU raster stays in use.
    bool attach(void *parentHwnd);
    // Main thread, every host frame: keeps the child window over the parent's client area.
    void hostTick();
    // Main thread, after the game thread has joined and before closing the window.
    // Drain the GS queue and close the renderer/HD loader on the rendering thread.
    void shutdown();
    // PCSX2's GS is the image source (attach succeeded). Any thread.
    bool active();
    // Game-thread, ordered before the corresponding native framebuffer uploads.
    bool hdCgSupported();
    const wchar_t* hdCgDefaultRoot();
    void hdCgFrame(const void* data, uint32_t bytes);

    // Game thread. path 1 = VU1 XGKICK, 2 = VIF1 DIRECT, 3 = GIF DMA / FIFO; whole qwords in stream order.
    void gif(int path, const uint8_t *data, uint32_t sizeBytes);
    // Game thread, at vblank start: the privileged registers as the GS has them now.
    void vsync(const GSRegisters &regs);
    // Game thread: CSR.RESET.
    void reset();
    // Game thread: the next sizeBytes of a local -> host transfer (TRXDIR = 1) already sent through gif().
    void readFifo(uint8_t *dst, uint32_t sizeBytes);
    // Any thread: write the image shown at the next vsync as a 24-bit .bmp.
    void requestSnapshot(const char *path);
    // Fields presented so far (any thread).
    uint64_t presents();
    // Any thread: the game's scene changed (0x7E1344); the GS loads and uploads that room's HD textures ahead
    // (pcsx2gs_dod_prefetch_scene; nothing with a DLL that lacks it).
    void prefetchScene(uint32_t scene);


    // Set while the runtime submits a GIF packet it built itself (a synthetic IMAGE tag that continues a tag cut
    // between transfers): PCSX2's GS keeps each path's tag state on its own, so it gets the original bytes instead.
    struct SuppressRaw
    {
        SuppressRaw();
        ~SuppressRaw();
    };
    bool rawSuppressed();
}
