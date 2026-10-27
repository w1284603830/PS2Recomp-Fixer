// pcsx2gs.dll: the C interface of include/pcsx2gs_api.h over PCSX2's GS (docs/GS_PCSX2.md, phase 2).
#include "pcsx2gs_api.h"
#include "pcsx2gs_host.h"

#include "Config.h"
#include "GS/GS.h"
#include "GS/DoDWidescreen.h"
#include "GS/DoDHdCg.h"
#include "GS/DoDHdStatic.h"
#include "GS/GSPerfMon.h"
#include "GS/Renderers/Common/GSRenderer.h"
#include "GS/Renderers/DX11/GSDevice11.h"
#include "GS/Renderers/HW/GSTextureCache.h"
#include "GS/Renderers/HW/GSTextureReplacements.h"
#include "common/ScopedGuard.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <psapi.h>

extern Pcsx2Config EmuConfig;
extern bool g_dodExclusiveFullscreen; // HostStubs.cpp
bool g_dodWidescreen = false;
unsigned long long g_dodGsField = 0;

namespace
{
    // The GS reads its privileged registers (PMODE, DISPFBn, CSR...) from this block, in the PS2 layout.
    alignas(16) u8 g_regs[0x2000] = {};
    bool g_open = false;
    WindowInfo g_window;
    // [DoD] dump_field: the field (counted from the open, as the host's [fps] / snapshot fields) whose draws PCSX2
    // dumps; its own frame counter skips fields it does not present.
    long long g_dumpField = -1;
    long long g_fields = 0;
    int g_dumpFrames = 1;

    // [DoD] Shin Onimusha: Dawn of Dreams' entry in PCSX2's GameIndex.yaml (SLPM-66275 / SLPM-66276, same as
    // SLUS-21180 / SLUS-21362): recommendedBlendingLevel 2, halfPixelOffset 5, nativeScaling 2, bilinearUpscale 2.
    // PCSX2 applies them through its game database; here they are fixed. (Urban Reign's port had its own set.)
    void ApplyGameFixes(Pcsx2Config::GSOptions &options)
    {
        options.AccurateBlendingUnit = AccBlendLevel::Medium;
        options.UserHacks_HalfPixelOffset = GSHalfPixelOffset::NativeWTexOffset;
        options.UserHacks_NativeScaling = GSNativeScaling::Aggressive;
        options.UserHacks_BilinearHack = GSBilinearDirtyMode::ForceNearest;
    }

    // Pcsx2GsOpenParams::options: "key=value;key=value".
    void ApplyOptions(Pcsx2Config::GSOptions &options, const char *text)
    {
        if (!text)
            return;
        // [DoD] HD pack loading (GSTextureReplacements::DodConfigure): texture_threads (loader threads, 1-8),
        // texture_cache_mb (cap on the loaded CPU copies, 0 = none), texture_upload_mb (GPU uploads per vsync,
        // 0 = no limit), texture_keep_mb (replacement VRAM the hash cache keeps for a minute instead of 30 frames),
        // texture_prefetch (room prefetch list; default <texture_dir>\dod_prefetch.txt, 0 = off).
        int texThreads = 3, texCacheMb = 1536, texUploadMb = 16, texKeepMb = 2048;
        std::string texPrefetch;
        ScopedGuard configureTextures([&]() {
            GSTextureReplacements::DodConfigure(static_cast<u32>(std::clamp(texThreads, 1, 8)), static_cast<u32>(std::max(texCacheMb, 0)),
                static_cast<u32>(std::max(texUploadMb, 0)), static_cast<u32>(std::max(texKeepMb, 0)), texPrefetch);
        });
        std::string_view rest(text);
        while (!rest.empty())
        {
            const size_t end = rest.find(';');
            const std::string_view item = rest.substr(0, end);
            rest = end == std::string_view::npos ? std::string_view() : rest.substr(end + 1);
            const size_t eq = item.find('=');
            if (eq == std::string_view::npos)
                continue;
            const std::string_view key = item.substr(0, eq);
            const std::string text = std::string(item.substr(eq + 1));
            const int value = std::atoi(text.c_str());
            if (key == "texture_dir")
            {
                // Replacement textures (PNG/DDS in <dir>/replacements), loaded as the game uses them.
                pcsx2gs::SetTextureFolder(text);
                options.LoadTextureReplacements = !text.empty();
                options.LoadTextureReplacementsAsync = true;
                continue;
            }
            const auto clampU8 = [](int v, int lo, int hi) { return static_cast<u8>(std::clamp(v, lo, hi)); };
            if (key == "fxaa")
                options.FXAA = value != 0;
            else if (key == "fxaa_quality")
                options.FXAAQuality = (value == 2 || value == 4 || value == 8 || value == 16) ? value : 4;
            // [DoD] The experimental geometry MSAA (GSDevice11MSAA.inl) is retired: it resolved and re-copied the whole
            // target around every non-MSAA draw and averaged the alpha/depth the game's effects read back. Antialiasing
            // is now supersampling (dod.ini ssaa, a higher internal resolution) plus optional FXAA; old msaa values
            // are ignored.
            else if (key == "msaa")
            {
                if (value > 1)
                    std::fprintf(stderr, "[aa] msaa=%d ignored (MSAA removed; use ssaa in dod.ini)\n", value);
            }
            else if (key == "widescreen")
                g_dodWidescreen = value == 1;
            else if (key == "texture_filtering")
                options.TextureFiltering = static_cast<BiFiltering>(std::clamp(value, 0, 3));
            else if (key == "trilinear")
                options.TriFilter = static_cast<TriFiltering>(std::clamp(value, -1, 2));
            else if (key == "anisotropy")
                options.MaxAnisotropy = clampU8(value, 0, 16);
            else if (key == "cas")
                options.CASMode = static_cast<GSCASMode>(std::clamp(value, 0, 2));
            else if (key == "cas_sharpness")
                options.CAS_Sharpness = clampU8(value, 0, 100);
            else if (key == "shadeboost")
                options.ShadeBoost = value != 0;
            else if (key == "brightness")
                options.ShadeBoost_Brightness = clampU8(value, 1, 100);
            else if (key == "contrast")
                options.ShadeBoost_Contrast = clampU8(value, 1, 100);
            else if (key == "saturation")
                options.ShadeBoost_Saturation = clampU8(value, 1, 100);
            else if (key == "gamma")
                options.ShadeBoost_Gamma = clampU8(value, 1, 100);
            else if (key == "tv_shader")
                options.TVShader = clampU8(value, 0, 7);
            else if (key == "present_filter")
                options.LinearPresent = static_cast<GSPostBilinearMode>(std::clamp(value, 0, 2));
            else if (key == "dithering")
                options.Dithering = clampU8(value, 0, 3);
            else if (key == "texture_precache")
                options.PrecacheTextureReplacements = value != 0;
            else if (key == "texture_dump")
                options.DumpReplaceableTextures = value != 0;
            else if (key == "texture_threads")
                texThreads = value;
            else if (key == "texture_cache_mb")
                texCacheMb = value;
            else if (key == "texture_upload_mb")
                texUploadMb = value;
            else if (key == "texture_keep_mb")
                texKeepMb = value;
            else if (key == "texture_prefetch")
                texPrefetch = text;
            else if (key == "blending")
                options.AccurateBlendingUnit = static_cast<AccBlendLevel>(std::clamp(value, 0, 5));
            // [DoD] PCSX2's upscaling fixes, to override ApplyGameFixes (A/B tests and per-setting choices):
            // native_scaling 0 off / 1 normal / 2 aggressive / 3 normal upscaled / 4 aggressive upscaled,
            // bilinear_upscale 0 auto / 1 bilinear / 2 nearest, half_pixel_offset 0-5 (GSHalfPixelOffset),
            // round_sprite 0 off / 1 half / 2 full, align_sprite, merge_sprite, tc_offset_x / _y, autoflush 0-2.
            else if (key == "native_scaling")
                options.UserHacks_NativeScaling = static_cast<GSNativeScaling>(std::clamp(value, 0, 4));
            else if (key == "bilinear_upscale")
                options.UserHacks_BilinearHack = static_cast<GSBilinearDirtyMode>(std::clamp(value, 0, 2));
            else if (key == "half_pixel_offset")
                options.UserHacks_HalfPixelOffset = static_cast<GSHalfPixelOffset>(std::clamp(value, 0, 5));
            else if (key == "round_sprite")
                options.UserHacks_RoundSprite = static_cast<s8>(std::clamp(value, 0, 2));
            else if (key == "align_sprite")
                options.UserHacks_AlignSpriteX = value != 0;
            else if (key == "merge_sprite")
                options.UserHacks_MergePPSprite = value != 0;
            else if (key == "tc_offset_x")
                options.UserHacks_TCOffsetX = value;
            else if (key == "tc_offset_y")
                options.UserHacks_TCOffsetY = value;
            else if (key == "autoflush")
                options.UserHacks_AutoFlush = static_cast<GSHWAutoFlushLevel>(std::clamp(value, 0, 2));
            // [DoD] exclusive_fullscreen=1: DXGI exclusive fullscreen in the desktop's current mode (HostStubs.cpp).
            else if (key == "exclusive_fullscreen")
                g_dodExclusiveFullscreen = value != 0;
            // [DoD] diagnostics: PCSX2's draw dumps (render target / texture / depth of every draw in a frame
            // range) into dump_dir; frames count from PCSX2's own vsync counter.
            else if (key == "dump_dir")
            {
                options.HWDumpDirectory = text;
                options.DumpGSData = !text.empty();
                options.SaveDrawCount = -1;
            }
            else if (key == "dump_field")
            {
                g_dumpField = std::atoll(text.c_str());
                options.SaveFrameStart = 0x7FFFFFFF; // armed at that field (Vsync)
            }
            else if (key == "dump_frames")
                g_dumpFrames = std::max(1, value);
            else if (key == "dump_rt")
                options.SaveRT = value != 0;
            else if (key == "dump_tex")
                options.SaveTexture = value != 0;
            else if (key == "dump_depth")
                options.SaveDepth = value != 0;
            else if (key == "dump_output")
                options.SaveFrame = value != 0;
            else if (key == "dump_info")
                options.SaveInfo = value != 0;
        }
    }

    int Open(const Pcsx2GsOpenParams *params)
    {
        if (g_open || !params || !params->hwnd)
            return 0;
        g_window = {};
        g_window.type = WindowInfo::Type::Win32;
        g_window.window_handle = params->hwnd;
        g_window.surface_width = std::max(1u, params->width);
        g_window.surface_height = std::max(1u, params->height);
        pcsx2gs::SetRenderWindow(g_window);
        pcsx2gs::SetFolders(params->resources ? params->resources : "", params->cache ? params->cache : "");

        Pcsx2Config::GSOptions options;
        options.Renderer = GSRendererType::DX11;
        options.UpscaleMultiplier = std::max(1.0f, params->scale);
        options.AspectRatio = params->stretch ? AspectRatioType::Stretch : AspectRatioType::R4_3;
        ApplyGameFixes(options);
        ApplyOptions(options, params->options);
        if (g_dodWidescreen)
            options.AspectRatio = AspectRatioType::R16_9;
        options.MSAASamples = 1;
        EmuConfig.GS = options;
        // The presentation reads the current aspect ratio (PCSX2's VMManager copies it from GS.AspectRatio at boot);
        // without it [Video] Stretch=1 still drew 4:3 (PROGRESS §100).
        EmuConfig.CurrentAspectRatio = options.AspectRatio;

        const GSVSyncMode vsync = params->vsync == 1u ? GSVSyncMode::FIFO
                                  : params->vsync == 2u ? GSVSyncMode::Mailbox
                                                        : GSVSyncMode::Disabled;
        if (!GSopen(options, GSRendererType::DX11, g_regs, vsync, false))
        {
            std::fprintf(stderr, "[pcsx2-gs] GSopen failed\n");
            return 0;
        }
        g_open = true;
        DoDHdStatic::Configure();
        // [DoD] The host's GS thread runs until the process exits, so Close() never runs and the thread is already
        // gone (killed mid-frame) when the DLL unloads: the static renderer's and texture cache's destructors then
        // crashed in ~GSTextureCache -> RemoveAll on the device. Registered after the statics, this runs before their
        // destructors and leaves the renderer, the texture cache and the device to the OS.
        static bool exitRegistered = false;
        if (!exitRegistered)
        {
            exitRegistered = true;
            std::atexit([] {
                (void)g_gs_renderer.release();
                (void)g_texture_cache.release();
                (void)g_gs_device.release();
            });
        }
        int w = 0, h = 0;
        GSgetInternalResolution(&w, &h);
        std::fprintf(stderr, "[pcsx2-gs] DX11, scale %.0f (%dx%d), window %ux%u, options: %s\n", options.UpscaleMultiplier, w, h,
                     g_window.surface_width, g_window.surface_height, params->options ? params->options : "");
        return 1;
    }

    void Close()
    {
        if (!g_open)
            return;
        DoDHdCg::Clear();
        DoDHdStatic::Clear();
        GSclose();
        g_open = false;
    }

    void Gif(const uint8_t *data, uint32_t qwc)
    {
        if (g_open && data && qwc)
            GSgifTransfer(data, qwc);
    }

    // PS2X_PRESENT_STATS=<file>: after each present, the swap chain's frame statistics (which present is on screen
    // and since which monitor refresh): what the monitor really showed, which the present times alone cannot tell
    // with mailbox vsync (tools/present_stats.py).
    void WritePresentStats()
    {
        static std::FILE *file = []() -> std::FILE *
        {
            const char *path = std::getenv("PS2X_PRESENT_STATS");
            std::FILE *f = (path && *path) ? std::fopen(path, "w") : nullptr;
            if (f)
                std::fputs("qpc,last_present,shown_present,shown_refresh,sync_refresh,sync_qpc\n", f);
            return f;
        }();
        GSDevice11 *device = file ? GSDevice11::GetInstance() : nullptr;
        IDXGISwapChain1 *swapChain = device ? device->GetSwapChain() : nullptr;
        if (!swapChain)
            return;
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        UINT last = 0;
        swapChain->GetLastPresentCount(&last);
        DXGI_FRAME_STATISTICS stats{};
        if (FAILED(swapChain->GetFrameStatistics(&stats)))
            stats = {};
        std::fprintf(file, "%lld,%u,%u,%u,%u,%lld\n", static_cast<long long>(now.QuadPart), last, stats.PresentCount,
                     stats.PresentRefreshCount, stats.SyncRefreshCount, static_cast<long long>(stats.SyncQPCTime.QuadPart));
    }

    // Opt-in comparison telemetry. CPU percent is one logical core = 100%;
    // local video-memory usage is this process's DXGI allocation, not the whole GPU.
    void WriteAaProfile()
    {
        static const bool enabled = std::getenv("DOD_AA_PROFILE") != nullptr;
        if (!enabled) return;
        static unsigned long long fields = 0, lastTime = GetTickCount64(), lastCpu = 0;
        if ((++fields % 300) != 0) return;
        FILETIME created, exited, kernel, user;
        GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
        const auto ticks = [](FILETIME t) { return (static_cast<unsigned long long>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
        const auto now = GetTickCount64(), cpu = ticks(kernel) + ticks(user);
        const double cpuPercent = now > lastTime ? 100.0 * (cpu - lastCpu) / ((now - lastTime) * 10000.0) : 0;
        lastTime = now; lastCpu = cpu;
        PROCESS_MEMORY_COUNTERS pm = {}; pm.cb = sizeof(pm);
        K32GetProcessMemoryInfo(GetCurrentProcess(), &pm, sizeof(pm));
        wil::com_ptr_nothrow<IDXGIDevice> dxgi;
        wil::com_ptr_nothrow<IDXGIAdapter> adapter;
        wil::com_ptr_nothrow<IDXGIAdapter3> adapter3;
        DXGI_QUERY_VIDEO_MEMORY_INFO memory = {};
        bool known = SUCCEEDED(GSDevice11::GetInstance()->GetD3DDevice()->QueryInterface(IID_PPV_ARGS(dxgi.put()))) &&
            SUCCEEDED(dxgi->GetAdapter(adapter.put())) && SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(adapter3.put()))) &&
            SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory));
        std::fprintf(stderr, "[aa-profile] field=%llu cpu_core_pct=%.2f working_set_mib=%.2f local_vram_mib=%.2f\n",
            fields, cpuPercent, pm.WorkingSetSize / 1048576.0, known ? memory.CurrentUsage / 1048576.0 : -1.0);
    }

    void Vsync(const Pcsx2GsPrivRegs *regs)
    {
        if (!g_open || !regs)
            return;
        for (u32 i = 0; i < 15u; ++i)
            std::memcpy(g_regs + i * 0x10u, &regs->regset1[i], 8);
        std::memcpy(g_regs + 0x1000, &regs->csr, 8);
        std::memcpy(g_regs + 0x1010, &regs->imr, 8);
        std::memcpy(g_regs + 0x1040, &regs->busdir, 8);
        std::memcpy(g_regs + 0x1080, &regs->siglblid, 8);
        if (g_dumpField >= 0 && ++g_fields == g_dumpField)
        {
            // PCSX2 counts the frame at this vsync's present (or not, when it skips presenting it): one more covers both
            GSConfig.SaveFrameStart = g_perfmon.GetFrame();
            GSConfig.SaveFrameCount = g_dumpFrames + 1;
            std::fprintf(stderr, "[pcsx2-gs] dumping the draws of field %lld (PCSX2 frame %d) to %s\n", g_dumpField,
                         GSConfig.SaveFrameStart, GSConfig.HWDumpDirectory.c_str());
        }
        // As PCSX2's MTGS: CSR.FIELD (bit 13) picks the field.
        ++g_dodGsField;
        GSvsync((regs->csr & 0x2000u) ? 0u : 1u, true);
        WritePresentStats();
        WriteAaProfile();
    }

    void Reset()
    {
        DoDHdCg::Clear();
        DoDHdStatic::Clear();
        DoDWide::reset();
        if (g_open)
        {
            GSreset(false);
            DoDHdStatic::Configure();
        }
    }

    void ReadFifo(uint8_t *dst, uint32_t qwc)
    {
        if (g_open && dst && qwc)
            GSInitAndReadFIFO(dst, qwc);
    }

    void Resize(uint32_t width, uint32_t height)
    {
        if (!g_open || width == 0u || height == 0u)
            return;
        g_window.surface_width = width;
        g_window.surface_height = height;
        pcsx2gs::SetRenderWindow(g_window);
        GSResizeDisplayWindow(width, height, 1.0f);
    }

    void SetScale(float scale)
    {
        if (!g_open)
            return;
        Pcsx2Config::GSOptions options = GSConfig;
        options.UpscaleMultiplier = std::max(1.0f, scale);
        EmuConfig.GS = options;
        GSUpdateConfig(options);
    }

    // As GSState::Defrost: a hardware reset (the HW renderer empties its texture cache and targets), then the memory.
    void LoadVram(const uint8_t *data, uint32_t size)
    {
        if (!g_open || !data || !g_gs_renderer)
            return;
        g_gs_renderer->Reset(true);
        std::memcpy(g_gs_renderer->m_mem.m_vm8, data, std::min<size_t>(size, g_gs_renderer->m_mem.m_vmsize));
    }

    uint32_t SaveVram(uint8_t *dst, uint32_t size)
    {
        if (!g_open || !dst || !g_gs_renderer)
            return 0;
        g_gs_renderer->Flush(GSState::GSFlushReason::SAVESTATE);
        g_gs_renderer->ReadbackTextureCache();
        const uint32_t bytes = std::min<uint32_t>(size, g_gs_renderer->m_mem.m_vmsize);
        std::memcpy(dst, g_gs_renderer->m_mem.m_vm8, bytes);
        return bytes;
    }

    // As GSState::InitReadFIFO, for an arbitrary transfer and leaving m_env/m_tr alone.
    uint32_t ReadLocal(uint8_t *dst, uint32_t size, uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg)
    {
        if (!g_open || !dst || !size || !g_gs_renderer)
            return 0;
        GIFRegBITBLTBUF blit{bitbltbuf};
        GIFRegTRXPOS pos{trxpos};
        GIFRegTRXREG reg{trxreg};
        g_gs_renderer->Flush(GSState::DOWNLOADFIFO);
        const GSVector4i rect(pos.SSAX, pos.SSAY, pos.SSAX + reg.RRW, pos.SSAY + reg.RRH);
        g_gs_renderer->InvalidateLocalMem(blit, rect);
        int x = pos.SSAX, y = pos.SSAY;
        g_gs_renderer->m_mem.ReadImageX(x, y, dst, static_cast<int>(size), blit, pos, reg);
        return size;
    }

    int Snapshot(const char *path)
    {
        u32 width = 0, height = 0;
        std::vector<u32> pixels;
        if (!g_open || !path || !GSSaveSnapshotToMemory(0, 0, false, false, &width, &height, &pixels) || !width || !height)
            return 0;
        std::FILE *file = std::fopen(path, "wb");
        if (!file)
            return 0;
        std::fprintf(stderr, "[snapshot-aspect] field=%llu aspect=%s path=%s\n", g_dodGsField,
            EmuConfig.CurrentAspectRatio == AspectRatioType::R16_9 ? "16:9" : "4:3", path);
        const u32 rowBytes = (width * 3u + 3u) & ~3u;
        const u32 imageBytes = rowBytes * height;
        u8 header[54] = {'B', 'M'};
        const auto put32 = [&](u32 offset, u32 value) { std::memcpy(header + offset, &value, 4); };
        put32(2, 54u + imageBytes);
        put32(10, 54u);
        put32(14, 40u);
        put32(18, width);
        put32(22, height);
        header[26] = 1;
        header[28] = 24;
        put32(34, imageBytes);
        std::fwrite(header, 1, sizeof(header), file);
        std::vector<u8> row(rowBytes, 0);
        for (u32 y = height; y-- > 0;)
        {
            for (u32 x = 0; x < width; ++x)
            {
                const u32 c = pixels[static_cast<size_t>(y) * width + x];
                row[x * 3 + 0] = static_cast<u8>(c >> 16);
                row[x * 3 + 1] = static_cast<u8>(c >> 8);
                row[x * 3 + 2] = static_cast<u8>(c);
            }
            std::fwrite(row.data(), 1, rowBytes, file);
        }
        std::fclose(file);
        return 1;
    }

    const Pcsx2GsApi g_api = {PCSX2GS_API_VERSION, Open, Close, Gif, Vsync, Reset, ReadFifo, Resize, SetScale, LoadVram, SaveVram, ReadLocal, pcsx2gs::SetOsd, Snapshot, pcsx2gs::SetMenu, pcsx2gs::MenuInput};
}

extern "C" __declspec(dllexport) const Pcsx2GsApi *pcsx2gs_get_api(uint32_t version)
{
    return version == PCSX2GS_API_VERSION ? &g_api : nullptr;
}

// Launcher-only capability query; independent device, no GS state or game window.
extern "C" __declspec(dllexport) uint32_t pcsx2gs_msaa_support()
{
    wil::com_ptr_nothrow<ID3D11Device> device;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, device.put(), nullptr, nullptr))) return 0;
    uint32_t mask = 0;
    for (UINT samples : {2u, 4u, 8u})
    {
        UINT color = 0, depth = 0;
        if (SUCCEEDED(device->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, samples, &color)) && color &&
            SUCCEEDED(device->CheckMultisampleQualityLevels(DXGI_FORMAT_D32_FLOAT_S8X24_UINT, samples, &depth)) && depth)
            mask |= samples;
    }
    return mask;
}

// [DoD] The game's current scene (0x7E1344, read by the port on every pad poll): its HD replacements are loaded and
// uploaded while it loads (GSTextureReplacements::DodRequestScenePrefetch). Only stores the number, so unlike the other
// calls it may come from any thread; the GS thread acts on it at its next vsync.
extern "C" __declspec(dllexport) void pcsx2gs_dod_hd_cg_v1(const void* data,uint32_t bytes)
{
    DoDHdCg::Receive(data,bytes);
}

extern "C" __declspec(dllexport) void pcsx2gs_dod_prefetch_scene(uint32_t scene)
{
    GSTextureReplacements::DodRequestScenePrefetch(scene);
}

// [DoD] GIF data of one path (1 = VU1 XGKICK, 2 = VIF1 DIRECT, 3 = GIF DMA), whole qwords, in stream order. Each path
// keeps its own tag state across calls, as on the GS (PCSX2's MTGS sends P1/P2/P3 through Transfer<0/1/2>), so a tag
// cut between two transfers of one path continues correctly even when another path's packet comes in between.
// Exported on its own so that the API table above stays the same.
extern "C" __declspec(dllexport) void pcsx2gs_gif_path(uint32_t path, const uint8_t *data, uint32_t qwc)
{
    if (!g_open || !data || !qwc || !g_gs_renderer)
        return;
    switch (path)
    {
    case 1u: g_gs_renderer->Transfer<0>(data, qwc); break;
    case 2u: g_gs_renderer->Transfer<1>(data, qwc); break;
    case 3u: g_gs_renderer->Transfer<2>(data, qwc); break;
    default: g_gs_renderer->Transfer<3>(data, qwc); break;
    }
}

// The aspect ratio while the game runs (0 = 4:3, 1 = 16:9, 2 = fill the window), read at every present: the port uses
// it to follow the game's own 4:3 / 16:9 option. Exported on its own so that the API table above stays the same.
extern "C" __declspec(dllexport) void pcsx2gs_set_aspect(uint32_t mode)
{
    EmuConfig.CurrentAspectRatio = mode == 2u ? AspectRatioType::Stretch : mode == 1u ? AspectRatioType::R16_9 : AspectRatioType::R4_3;
}
