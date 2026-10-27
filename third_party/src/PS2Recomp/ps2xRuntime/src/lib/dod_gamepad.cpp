#include "dod_gamepad.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#define SDL_MAIN_HANDLED
#include "../../../../SDL2-2.30.9/include/SDL.h"

namespace {
// Reuse the shipped SDL 2.30.9. Loading is optional so a missing dependency
// cannot prevent keyboard input or the existing raylib fallback from booting.
struct Api {
    HMODULE dll = nullptr;
#define API(name) decltype(&SDL_##name) name = nullptr
    API(SetHint); API(InitSubSystem); API(QuitSubSystem); API(GetVersion);
    API(GetError); API(PumpEvents); API(NumJoysticks); API(IsGameController);
    API(GameControllerOpen); API(GameControllerClose); API(GameControllerUpdate);
    API(GameControllerGetAttached); API(GameControllerGetButton); API(GameControllerGetAxis);
    API(GameControllerName); API(GameControllerRumble); API(GameControllerHasRumble);
    API(GameControllerEventState); API(JoystickEventState);
#undef API
} api;
SDL_GameController *pad = nullptr;
bool attempted = false, ready = false, enabled = true, rumbleAvailable = false;
bool failureLogged = false;
uint64_t nextScan = 0, lastOutput = 0;
uint16_t lastMotors = 0;
std::atomic<uint64_t> request{0}; // monotonic milliseconds : large : small
uint64_t nowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
bool initialize() {
    if (attempted) return ready;
    attempted = true;
    wchar_t filename[32768];
    const DWORD n = GetModuleFileNameW(nullptr, filename, 32768);
    if (!n || n >= 32768) return false;
    const std::wstring exe(filename, n);
    const auto slash = exe.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;
    api.dll = LoadLibraryW((exe.substr(0, slash + 1) + L"SDL2.dll").c_str());
    if (!api.dll) {
        std::fprintf(stderr, "[gamepad] SDL2.dll unavailable; raylib/keyboard fallback\n");
        return false;
    }
#define LOAD(name) api.name = reinterpret_cast<decltype(api.name)>(GetProcAddress(api.dll, "SDL_" #name)); if (!api.name) return false
    LOAD(SetHint); LOAD(InitSubSystem); LOAD(QuitSubSystem); LOAD(GetVersion);
    LOAD(GetError); LOAD(PumpEvents); LOAD(NumJoysticks); LOAD(IsGameController);
    LOAD(GameControllerOpen); LOAD(GameControllerClose); LOAD(GameControllerUpdate);
    LOAD(GameControllerGetAttached); LOAD(GameControllerGetButton); LOAD(GameControllerGetAxis);
    LOAD(GameControllerName); LOAD(GameControllerRumble); LOAD(GameControllerHasRumble);
    LOAD(GameControllerEventState); LOAD(JoystickEventState);
#undef LOAD
    SDL_version version{}; api.GetVersion(&version);
    if (version.major != 2 || SDL_VERSIONNUM(version.major, version.minor, version.patch) < SDL_VERSIONNUM(2, 0, 18)) {
        std::fprintf(stderr, "[gamepad] SDL 2.0.18+ required; using fallback\n");
        return false;
    }
    api.SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    api.SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "1");
    api.SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
    // Sony Bluetooth needs enhanced reports for motor output (also used by Oni3).
    api.SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, enabled ? "1" : "0");
    api.SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, enabled ? "1" : "0");
    // SDL has no window here (raylib owns it). Focus is gated explicitly below.
    api.SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (api.InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "[gamepad] SDL init failed: %s\n", api.GetError());
        return false;
    }
    api.GameControllerEventState(SDL_IGNORE);
    api.JoystickEventState(SDL_IGNORE);
    ready = true;
    std::fprintf(stderr, "[gamepad] SDL %u.%u.%u ready; rumble=%d; controllers=%d\n",
                 version.major, version.minor, version.patch, enabled, api.NumJoysticks());
    return true;
}
void closePad() {
    if (pad) {
        if (rumbleAvailable) api.GameControllerRumble(pad, 0, 0, 0);
        api.GameControllerClose(pad);
        pad = nullptr;
    }
    request.store(0, std::memory_order_release);
    lastMotors = 0; lastOutput = 0; rumbleAvailable = false;
}
void motors(bool focused, uint64_t now) {
    const uint64_t r = request.load(std::memory_order_acquire);
    const uint64_t published = r >> 16;
    const uint64_t age = now >= published ? now - published : 0;
    const uint16_t value = enabled && focused && r && age < 750 ? static_cast<uint16_t>(r) : 0;
    if (!focused) request.store(0, std::memory_order_release);
    if (!rumbleAvailable || (value == lastMotors && (!value || now - lastOutput < 100))) return;
    // Large PS2 motor: variable low frequency. Small motor: binary high frequency.
    const int result = api.GameControllerRumble(pad, static_cast<Uint16>((value >> 8) * 257u),
                                               (value & 1u) ? 65535u : 0u, value ? 250u : 0u);
    if (result != 0 && !failureLogged) {
        failureLogged = true;
        std::fprintf(stderr, "[gamepad] rumble failed: %s\n", api.GetError());
    }
    lastMotors = value; lastOutput = now;
}
}

void dodConfigureGamepad(bool rumbleEnabled) { enabled = rumbleEnabled; }
void dodRequestPadRumble(int port, uint8_t large, uint8_t small) {
    if (port != 0) return;
    request.store((nowMs() << 16) | (uint64_t(large) << 8) | (small & 1u), std::memory_order_release);
    static const bool trace = std::getenv("DOD_PAD_TRACE") != nullptr;
    static unsigned logged = 0;
    static int previous = -1;
    const int current = (int(large) << 8) | (small & 1u);
    if (trace && current != previous && logged < 128) {
        ++logged; previous = current;
        std::fprintf(stderr, "[gamepad] guest motors large=%u small=%u\n", large, small & 1u);
    }
}
bool dodGamepadBackendReady() { return ready; }
bool dodPollGamepad(uint8_t *data, bool focused) {
    if (!initialize()) return false;
    api.PumpEvents();
    api.GameControllerUpdate();
    const uint64_t now = nowMs();
    if (pad && !api.GameControllerGetAttached(pad)) {
        std::fprintf(stderr, "[gamepad] disconnected\n");
        closePad(); nextScan = 0;
    }
    if (!pad && now >= nextScan) {
        nextScan = now + 500;
        for (int i = 0, count = api.NumJoysticks(); i < count; ++i) {
            if (!api.IsGameController(i)) continue;
            pad = api.GameControllerOpen(i);
            if (!pad) continue;
            request.store(0, std::memory_order_release); // never replay old-device vibration
            failureLogged = false;
            rumbleAvailable = api.GameControllerHasRumble(pad) == SDL_TRUE;
            std::fprintf(stderr, "[gamepad] connected: %s; rumble=%d\n", api.GameControllerName(pad), rumbleAvailable);
            break;
        }
    }
    if (!pad) return false;
    motors(focused, now);
    if (!focused) return true; // retain controller priority, publish neutral input
    uint16_t buttons = 0xFFFFu;
    const struct { SDL_GameControllerButton button; uint16_t mask; } bindings[] = {
        {SDL_CONTROLLER_BUTTON_A,0x4000}, {SDL_CONTROLLER_BUTTON_B,0x2000},
        {SDL_CONTROLLER_BUTTON_X,0x8000}, {SDL_CONTROLLER_BUTTON_Y,0x1000},
        {SDL_CONTROLLER_BUTTON_BACK,1}, {SDL_CONTROLLER_BUTTON_START,8},
        {SDL_CONTROLLER_BUTTON_LEFTSTICK,2}, {SDL_CONTROLLER_BUTTON_RIGHTSTICK,4},
        {SDL_CONTROLLER_BUTTON_LEFTSHOULDER,0x400}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,0x800},
        {SDL_CONTROLLER_BUTTON_DPAD_UP,0x10}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN,0x40},
        {SDL_CONTROLLER_BUTTON_DPAD_LEFT,0x80}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT,0x20}
    };
    for (const auto &b : bindings) if (api.GameControllerGetButton(pad, b.button)) buttons &= ~b.mask;
    if (api.GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8192) buttons &= ~0x100u;
    if (api.GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8192) buttons &= ~0x200u;
    data[2] = static_cast<uint8_t>(buttons); data[3] = static_cast<uint8_t>(buttons >> 8);
    const SDL_GameControllerAxis axes[] = {SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY,
                                          SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY};
    for (int i = 0; i < 4; ++i) {
        const int value = api.GameControllerGetAxis(pad, axes[i]);
        data[4+i] = static_cast<uint8_t>((value + 32768) >> 8);
    }
    return true;
}
void dodShutdownGamepad() {
    closePad();
    if (ready) api.QuitSubSystem(SDL_INIT_GAMECONTROLLER);
    ready = false;
    if (api.dll) { FreeLibrary(api.dll); api.dll = nullptr; }
}
#else
void dodConfigureGamepad(bool) {}
void dodRequestPadRumble(int, uint8_t, uint8_t) {}
bool dodPollGamepad(uint8_t *, bool) { return false; }
bool dodGamepadBackendReady() { return false; }
void dodShutdownGamepad() {}
#endif
