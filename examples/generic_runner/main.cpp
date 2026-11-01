// Generic host entry point for a PS2Recomp-Fixer port (replaces ps2xRuntime/src/main.cpp).
//
// Copy this file to third_party/src/PS2Recomp/ps2xRuntime/src/runner/main.cpp (that folder is git-ignored upstream, add
// it with `git add -f`), next to the generated function files and register_functions.cpp of YOUR game. It knows no game:
// everything game-specific comes from game.ini next to the executable (flat "key = value" lines, '#' or ';' comments);
// command-line flags override it:  game.exe --iso D:\game.iso --fullscreen 1
//
//   elf          boot ELF (the CD root for files beside it)                 default: game/boot.elf
//   iso          disc image the game reads sectors from                      required
//   title        window title                                                default: PS2Recomp game
//   window       window size, e.g. 1280x960                                  default: raylib's
//   fullscreen   0 / 1 (borderless window)                                   default: 0
//   renderer     pcsx2 (PCSX2's hardware GS, pcsx2gs.dll) or cpu             default: pcsx2
//   gs_scale     internal resolution 1..8, or auto (follows the display)     default: auto
//   gs_options   PCSX2 image options "fxaa=1;texture_filtering=2;..." (see pcsx2gs_api.h and docs/GAME_PORTING.md)
//   vu1          microvu (PCSX2 microVU through kzvu) | interp | builtin     default: microvu
//   volume       master volume 0..100                                        default: 100
//   rumble       0 / 1, gamepad rumble                                       default: 1
//   gparam       guest address of the game's libgraph GParam record          default: unknown (see GS.cpp)
//
// Per-game GS settings (PCSX2's GameIndex.yaml fixes such as half_pixel_offset / native_scaling / blending) go in
// gs_options; nothing is applied by default.
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "../lib/gs/dod_pcsx2_gs.h"

#include <raylib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>

bool dodKzvuInstall(PS2Runtime &runtime, const char *mode); // lib/vu/dod_kzvu.cpp
void dodConfigureGamepad(bool enabled);
void dodRequestPadRumble(int port, uint8_t large, uint8_t small);
bool dodPeekHostPadInput(uint8_t *data, size_t size); // lib/ps2_pad.cpp
namespace ps2x::iop
{
    extern void (*g_dodPadRumbleOutput)(int port, uint8_t large, uint8_t small);
    extern bool (*g_dodPadStateProvider)(int port, uint8_t *data32); // ps2xIOP iop_sio2.cpp
}
namespace ps2_stubs
{
    void setGsGParamAddress(uint32_t address); // Kernel/Stubs/GS.cpp
}

namespace
{
    namespace fs = std::filesystem;

    std::string trim(const std::string &s)
    {
        const size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos)
            return {};
        const size_t e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    }

    std::map<std::string, std::string> readIni(const fs::path &path)
    {
        std::map<std::string, std::string> values;
        std::ifstream in(path);
        std::string line;
        for (bool first = true; std::getline(in, line); first = false)
        {
            if (first && line.rfind("\xEF\xBB\xBF", 0) == 0)
                line.erase(0, 3); // UTF-8 BOM
            const std::string t = trim(line);
            if (t.empty() || t[0] == '#' || t[0] == ';')
                continue;
            const size_t eq = t.find('=');
            if (eq == std::string::npos || eq == 0)
                continue;
            values[trim(t.substr(0, eq))] = trim(t.substr(eq + 1));
        }
        return values;
    }

    fs::path executableDirectory(const char *argv0)
    {
        std::error_code ec;
        const fs::path p = fs::absolute(fs::path(argv0), ec);
        return ec ? fs::current_path() : p.parent_path();
    }

    fs::path resolveFrom(const fs::path &base, const std::string &value)
    {
        if (value.empty())
            return {};
        const fs::path p(value);
        return (p.is_absolute() ? p : base / p).lexically_normal();
    }

    bool parseWindowSize(const std::string &text, int &width, int &height)
    {
        int w = 0, h = 0;
        char x = 0;
        if (std::sscanf(text.c_str(), "%d%c%d", &w, &x, &h) != 3 || (x != 'x' && x != 'X') || w < 320 || h < 240)
            return false;
        width = w;
        height = h;
        return true;
    }
}

int main(int argc, char *argv[])
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); // logs must survive the process being killed
    std::cout << std::unitbuf;
    try
    {
        const fs::path exeDir = executableDirectory(argv[0]);
        std::map<std::string, std::string> ini = readIni(exeDir / "game.ini");
        for (int i = 1; i + 1 < argc; i += 2)
        {
            const std::string key = argv[i];
            if (key.rfind("--", 0) != 0)
            {
                std::cerr << "unknown argument: " << key << std::endl;
                return 2;
            }
            ini[key.substr(2)] = argv[i + 1];
        }
        const auto get = [&](const char *key, const char *fallback) { return ini.count(key) ? ini[key] : std::string(fallback); };

        const fs::path elf = resolveFrom(exeDir, get("elf", "game/boot.elf"));
        const fs::path iso = resolveFrom(exeDir, get("iso", ""));
        if (!fs::exists(elf))
        {
            std::cerr << "[runner] boot ELF not found: " << elf.string() << " (set elf in game.ini)" << std::endl;
            return 1;
        }
        if (iso.empty() || !fs::exists(iso))
        {
            std::cerr << "[runner] disc image not found: " << (iso.empty() ? std::string("(not set)") : iso.string())
                      << " (set iso in game.ini or pass --iso)" << std::endl;
            return 1;
        }

        // The game reads its data sectors from the disc image; the boot files beside the ELF stay the CD root.
        PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
        paths.cdImage = iso;
        PS2Runtime::setIoPaths(paths);
        if (ini.count("gparam"))
            ps2_stubs::setGsGParamAddress(static_cast<uint32_t>(std::strtoul(ini["gparam"].c_str(), nullptr, 0)));

        dodConfigureGamepad(get("rumble", "1") != "0");
        ps2x::iop::g_dodPadRumbleOutput = dodRequestPadRumble;
        // The game's own pad stack runs on the emulated IOP; its SIO2 sees a DualShock 2 in port 1 driven by the
        // keyboard / host gamepad mapping.
        ps2x::iop::g_dodPadStateProvider = [](int port, uint8_t *data32) { return port == 0 && dodPeekHostPadInput(data32, 32u); };

        PS2Runtime runtime;
        const std::string title = get("title", "PS2Recomp game");
        if (!runtime.initialize(title.c_str()))
        {
            std::cerr << "[runner] failed to initialize the runtime" << std::endl;
            return 1;
        }
        if (IsAudioDeviceReady())
            SetMasterVolume(static_cast<float>(std::clamp(std::atoi(get("volume", "100").c_str()), 0, 100)) / 100.0f);
        dodKzvuInstall(runtime, get("vu1", "microvu").c_str());
        SetExitKey(KEY_NULL); // Esc is a pad button, not "close the window"

        int width = 0, height = 0;
        if (ini.count("window"))
        {
            if (parseWindowSize(ini["window"], width, height))
                SetWindowSize(width, height);
            else
                std::cerr << "[runner] invalid window size, keeping the default: " << ini["window"] << std::endl;
        }
        const bool fullscreen = get("fullscreen", "0") == "1";
        if (fullscreen)
            ToggleBorderlessWindowed();
        ClearWindowState(FLAG_VSYNC_HINT);
        SetTargetFPS(0); // one deadline pacer in the host loop

        // gs_scale = auto: the smallest multiple of the PS2's 640x448 at least as tall as the 4:3 picture the window
        // (or the monitor, when fullscreen) shows, so the 3D image is never upscaled.
        int scale = get("gs_scale", "auto") != "auto" ? std::atoi(get("gs_scale", "0").c_str()) : 0;
        if (scale <= 0)
        {
            int outW = width > 0 ? width : GetScreenWidth();
            int outH = height > 0 ? height : GetScreenHeight();
            if (fullscreen)
            {
                outW = GetMonitorWidth(GetCurrentMonitor());
                outH = GetMonitorHeight(GetCurrentMonitor());
            }
            const int pictureH = std::max(1, std::min(outH, outW * 3 / 4));
            scale = std::clamp(static_cast<int>(std::ceil(pictureH / 448.0 - 1e-9)), 1, 8);
            std::cout << "[runner] render scale " << scale << " for a " << outW << "x" << outH << " display" << std::endl;
        }
        dodpcsx2::configure(get("renderer", "pcsx2").c_str(), scale, 0, get("gs_options", "").c_str());

        if (!runtime.loadELF(elf.string()))
        {
            std::cerr << "[runner] failed to load ELF: " << elf.string() << std::endl;
            return 1;
        }
        runtime.run();
        std::cout.flush();
        std::cerr.flush();
        std::_Exit(0);
    }
    catch (const std::exception &e)
    {
        std::cerr << "[runner] fatal exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[runner] fatal exception: unknown" << std::endl;
    }
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(1);
}
