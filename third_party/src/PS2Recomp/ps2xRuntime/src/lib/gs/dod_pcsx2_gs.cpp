// [DoD] PCSX2's GS as the image source (dod_pcsx2_gs.h). Adapted from the Urban Reign port's gs_pcsx2_backend.cpp
// (github.com/thiagordsfx/urban-reign-recomp, GPL-3.0): same DLL interface and child window; here the raw GIF is
// taken per path where the runtime submits it (PS2Memory::submitGifPacket) instead of through a raster back end, so
// the generated code's headers stay unchanged.
#include "dod_pcsx2_gs.h"

#include "runtime/ps2_memory.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cfloat>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <emmintrin.h>
#include <xmmintrin.h>

#if defined(_WIN32)
#include "pcsx2gs_api.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#define DOD_HAS_PCSX2_GS 1
#endif

#if DOD_HAS_PCSX2_GS
#include "dod_cg_cadence.h"
#endif

extern "C" void ps2_ipu_report(void); // ipu/ps2_ipu.c
extern std::atomic<uint64_t> g_dodIopEeCyclesTotal; // ps2_runtime.cpp

namespace dodpcsx2
{
    namespace
    {
        thread_local int t_suppress = 0;
    }

    SuppressRaw::SuppressRaw() { ++t_suppress; }
    SuppressRaw::~SuppressRaw() { --t_suppress; }
    bool rawSuppressed() { return t_suppress != 0; }
}

#if DOD_HAS_PCSX2_GS

// Inside dodpcsx2 so that these names cannot meet another file's in a unity build.
namespace dodpcsx2
{
namespace
{
    using GifPathFn = void(__cdecl *)(uint32_t path, const uint8_t *data, uint32_t qwc);
    using HdCgFn = void(__cdecl *)(const void*, uint32_t);
    std::atomic<HdCgFn> g_hdCg{nullptr};
    using PrefetchSceneFn = void(__cdecl *)(uint32_t scene);

    const Pcsx2GsApi *g_api = nullptr;
    GifPathFn g_gifPath = nullptr;
    std::atomic<PrefetchSceneFn> g_prefetchScene{nullptr}; // [DoD] optional export (HD room prefetch)
    HWND g_parent = nullptr;
    HWND g_child = nullptr;
    std::atomic<bool> g_active{false};
    std::atomic<uint32_t> g_clientWidth{0u};
    std::atomic<uint32_t> g_clientHeight{0u};
    std::atomic<uint64_t> g_presents{0u};
    std::atomic<uint64_t> g_queuedVsyncs{0u};

    std::string g_renderer = "pcsx2";
    int g_scale = 1;
    int g_vsync = 2;
    std::string g_options;

    // Game-thread state.
    bool g_open = false;
    bool g_failed = false;
    uint32_t g_width = 0u, g_height = 0u;
    uint64_t g_gifBytes[4] = {};
    std::thread::id g_gsThread{};
    bool g_wrongThreadLogged = false;

    std::mutex g_snapshotMutex;
    std::string g_snapshotPath;

    // [DoD] requestLineCapture: a burst of GS output snapshots (every kBurstStep-th field, kBurstShots of them; light:
    // one) and the GS's dump of the next frames (pcsx2gs_dod_capture: draw table, PNG images; a DLL with only
    // pcsx2gs_dod_dump_now: per-draw info and BMP images; nothing with a DLL that lacks both).
    using DumpNowFn = void(__cdecl *)(const char *dir, uint32_t frames);
    std::atomic<DumpNowFn> g_dumpNow{nullptr};
    std::atomic<Pcsx2GsDodCaptureFn> g_dodCapture{nullptr};
    constexpr uint64_t kBurstStep = 3u, kBurstShots = 10u;
    std::string g_burstDir;      // under g_snapshotMutex
    uint64_t g_burstFirst = 0u;  // 0: starts at the next vsync
    uint64_t g_burstLast = 0u;
    uint64_t g_burstShots = kBurstShots;
    const char *g_burstExt = ".bmp";
    std::atomic<uint32_t> g_lastScene{0xFFFFu};

    std::filesystem::path exeDirectory()
    {
        wchar_t path[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        return length ? std::filesystem::path(path).parent_path() : std::filesystem::current_path();
    }

    std::string utf8(const std::filesystem::path &path)
    {
        const std::u8string text = path.u8string();
        return std::string(reinterpret_cast<const char *>(text.data()), text.size());
    }

    // The child window only shows the GS output: mouse input goes through to the game window.
    LRESULT CALLBACK ChildProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
    {
        switch (msg)
        {
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_ERASEBKGND:
            return 1;
        default:
            return DefWindowProcW(hwnd, msg, wparam, lparam);
        }
    }

    // gif/vsync/reset/readFifo come from one thread (the game thread); a call from elsewhere is logged once. The DLL
    // itself is called from that thread, or with the GS thread (below) only from the GS thread.
    bool onGsThread()
    {
        const std::thread::id self = std::this_thread::get_id();
        if (g_gsThread == std::thread::id{})
            g_gsThread = self;
        if (g_gsThread == self)
            return true;
        if (!g_wrongThreadLogged)
        {
            g_wrongThreadLogged = true;
            std::fprintf(stderr, "[pcsx2-gs] call from a second thread ignored (the GS is single-threaded)\n");
        }
        return false;
    }

    // Opened on the game thread at its first command, as the Urban port does: the main thread (which owns the window
    // and pumps its messages) stays free while DXGI creates the swap chain.
    bool ensureOpen()
    {
        if (g_open)
            return true;
        if (g_failed || !g_api || !g_child)
            return false;
        const std::filesystem::path dir = exeDirectory();
        const std::string resources = utf8(dir / "resources");
        // [DoD] DOD_GS_CACHE_DIR (tests): another shader cache folder, so that test runs side by side do not write
        // the same cache files.
        const char *cacheEnv = std::getenv("DOD_GS_CACHE_DIR");
        const std::filesystem::path cacheDir = cacheEnv && *cacheEnv ? std::filesystem::path(cacheEnv) : dir / "shadercache_pcsx2";
        std::error_code ec;
        std::filesystem::create_directories(cacheDir, ec);
        const std::string cache = utf8(cacheDir);
        g_width = g_clientWidth.load();
        g_height = g_clientHeight.load();
        Pcsx2GsOpenParams params{};
        params.hwnd = g_child;
        params.width = g_width;
        params.height = g_height;
        params.scale = static_cast<float>(std::clamp(g_scale, 1, 8));
        params.resources = resources.c_str();
        params.cache = cache.c_str();
        params.vsync = static_cast<uint32_t>(std::clamp(g_vsync, 0, 2));
        params.stretch = 0u; // the game's 4:3, as the launcher promises
        params.options = g_options.c_str();
        g_open = g_api->open(&params) != 0;
        g_failed = !g_open;
        if (g_failed)
            std::fprintf(stderr, "[pcsx2-gs] the GS failed to open; nothing will be drawn\n");
        return g_open;
    }
}
}

namespace dodpcsx2
{
    void configure(const char *renderer, int scale, int vsync, const char *options)
    {
        if (renderer && *renderer)
        {
            g_renderer = renderer;
            std::transform(g_renderer.begin(), g_renderer.end(), g_renderer.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        }
        g_scale = scale;
        g_vsync = vsync;
        g_options = options ? options : "";
    }

    bool attach(void *parentHwnd)
    {
        if (g_child || !parentHwnd)
            return g_child != nullptr;
        if (g_renderer != "pcsx2")
        {
            std::fprintf(stderr, "[pcsx2-gs] renderer=%s: using the CPU raster\n", g_renderer.c_str());
            return false;
        }
        const std::filesystem::path dll = exeDirectory() / "pcsx2gs.dll";
        HMODULE module = LoadLibraryW(dll.c_str());
        if (!module)
        {
            std::fprintf(stderr, "[pcsx2-gs] %s did not load (error %lu); using the CPU raster\n", utf8(dll).c_str(),
                         GetLastError());
            return false;
        }
        const auto getApi = reinterpret_cast<Pcsx2GsGetApiFn>(GetProcAddress(module, "pcsx2gs_get_api"));
        g_api = getApi ? getApi(PCSX2GS_API_VERSION) : nullptr;
        g_gifPath = reinterpret_cast<GifPathFn>(GetProcAddress(module, "pcsx2gs_gif_path"));
        g_hdCg.store(reinterpret_cast<HdCgFn>(GetProcAddress(module, "pcsx2gs_dod_hd_cg_v1")), std::memory_order_release);
        g_prefetchScene.store(reinterpret_cast<PrefetchSceneFn>(GetProcAddress(module, "pcsx2gs_dod_prefetch_scene")),
                              std::memory_order_release);
        g_dumpNow.store(reinterpret_cast<DumpNowFn>(GetProcAddress(module, "pcsx2gs_dod_dump_now")),
                        std::memory_order_release);
        g_dodCapture.store(reinterpret_cast<Pcsx2GsDodCaptureFn>(GetProcAddress(module, "pcsx2gs_dod_capture")),
                           std::memory_order_release);
        if (!g_api || !g_gifPath)
        {
            std::fprintf(stderr, "[pcsx2-gs] pcsx2gs.dll lacks pcsx2gs_get_api v%u / pcsx2gs_gif_path; using the CPU raster\n",
                         PCSX2GS_API_VERSION);
            g_api = nullptr;
            return false;
        }
        g_parent = static_cast<HWND>(parentHwnd);
        // The game window must not paint over its child (GLFW sets WS_CLIPCHILDREN; made sure here).
        SetWindowLongPtrW(g_parent, GWL_STYLE, GetWindowLongPtrW(g_parent, GWL_STYLE) | WS_CLIPCHILDREN);
        WNDCLASSW wc{};
        wc.lpfnWndProc = ChildProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"dod_pcsx2_gs";
        RegisterClassW(&wc);
        RECT rect{};
        GetClientRect(g_parent, &rect);
        const uint32_t width = static_cast<uint32_t>(std::max<LONG>(1, rect.right - rect.left));
        const uint32_t height = static_cast<uint32_t>(std::max<LONG>(1, rect.bottom - rect.top));
        g_child = CreateWindowExW(WS_EX_NOPARENTNOTIFY, wc.lpszClassName, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0,
                                  static_cast<int>(width), static_cast<int>(height), g_parent, nullptr, wc.hInstance, nullptr);
        if (!g_child)
        {
            std::fprintf(stderr, "[pcsx2-gs] child window creation failed (error %lu); using the CPU raster\n", GetLastError());
            g_api = nullptr;
            return false;
        }
        g_clientWidth.store(width);
        g_clientHeight.store(height);
        g_active.store(true, std::memory_order_release);
        std::fprintf(stderr, "[pcsx2-gs] renderer: PCSX2 GS (pcsx2gs.dll), scale %d, vsync %d, window %ux%u\n", g_scale, g_vsync,
                     width, height);
        return true;
    }

    void hostTick()
    {
        if (!g_child)
            return;
        RECT rect{};
        GetClientRect(g_parent, &rect);
        const uint32_t width = static_cast<uint32_t>(std::max<LONG>(1, rect.right - rect.left));
        const uint32_t height = static_cast<uint32_t>(std::max<LONG>(1, rect.bottom - rect.top));
        if (width == g_clientWidth.load(std::memory_order_relaxed) && height == g_clientHeight.load(std::memory_order_relaxed))
            return;
        SetWindowPos(g_child, nullptr, 0, 0, static_cast<int>(width), static_cast<int>(height), SWP_NOZORDER | SWP_NOACTIVATE);
        g_clientWidth.store(width, std::memory_order_relaxed);
        g_clientHeight.store(height, std::memory_order_relaxed);
    }

    bool active()
    {
        return g_active.load(std::memory_order_acquire);
    }

    // [DoD] DOD_GS_CAPTURE=<file>:<first field>:<fields>: records (u32 path, u32 bytes, data) of what the GS gets;
    // path 0xFFFFFFFF marks the start of a field (bytes = field number). Read by tools/scripts/diag/gif_analyze.py.
    struct GsCapture
    {
        FILE *file = nullptr;
        uint64_t first = 0, last = 0;
        bool parsed = false;
        bool on = false;
    };
    GsCapture g_capture;

    void captureField(uint64_t field)
    {
        if (!g_capture.parsed)
        {
            g_capture.parsed = true;
            if (const char *v = std::getenv("DOD_GS_CAPTURE"))
            {
                std::string spec(v);
                const size_t b = spec.rfind(':');
                const size_t a = b == std::string::npos ? std::string::npos : spec.rfind(':', b - 1);
                if (a != std::string::npos)
                {
                    g_capture.first = std::strtoull(spec.c_str() + a + 1, nullptr, 10);
                    g_capture.last = g_capture.first + std::strtoull(spec.c_str() + b + 1, nullptr, 10);
                    g_capture.file = std::fopen(spec.substr(0, a).c_str(), "wb");
                }
            }
        }
        if (!g_capture.file)
            return;
        g_capture.on = field >= g_capture.first && field < g_capture.last;
        if (g_capture.on)
        {
            const uint32_t header[2] = {0xFFFFFFFFu, static_cast<uint32_t>(field)};
            std::fwrite(header, sizeof(header), 1, g_capture.file);
        }
        else if (field >= g_capture.last)
        {
            std::fclose(g_capture.file);
            g_capture.file = nullptr;
            std::fprintf(stderr, "[pcsx2-gs] capture done\n");
        }
    }

    // The DLL side of gif/vsync/reset/readFifo: on the game thread, or with the GS thread on the GS thread.
    void gifDirect(int path, const uint8_t *data, uint32_t sizeBytes)
    {
        cgtrace::gif(static_cast<unsigned>(path), data, sizeBytes);
        if (g_capture.on && g_capture.file)
        {
            const uint32_t header[2] = {static_cast<uint32_t>(path), sizeBytes & ~15u};
            std::fwrite(header, sizeof(header), 1, g_capture.file);
            std::fwrite(data, 1, sizeBytes & ~15u, g_capture.file);
        }
        // PCSX2's GIF handlers read the qwords with aligned SSE loads (movntdqa): a VIF1 DIRECT payload follows a
        // 4-byte VIFcode in our buffers, and an unaligned pointer faulted inside the DLL (as the Urban port's GS
        // thread does, the data goes through an aligned copy then).
        if ((reinterpret_cast<uintptr_t>(data) & 15u) != 0u)
        {
            static thread_local std::vector<__m128i> aligned;
            const size_t qwords = sizeBytes / 16u;
            if (aligned.size() < qwords)
                aligned.resize(qwords);
            std::memcpy(aligned.data(), data, qwords * 16u);
            data = reinterpret_cast<const uint8_t *>(aligned.data());
        }
        g_gifPath(static_cast<uint32_t>(path), data, sizeBytes / 16u);
        g_gifBytes[std::clamp(path, 0, 3)] += sizeBytes & ~15u;
    }

    void vsyncDirect(Pcsx2GsPrivRegs priv)
    {
        const uint32_t width = g_clientWidth.load(std::memory_order_relaxed);
        const uint32_t height = g_clientHeight.load(std::memory_order_relaxed);
        if (width != g_width || height != g_height)
        {
            g_width = width;
            g_height = height;
            g_api->resize(width, height);
        }
        cgtrace::select(g_presents.load(std::memory_order_relaxed) + 1u, priv);
        const auto presentStart = std::chrono::steady_clock::now();
        g_api->vsync(&priv);
        const uint64_t fields = g_presents.fetch_add(1u, std::memory_order_relaxed) + 1u;
        cgtrace::vsync(fields, priv, std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - presentStart).count());
        captureField(fields);
        {
            // [DoD] DOD_FPS=1: the game's frame rate = display flips (DISPFB changes) per second, every 300 fields.
            static const bool fpsLog = std::getenv("DOD_FPS") != nullptr;
            static uint64_t lastDispfb = 0, flips = 0;
            static auto windowStart = std::chrono::steady_clock::now();
            const uint64_t dispfb = priv.regset1[7] ^ (priv.regset1[9] << 1); // DISPFB1, DISPFB2
            if (dispfb != lastDispfb)
            {
                lastDispfb = dispfb;
                ++flips;
            }
            if (fpsLog && fields % 300u == 0u)
            {
                const auto now = std::chrono::steady_clock::now();
                const double seconds = std::chrono::duration<double>(now - windowStart).count();
                static uint64_t lastEe = 0;
                const uint64_t ee = g_dodIopEeCyclesTotal.load(std::memory_order_relaxed);
                std::fprintf(stderr, "[fps] field %llu: %.1f game frames/s, %.1f fields/s, EE clock %.1f MHz (real 294.9)\n",
                             static_cast<unsigned long long>(fields), flips / seconds, 300.0 / seconds,
                             (ee - lastEe) / seconds / 1e6);
                lastEe = ee;
                flips = 0;
                windowStart = now;
            }
        }
        static const bool regLog = std::getenv("DOD_PCSX2_REGLOG") != nullptr;
        if (regLog && (fields % 120u) == 1u)
            std::fprintf(stderr, "[pcsx2-gs] field %llu pmode %llX smode1 %llX smode2 %llX dispfb1 %llX display1 %llX dispfb2 %llX display2 %llX bg %llX csr %llX\n",
                         static_cast<unsigned long long>(fields), static_cast<unsigned long long>(priv.regset1[0]),
                         static_cast<unsigned long long>(priv.regset1[1]), static_cast<unsigned long long>(priv.regset1[2]),
                         static_cast<unsigned long long>(priv.regset1[7]), static_cast<unsigned long long>(priv.regset1[8]),
                         static_cast<unsigned long long>(priv.regset1[9]), static_cast<unsigned long long>(priv.regset1[10]),
                         static_cast<unsigned long long>(priv.regset1[14]), static_cast<unsigned long long>(priv.csr));
        static const bool ipuReport = std::getenv("DOD_IPU_REPORT") != nullptr; // [DoD] IPU command counts every 600 fields
        if (ipuReport && fields % 600u == 0u)
            ps2_ipu_report();
        if (fields == 60u || fields % 3600u == 0u)
            std::fprintf(stderr, "[pcsx2-gs] %llu fields, GIF MB path1 %.1f path2 %.1f path3 %.1f\n",
                         static_cast<unsigned long long>(fields), g_gifBytes[1] / 1048576.0, g_gifBytes[2] / 1048576.0,
                         g_gifBytes[3] / 1048576.0);
        std::string snapshot;
        {
            std::lock_guard<std::mutex> lock(g_snapshotMutex);
            snapshot.swap(g_snapshotPath);
            if (snapshot.empty() && !g_burstDir.empty())
            {
                if (g_burstFirst == 0u)
                    g_burstFirst = fields, g_burstLast = fields + kBurstStep * g_burstShots;
                if (fields >= g_burstLast)
                    g_burstDir.clear();
                else if ((fields - g_burstFirst) % kBurstStep == 0u)
                    snapshot = g_burstDir + "/field_" + std::to_string(fields) + g_burstExt;
            }
        }
        {
            // [DoD] DOD_PCSX2_FIELDS=<dir>:<first field>:<count>: every field in the window, its display registers on
            // stderr and PCSX2's presented image as <dir>/field_<n>.bmp (to look for per-field jitter).
            struct FieldShots { std::string dir; uint64_t first = 0, last = 0; };
            static const FieldShots shots = []() {
                FieldShots f;
                if (const char *v = std::getenv("DOD_PCSX2_FIELDS"))
                {
                    std::string text(v);
                    const size_t b = text.rfind(':'), a = b == std::string::npos ? b : text.rfind(':', b - 1);
                    if (a != std::string::npos && a > 0)
                    {
                        f.dir = text.substr(0, a);
                        f.first = std::strtoull(text.c_str() + a + 1, nullptr, 10);
                        f.last = f.first + std::strtoull(text.c_str() + b + 1, nullptr, 10);
                    }
                }
                return f;
            }();
            if (!shots.dir.empty() && fields >= shots.first && fields < shots.last)
            {
                std::fprintf(stderr, "[pcsx2-field] %llu pmode %llX smode2 %llX dispfb1 %llX display1 %llX dispfb2 %llX display2 %llX csr %llX\n",
                             static_cast<unsigned long long>(fields), static_cast<unsigned long long>(priv.regset1[0]),
                             static_cast<unsigned long long>(priv.regset1[2]), static_cast<unsigned long long>(priv.regset1[7]),
                             static_cast<unsigned long long>(priv.regset1[8]), static_cast<unsigned long long>(priv.regset1[9]),
                             static_cast<unsigned long long>(priv.regset1[10]), static_cast<unsigned long long>(priv.csr));
                if (snapshot.empty())
                    snapshot = shots.dir + "/field_" + std::to_string(fields) + ".bmp";
            }
        }
        if (!snapshot.empty() && !g_api->snapshot(snapshot.c_str()))
            std::fprintf(stderr, "[pcsx2-gs] snapshot %s failed\n", snapshot.c_str());
    }

    void resetDirect()
    {
        cgtrace::submitted(0, 0, false);
        for (auto& path : cgtrace::state.paths) path = {};
        if (g_open)
            g_api->reset();
    }

    void readFifoDirect(uint8_t *dst, uint32_t sizeBytes)
    {
        const uint32_t qwc = (sizeBytes + 15u) / 16u;
        if ((sizeBytes & 15u) == 0u && (reinterpret_cast<uintptr_t>(dst) & 15u) == 0u)
        {
            g_api->read_fifo(dst, qwc);
            return;
        }
        std::vector<__m128i> scratch(qwc); // aligned, as PCSX2 writes whole qwords
        g_api->read_fifo(reinterpret_cast<uint8_t *>(scratch.data()), qwc);
        std::memcpy(dst, scratch.data(), sizeBytes);
    }

    // [DoD] GS thread (DOD_GS_THREAD=0 turns it off): PCSX2's GS draws and presents on its own thread, as in PCSX2
    // (MTGS) and the Urban Reign port (gs_threaded_backend.cpp). The game thread only copies each command into a
    // 64 KB chunk; full chunks, and the open one at every vsync, go to the GS thread in order. At most two fields
    // may wait for the GS thread (PCSX2's VsyncQueueSize); the game thread waits beyond that, and readFifo waits
    // for everything queued before it. The game thread's MXCSR travels with each command.
    // [DoD] DoD renders its interlaced fields (SMODE2 FFMD, 224-line fields at 60 fps) with the 3D half a line apart on
    // alternate fields (XYOFFSET OFY x.5 on one buffer, x.0 on the other) for a TV that puts odd fields one scanline
    // lower; its 2D is drawn at the same place on both. PCSX2 shows the fields progressive (SYNCV left as the port sets
    // it), so the 3D shook by a line from field to field. This takes the half line out of the copy of the GIF stream
    // that PCSX2's GS gets (the game's memory and state are untouched): every field lines up. A tag walker per GIF
    // path, because tags continue across transfers. DOD_GS_FIELD_OFFSET=keep sends the stream as it is.
    struct GifFieldOffsetFix
    {
        uint32_t left = 0u;
        uint32_t nreg = 1u;
        uint32_t idx = 0u;
        uint32_t flg = 0u;
        uint8_t regs[16]{};
    };
    GifFieldOffsetFix g_fieldFix[4];

    bool fieldOffsetFixOn()
    {
        static const bool on = []() { const char *v = std::getenv("DOD_GS_FIELD_OFFSET"); return !(v && std::strcmp(v, "keep") == 0); }();
        return on;
    }

    void fixFieldOffsets(uint32_t path, uint8_t *data, uint32_t bytes)
    {
        GifFieldOffsetFix &st = g_fieldFix[path & 3u];
        for (uint32_t pos = 0u; pos + 16u <= bytes; pos += 16u)
        {
            uint64_t lo = 0u;
            uint64_t hi = 0u;
            std::memcpy(&lo, data + pos, 8u);
            std::memcpy(&hi, data + pos + 8u, 8u);
            if (st.left == 0u)
            {
                const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu);
                st.flg = static_cast<uint32_t>((lo >> 58u) & 3u);
                st.nreg = static_cast<uint32_t>((lo >> 60u) & 0xFu);
                if (st.nreg == 0u)
                    st.nreg = 16u;
                for (uint32_t i = 0u; i < 16u; ++i)
                    st.regs[i] = static_cast<uint8_t>((hi >> (4u * i)) & 0xFu);
                st.idx = 0u;
                st.left = st.flg == 0u ? nloop * st.nreg : st.flg == 1u ? (nloop * st.nreg + 1u) / 2u : nloop;
                continue;
            }
            if (st.flg == 0u)
            {
                if (st.regs[st.idx % st.nreg] == 0xEu) // A+D
                {
                    const uint32_t addr = static_cast<uint32_t>(hi & 0xFFu);
                    if ((addr == 0x18u || addr == 0x19u) && ((lo >> 32u) & 0xFu) == 8u) // XYOFFSET_1/2, OFY x.5
                    {
                        // DOD_GS_FIELD_LOG=<n>: the first n changes (path, offset, the qword before the change)
                        static const int logLeft = []() { const char *v = std::getenv("DOD_GS_FIELD_LOG"); return v ? std::atoi(v) : 0; }();
                        static int logged = 0;
                        if (logged < logLeft)
                        {
                            ++logged;
                            std::fprintf(stderr, "[gs-field] path %u pos %u nreg %u idx %u left %u lo %016llX hi %016llX\n", path, pos,
                                         st.nreg, st.idx, st.left, static_cast<unsigned long long>(lo),
                                         static_cast<unsigned long long>(hi));
                        }
                        lo &= ~(0xFull << 32u);
                        std::memcpy(data + pos, &lo, 8u);
                    }
                }
                ++st.idx;
            }
            --st.left;
        }
    }

    // [DoD] std::vector::resize value-initializes (zero-fills) the new qwords, which push() then overwrites at once:
    // 8% of the EE thread's memcpy/memset time in room 0x386 (RUNTIME_PATCHES row 120). New qwords stay uninitialized.
    template <class T>
    struct DefaultInitAllocator : std::allocator<T>
    {
        template <class U>
        struct rebind
        {
            using other = DefaultInitAllocator<U>;
        };
        DefaultInitAllocator() noexcept = default;
        template <class U>
        DefaultInitAllocator(const DefaultInitAllocator<U> &) noexcept {}
        template <class U>
        void construct(U *p) noexcept { ::new (static_cast<void *>(p)) U; }
        template <class U, class... Args>
        void construct(U *p, Args &&...args) { ::new (static_cast<void *>(p)) U(std::forward<Args>(args)...); }
    };
    using QwordChunk = std::vector<__m128i, DefaultInitAllocator<__m128i>>;

    class GsThread
    {
    public:
        enum Op : uint32_t
        {
            kGif = 1,
            kVsync = 2,
            kReset = 3,
            kReadFifo = 4,
            kCgSubmit = 5,
            kHdCg = 6,
        };

        GsThread()
        {
            m_open.reserve(kChunkQwords + 64u);
            m_thread = std::thread([this] { loop(); });
        }

        // Main thread only, after the producer has joined. No further push/flush calls.
        void stop()
        {
            if (!m_thread.joinable())
                return;
            publish();
            {
                std::lock_guard lock(m_mutex);
                m_stopping = true;
            }
            m_work.notify_one();
            // DXGI teardown can send synchronous window messages. Service those while
            // waiting, without dispatching new posted input or running another frame.
            const HANDLE handle = m_thread.native_handle();
            while (MsgWaitForMultipleObjects(1, &handle, FALSE, INFINITE, QS_SENDMESSAGE) == WAIT_OBJECT_0 + 1)
            {
                MSG message;
                PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
            }
            m_thread.join();
        }

        // Game thread.
        void push(Op op, uint32_t arg, const void *payload, uint32_t bytes)
        {
            const size_t qwords = (static_cast<size_t>(bytes) + 15u) / 16u;
            const size_t at = m_open.size();
            m_open.resize(at + 1u + qwords);
            const uint32_t header[4] = {static_cast<uint32_t>(op), arg, bytes, _mm_getcsr()};
            std::memcpy(&m_open[at], header, sizeof(header));
            if (bytes)
            {
                std::memcpy(&m_open[at + 1u], payload, bytes);
                if ((bytes & 15u) != 0u) // the rest of the last qword, zero as before
                    std::memset(reinterpret_cast<uint8_t *>(&m_open[at + 1u]) + bytes, 0, 16u - (bytes & 15u));
            }
            if (op == kGif && fieldOffsetFixOn())
                fixFieldOffsets(arg, reinterpret_cast<uint8_t *>(&m_open[at + 1u]), bytes);
            if (op == kVsync)
            {
                m_vsyncsInFlight.fetch_add(1u, std::memory_order_relaxed);
                publish();
                std::unique_lock lock(m_mutex);
                m_done.wait(lock, [this] { return m_vsyncsInFlight.load(std::memory_order_relaxed) <= kMaxFieldsInFlight; });
            }
            else if (m_open.size() >= kChunkQwords)
                publish();
        }

        // Game thread: runs everything queued so far, then returns.
        void flush()
        {
            publish();
            std::unique_lock lock(m_mutex);
            m_done.wait(lock, [this] { return m_queue.empty() && !m_busy; });
        }

    private:
        static constexpr size_t kChunkQwords = 64u * 1024u / 16u;
        static constexpr size_t kMaxChunks = 64u;
        static constexpr uint32_t kMaxFieldsInFlight = 2u;

        void publish()
        {
            if (m_open.empty())
                return;
            QwordChunk next;
            {
                std::unique_lock lock(m_mutex);
                m_done.wait(lock, [this] { return m_queue.size() < kMaxChunks; });
                m_queue.push_back(std::move(m_open));
                if (!m_free.empty())
                {
                    next = std::move(m_free.back());
                    m_free.pop_back();
                }
            }
            m_work.notify_one();
            next.clear();
            m_open = std::move(next);
            m_open.reserve(kChunkQwords + 64u);
        }

        void loop()
        {
            for (;;)
            {
                QwordChunk chunk;
                {
                    std::unique_lock lock(m_mutex);
                    m_work.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
                    if (m_queue.empty())
                        break;
                    chunk = std::move(m_queue.front());
                    m_queue.pop_front();
                    m_busy = true;
                }
                run(chunk);
                {
                    std::lock_guard lock(m_mutex);
                    m_busy = !m_queue.empty();
                    if (m_free.size() < 8u)
                        m_free.push_back(std::move(chunk));
                }
                m_done.notify_all();
            }
            if (g_open)
            {
                g_api->close(); // joins the HD texture loader before DLL static destruction
                g_open = false;
            }
        }

        void run(const QwordChunk &chunk)
        {
            const bool open = ensureOpen();
            size_t at = 0u;
            while (at < chunk.size())
            {
                uint32_t header[4];
                std::memcpy(header, &chunk[at], sizeof(header));
                const uint8_t *payload = reinterpret_cast<const uint8_t *>(&chunk[at + 1u]);
                at += 1u + (static_cast<size_t>(header[2]) + 15u) / 16u;
                if (header[3] != m_mxcsr)
                    _mm_setcsr(m_mxcsr = header[3]);
                if (!open)
                {
                    if (header[0] == kVsync)
                        m_vsyncsInFlight.fetch_sub(1u, std::memory_order_relaxed);
                    continue;
                }
                switch (header[0])
                {
                case kGif:
                    gifDirect(static_cast<int>(header[1]), payload, header[2]);
                    break;
                case kVsync:
                {
                    Pcsx2GsPrivRegs priv;
                    std::memcpy(&priv, payload, sizeof(priv));
                    vsyncDirect(priv);
                    {
                        std::lock_guard lock(m_mutex);
                        m_vsyncsInFlight.fetch_sub(1u, std::memory_order_relaxed);
                    }
                    m_done.notify_all();
                    break;
                }
                case kReset:
                    resetDirect();
                    break;
                case kHdCg:
                    if(auto fn=g_hdCg.load(std::memory_order_acquire)) fn(header[2]?payload:nullptr,header[2]);
                    break;
                case kCgSubmit:
                {
                    uint64_t queued = 0;
                    std::memcpy(&queued, payload, sizeof(queued));
                    cgtrace::submitted(header[1] & 0x7fffffffu, queued, (header[1] >> 31) != 0);
                    break;
                }
                case kReadFifo:
                {
                    uint8_t *dst = nullptr;
                    std::memcpy(&dst, payload, sizeof(dst));
                    readFifoDirect(dst, header[1]);
                    break;
                }
                default:
                    break;
                }
            }
        }

        QwordChunk m_open; // game thread
        std::mutex m_mutex;
        std::condition_variable m_work, m_done;
        std::deque<QwordChunk> m_queue;
        std::vector<QwordChunk> m_free;
        bool m_busy = false;
        bool m_stopping = false; // m_mutex; set only after the producer has stopped
        std::atomic<uint32_t> m_vsyncsInFlight{0u};
        uint32_t m_mxcsr = 0u; // GS thread
        std::thread m_thread;
    };

    GsThread *g_worker = nullptr;

    GsThread *gsThread()
    {
        static GsThread *const thread = []() -> GsThread *
        {
            const char *v = std::getenv("DOD_GS_THREAD");
            if (v && *v == '0')
            {
                std::fprintf(stderr, "[pcsx2-gs] GS on the game thread (DOD_GS_THREAD=0)\n");
                return nullptr;
            }
            return g_worker = new GsThread(); // explicitly stopped after the game thread joins
        }();
        return thread;
    }

    void shutdown()
    {
        if (!g_active.exchange(false))
            return;
        if (g_worker)
        {
            g_worker->stop();
            delete g_worker;
            g_worker = nullptr;
        }
        else if (g_open)
        {
            g_api->close();
            g_open = false;
        }
        std::fprintf(stderr, "[pcsx2-gs] shutdown complete: renderer and texture worker closed\n");
    }

    void gif(int path, const uint8_t *data, uint32_t sizeBytes)
    {
        if (!active() || rawSuppressed() || !data || sizeBytes < 16u || !onGsThread())
            return;
        if (GsThread *thread = gsThread())
            thread->push(GsThread::kGif, static_cast<uint32_t>(path), data, sizeBytes & ~15u);
        else if (ensureOpen())
            gifDirect(path, data, sizeBytes);
    }

    bool hdCgSupported() { return active() && g_hdCg.load(std::memory_order_acquire)!=nullptr; }
    const wchar_t* hdCgDefaultRoot() { static const std::wstring path=(exeDirectory()/"movies_hd").wstring(); return path.c_str(); }
    void hdCgFrame(const void* data, uint32_t bytes)
    {
        if (!hdCgSupported() || !onGsThread()) return;
        if (GsThread* thread=gsThread()) thread->push(GsThread::kHdCg,0,data,bytes);
        else if (ensureOpen()) g_hdCg.load(std::memory_order_acquire)(data,bytes);
    }

    // Called only by the guarded story-player hook, in game-thread order.
    void cgSubmit(uint32_t frame, bool eligible)
    {
        if (!active() || !cgtrace::cadenceOn() || !onGsThread()) return;
        const uint64_t queued = g_queuedVsyncs.load(std::memory_order_relaxed);
        if (GsThread* thread = gsThread())
            thread->push(GsThread::kCgSubmit, (frame & 0x7fffffffu) | (eligible ? 0x80000000u : 0u),
                         &queued, sizeof(queued));
        else
            cgtrace::submitted(frame, queued, eligible);
    }

    void vsync(const GSRegisters &regs)
    {
        if (!active() || !onGsThread())
            return;
        g_queuedVsyncs.fetch_add(1u, std::memory_order_relaxed);
        Pcsx2GsPrivRegs priv{};
        const uint64_t regset1[15] = {regs.pmode, regs.smode1, regs.smode2, regs.srfsh, regs.synch1,
                                      regs.synch2, regs.syncv, regs.dispfb1, regs.display1, regs.dispfb2,
                                      regs.display2, regs.extbuf, regs.extdata, regs.extwrite, regs.bgcolor};
        std::memcpy(priv.regset1, regset1, sizeof(regset1));
        priv.csr = regs.csr.load(std::memory_order_acquire);
        // [DoD] diagnostics: DOD_GS_FIELD_INVERT=1 gives PCSX2's GS the other field (CSR.FIELD flipped) than the game
        // sees, to check which field order lines up with the half-line offset DoD renders alternate fields with.
        static const bool fieldInvert = []() { const char *v = std::getenv("DOD_GS_FIELD_INVERT"); return v && v[0] == '1'; }();
        if (fieldInvert)
            priv.csr ^= 0x2000ull;
        priv.imr = regs.imr;
        priv.busdir = regs.busdir;
        priv.siglblid = regs.siglblid;
        if (GsThread *thread = gsThread())
            thread->push(GsThread::kVsync, 0u, &priv, sizeof(priv));
        else if (ensureOpen())
            vsyncDirect(priv);
    }

    void reset()
    {
        if (!active() || !onGsThread())
            return;
        if (GsThread *thread = gsThread())
            thread->push(GsThread::kReset, 0u, nullptr, 0u);
        else
            resetDirect();
    }

    void readFifo(uint8_t *dst, uint32_t sizeBytes)
    {
        if (!active() || !dst || sizeBytes == 0u || !onGsThread())
            return;
        if (GsThread *thread = gsThread())
        {
            thread->push(GsThread::kReadFifo, sizeBytes, &dst, sizeof(dst));
            thread->flush();
        }
        else if (ensureOpen())
            readFifoDirect(dst, sizeBytes);
    }

    void requestSnapshot(const char *path)
    {
        if (!path)
            return;
        std::lock_guard<std::mutex> lock(g_snapshotMutex);
        g_snapshotPath = path;
    }

    uint64_t presents()
    {
        return g_presents.load(std::memory_order_relaxed);
    }

    void requestLineCapture(const char *dir, bool light, bool ab, const char *gameInfo)
    {
        if (!dir || !*dir)
            return;
        const Pcsx2GsDodCaptureFn capture = g_dodCapture.load(std::memory_order_acquire);
        const DumpNowFn dumpNow = g_dumpNow.load(std::memory_order_acquire);
        {
            std::lock_guard<std::mutex> lock(g_snapshotMutex);
            g_burstDir = dir;
            g_burstFirst = g_burstLast = 0u;
            g_burstShots = light ? 1u : kBurstShots;
            g_burstExt = capture ? ".png" : ".bmp";
        }
        const std::string draws = std::string(dir) + "/draws";
        if (capture)
            capture(draws.c_str(), light && !ab ? 1u : 2u,
                    (light ? PCSX2GS_DOD_CAPTURE_LIGHT : 0u) | (ab ? PCSX2GS_DOD_CAPTURE_AB : 0u));
        else if (dumpNow)
            dumpNow(draws.c_str(), 2u);
        if (FILE *f = std::fopen((std::string(dir) + "/info.txt").c_str(), "w"))
        {
            const char *edges = std::getenv("DOD_PARTICLE_EDGES");
            std::fprintf(f, "%sscene prefetched 0x%X\nfields presented %llu\nscale %d\noptions %s\ncapture %s\n"
                            "particle edge fix %s\ndraw dump %s\n",
                         gameInfo ? gameInfo : "", g_lastScene.load(), static_cast<unsigned long long>(g_presents.load()),
                         g_scale, g_options.c_str(),
                         ab ? "A/B (1 snapshot; frame output of two frames: the first with the particle fix, the second without)"
                         : light ? "light (1 snapshot, frame output)" : "full",
                         edges && edges[0] == '0' ? "OFF (DOD_PARTICLE_EDGES=0)" : "on",
                         capture ? "draws\\: dod_draws_summary.txt (each frame's draws by effect kind), dod_draws.tsv (one line per draw), frame PNGs"
                         : dumpNow ? "draws\\ (old pcsx2gs.dll: per-draw info, BMP; no draw table)"
                                   : "no (pcsx2gs.dll lacks pcsx2gs_dod_capture)");
            // The GPU the GS picked (GSDevice11 writes logs\gpu.txt at the start)
            if (FILE *gpu = std::fopen("logs\\gpu.txt", "r"))
            {
                std::fprintf(f, "gpu.txt:\n");
                char line[512];
                while (std::fgets(line, sizeof(line), gpu))
                    std::fprintf(f, "  %s", line);
                std::fclose(gpu);
            }
            std::fclose(f);
        }
    }

    void prefetchScene(uint32_t scene)
    {
        g_lastScene.store(scene, std::memory_order_relaxed);
        if (const PrefetchSceneFn fn = g_prefetchScene.load(std::memory_order_acquire))
            fn(scene);
    }

    uint64_t queuedVsyncs()
    {
        return g_queuedVsyncs.load(std::memory_order_relaxed);
    }

    void setMenu(void (*draw)(const ::Pcsx2GsUi *ui, void *user), void *user)
    {
        if (g_api && g_api->set_menu)
            g_api->set_menu(draw, user);
    }

    // The cursor from Win32 in the GS child window's client area, scaled to the size given to the GS (the image's
    // pixels): the same numbers ImGui lays out with, whatever the DPI awareness, Windows scaling, window size or
    // fullscreen mode. GetAsyncKeyState reads physical buttons, so swapped mouse buttons are swapped back.
    void menuInput(uint32_t keys, uint32_t pad, float wheel)
    {
        if (!g_api || !g_api->menu_input || !g_child)
            return;
        Pcsx2GsMenuInput input{-FLT_MAX, -FLT_MAX, 0u, wheel, keys, pad};
        POINT cursor{};
        RECT client{};
        const HWND foreground = GetForegroundWindow();
        if ((foreground == g_parent || foreground == g_child) && GetCursorPos(&cursor) && ScreenToClient(g_child, &cursor) &&
            GetClientRect(g_child, &client) && client.right > 0 && client.bottom > 0 && cursor.x >= 0 && cursor.y >= 0 &&
            cursor.x < client.right && cursor.y < client.bottom)
        {
            const float sx = static_cast<float>(g_clientWidth.load(std::memory_order_relaxed)) / static_cast<float>(client.right);
            const float sy = static_cast<float>(g_clientHeight.load(std::memory_order_relaxed)) / static_cast<float>(client.bottom);
            input.mouse_x = (static_cast<float>(cursor.x) + 0.5f) * sx;
            input.mouse_y = (static_cast<float>(cursor.y) + 0.5f) * sy;
            const bool swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
            const auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
            if (down(swapped ? VK_RBUTTON : VK_LBUTTON))
                input.mouse_buttons |= 1u;
            if (down(swapped ? VK_LBUTTON : VK_RBUTTON))
                input.mouse_buttons |= 2u;
            if (down(VK_MBUTTON))
                input.mouse_buttons |= 4u;
        }
        g_api->menu_input(&input);
    }
}

#else // not Windows: the CPU raster only

namespace dodpcsx2
{
    void configure(const char *, int, int, const char *) {}
    bool attach(void *) { return false; }
    void hostTick() {}
    void shutdown() {}
    bool active() { return false; }
    bool hdCgSupported() { return false; }
    const wchar_t* hdCgDefaultRoot() { return L""; }
    void hdCgFrame(const void*, uint32_t) {}
    void gif(int, const uint8_t *, uint32_t) {}
    void vsync(const GSRegisters &) {}
    void reset() {}
    void readFifo(uint8_t *, uint32_t) {}
    void requestSnapshot(const char *) {}
    void requestLineCapture(const char *, bool, bool, const char *) {}
    uint64_t presents() { return 0u; }
    void prefetchScene(uint32_t) {}
    uint64_t queuedVsyncs() { return 0u; }
    void cgSubmit(uint32_t, bool) {}
    void setMenu(void (*)(const ::Pcsx2GsUi *, void *), void *) {}
    void menuInput(uint32_t, uint32_t, float) {}
}

#endif
