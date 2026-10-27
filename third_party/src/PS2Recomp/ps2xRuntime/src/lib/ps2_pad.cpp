#include "runtime/ps2_pad.h"
#include "ps2_host_backend.h"
#include "dod_input_settings.h"
#include "dod_gamepad.h"
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <vector>

#if defined(_WIN32)
// [DoD] Only this from user32 (windows.h clashes with raylib's names).
extern "C" __declspec(dllimport) int __stdcall PostMessageW(void *window, unsigned int message, unsigned long long wparam,
                                                              long long lparam);
#endif

// [DoD] Host time (GetTime) when the game started a new game, set by the runner's 0x360810 hook in automated tests;
// -1 before (DOD_TEST_AUTOSTART, below).
std::atomic<double> g_dodTestNewGameAt{-1.0};
std::atomic<uint8_t *> g_dodTestRdram{nullptr}; // [DoD] set by the runner: DOD_PAD_SCRIPT_FRAMES reads the game's counter

namespace
{
    constexpr uint8_t kPadAnalogMarker = 0x73;
    constexpr uint8_t kPadStickCenter = 0x80;

    constexpr uint16_t PAD_LEFT = 0x0080u;
    constexpr uint16_t PAD_DOWN = 0x0040u;
    constexpr uint16_t PAD_RIGHT = 0x0020u;
    constexpr uint16_t PAD_UP = 0x0010u;
    constexpr uint16_t PAD_START = 0x0008u;
    constexpr uint16_t PAD_R3 = 0x0004u;
    constexpr uint16_t PAD_L3 = 0x0002u;
    constexpr uint16_t PAD_SELECT = 0x0001u;
    constexpr uint16_t PAD_SQUARE = 0x8000u;
    constexpr uint16_t PAD_CROSS = 0x4000u;
    constexpr uint16_t PAD_CIRCLE = 0x2000u;
    constexpr uint16_t PAD_TRIANGLE = 0x1000u;
    constexpr uint16_t PAD_R1 = 0x0800u;
    constexpr uint16_t PAD_L1 = 0x0400u;
    constexpr uint16_t PAD_R2 = 0x0200u;
    constexpr uint16_t PAD_L2 = 0x0100u;

    dod_input::KeyboardBindings g_keyboardBindings = dod_input::defaults();

    struct HostPadSnapshot
    {
        uint8_t data[32] = {0x01, kPadAnalogMarker, 0xFF, 0xFF,
                            kPadStickCenter, kPadStickCenter, kPadStickCenter, kPadStickCenter};
        double sampledAt = 0.0;
        int gamepad = 0;
    };

    std::mutex g_hostPadMutex;
    HostPadSnapshot g_hostPadSnapshot;
    uint16_t g_pendingHostPresses = 0u;
    uint16_t g_previousHostHeld = 0u;
}

void dodConfigureKeyboardInput(const dod_input::KeyboardBindings &bindings)
{
    g_keyboardBindings = bindings;
}

namespace
{
    dod_input::StickBindings g_keyboardSticks{};
}

void dodConfigureKeyboardSticks(const dod_input::StickBindings &sticks)
{
    g_keyboardSticks = sticks;
}

// [DoD] Called on the window thread immediately after EndDrawing polls events.
// Capture queued presses as well as held keys: a down/up pair in one event
// poll leaves IsKeyDown false, but its press remains in GetKeyPressed's queue.
// Publish held state and retain presses until a normal SIO2 input poll samples
// them. The IOP thread must not read raylib globals.
void dodPollHostPadInput()
{
    HostPadSnapshot snapshot;
    uint8_t *data = snapshot.data;

    uint16_t btns = 0xFFFFu;
    constexpr int kGamepad = 0;
    const bool sdlGamepad = dodPollGamepad(data, IsWindowFocused());
    const bool useGamepad = sdlGamepad || (!dodGamepadBackendReady() && IsGamepadAvailable(kGamepad));
    auto clearBit = [&btns](uint16_t mask)
    { btns &= ~mask; };

    if (sdlGamepad)
    {
        btns = static_cast<uint16_t>(data[2] | (uint16_t(data[3]) << 8));
    }
    else if (useGamepad)
    {
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_UP))
            clearBit(PAD_UP);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_DOWN))
            clearBit(PAD_DOWN);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_LEFT))
            clearBit(PAD_LEFT);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_RIGHT))
            clearBit(PAD_RIGHT);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN))
            clearBit(PAD_CROSS);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT))
            clearBit(PAD_CIRCLE);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_LEFT))
            clearBit(PAD_SQUARE);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_UP))
            clearBit(PAD_TRIANGLE);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_1))
            clearBit(PAD_L1);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_1))
            clearBit(PAD_R1);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_2))
            clearBit(PAD_L2);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_2))
            clearBit(PAD_R2);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_MIDDLE_RIGHT))
            clearBit(PAD_START);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_MIDDLE_LEFT))
            clearBit(PAD_SELECT);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_THUMB))
            clearBit(PAD_L3);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_THUMB))
            clearBit(PAD_R3);

        float lx = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_LEFT_X);
        float ly = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_LEFT_Y);
        float rx = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_RIGHT_X);
        float ry = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_RIGHT_Y);
        data[6] = static_cast<uint8_t>(128 + lx * 127);
        data[7] = static_cast<uint8_t>(128 + ly * 127);
        data[4] = static_cast<uint8_t>(128 + rx * 127);
        data[5] = static_cast<uint8_t>(128 + ry * 127);
    }
    else
    {
        for (const dod_input::KeyboardBinding &binding : g_keyboardBindings)
        {
            for (int key : binding.keys)
                if (key != 0 && IsKeyDown(key))
                    clearBit(binding.button);
        }
        // [DoD] Keyboard stick keys: full deflection while held; opposite keys together cancel to centre.
        const auto held = [](const std::array<int, 2> &keys)
        { return (keys[0] != 0 && IsKeyDown(keys[0])) || (keys[1] != 0 && IsKeyDown(keys[1])); };
        const auto axis = [&held](size_t negative, size_t positive)
        {
            const int value = (held(g_keyboardSticks[positive]) ? 1 : 0) - (held(g_keyboardSticks[negative]) ? 1 : 0);
            return static_cast<uint8_t>(value < 0 ? 0x00 : value > 0 ? 0xFF : kPadStickCenter);
        };
        data[6] = axis(2, 3); // left stick X: left / right
        data[7] = axis(0, 1); // left stick Y: up / down
        data[4] = axis(6, 7); // right stick X
        data[5] = axis(4, 5); // right stick Y
    }

    // [DoD] Reproducible test input: DOD_PAD_SCRIPT="seconds:BUTTON[:ms];..." holds BUTTON (UP, DOWN, LEFT, RIGHT,
    // START, SELECT, CROSS, CIRCLE, SQUARE, TRIANGLE, L1, R1, L2, R2, L3, R3) for ms (default 120) from that host
    // time on, exactly as a held key would. Each press is logged once.
    {
        struct ScriptedPress
        {
            double at = 0.0;
            double until = 0.0;
            uint16_t button = 0u;
            bool logged = false;
            char name[12] = {};
        };
        static std::vector<ScriptedPress> s_script = []
        {
            std::vector<ScriptedPress> script;
            const char *text = std::getenv("DOD_PAD_SCRIPT");
            static constexpr struct { const char *name; uint16_t bit; } kButtons[] = {
                {"UP", PAD_UP}, {"DOWN", PAD_DOWN}, {"LEFT", PAD_LEFT}, {"RIGHT", PAD_RIGHT}, {"START", PAD_START},
                {"SELECT", PAD_SELECT}, {"CROSS", PAD_CROSS}, {"CIRCLE", PAD_CIRCLE}, {"SQUARE", PAD_SQUARE},
                {"TRIANGLE", PAD_TRIANGLE}, {"L1", PAD_L1}, {"R1", PAD_R1}, {"L2", PAD_L2}, {"R2", PAD_R2},
                {"L3", PAD_L3}, {"R3", PAD_R3}};
            for (const char *p = text; p && *p;)
            {
                double at = 0.0;
                char name[12] = {};
                unsigned ms = 120u;
                int used = 0;
                if (std::sscanf(p, "%lf:%11[A-Z0-9]%n", &at, name, &used) < 2)
                    break;
                p += used;
                if (*p == ':')
                {
                    int more = 0;
                    std::sscanf(p, ":%u%n", &ms, &more);
                    p += more;
                }
                for (const auto &button : kButtons)
                {
                    if (std::strcmp(button.name, name) == 0)
                    {
                        ScriptedPress press;
                        press.at = at;
                        press.until = at + ms / 1000.0;
                        press.button = button.bit;
                        std::snprintf(press.name, sizeof(press.name), "%s", name);
                        script.push_back(press);
                    }
                }
                while (*p && *p != ';')
                    ++p;
                if (*p == ';')
                    ++p;
            }
            return script;
        }();
        // [DoD] DOD_TEST_AUTOSTART=1 (automated tests): from 40 s START and CIRCLE are pressed in turn every 1.5 s until
        // the game starts a new game (the runner sets g_dodTestNewGameAt from its 0x360810 hook), and the script's
        // times count from that moment - boot time varies from run to run.
        static const bool autostart = std::getenv("DOD_TEST_AUTOSTART") != nullptr;
        // DOD_TEST_EXIT_AFTER=<s>: the game exits that long after the new game started, so each automated run tests
        // the same length of play however long the boot took.
        static const double exitAfter = []() { const char *v = std::getenv("DOD_TEST_EXIT_AFTER"); return v ? std::atof(v) : 0.0; }();
        double now = GetTime();
        if (exitAfter > 0.0)
        {
            const double newGameAt = g_dodTestNewGameAt.load(std::memory_order_acquire);
            static bool closing = false;
            if (!closing && newGameAt >= 0.0 && now >= newGameAt + exitAfter)
            {
                // Close the window as the user would (WM_CLOSE): the normal shutdown stops the GS thread first.
                // (std::_Exit left PCSX2's GS thread running into the process teardown - a crash report each run.)
                closing = true;
                std::fprintf(stderr, "[dod-test] exit %.1f s after the new game\n", now - newGameAt);
                std::fflush(nullptr);
#if defined(_WIN32)
                PostMessageW(GetWindowHandle(), 0x0010u /* WM_CLOSE */, 0u, 0);
#else
                std::_Exit(0);
#endif
            }
        }
        if (autostart)
        {
            // DOD_TEST_AUTOSTART=<s> (more than 1): the presses start that many seconds after the boot (1 = 40 s).
            static const double autostartAt = []() { const char *v = std::getenv("DOD_TEST_AUTOSTART"); const double s = v ? std::atof(v) : 0.0; return s > 1.0 ? s : 40.0; }();
            const double newGameAt = g_dodTestNewGameAt.load(std::memory_order_acquire);
            if (newGameAt < 0.0)
            {
                if (now >= autostartAt)
                {
                    const double phase = std::fmod(now - autostartAt, 3.0);
                    if (phase < 0.15)
                        clearBit(PAD_START);
                    else if (phase >= 1.5 && phase < 1.65)
                        clearBit(PAD_CIRCLE);
                }
                now = -1.0; // no scripted press before the new game
            }
            else
            {
                now -= newGameAt;
                // [DoD] DOD_PAD_SCRIPT_FRAMES=<address> (RUNTIME_PATCHES row 124): the script's seconds are game frames
                // from the new game on (the u32 at that address, DoD's frame counter 0x817730; 60 a second), not host
                // time. The game logic advances once per game frame and stands still while a load holds the frame, so
                // the same script reaches the same game state here and in PCSX2 (tools/scripts/diag/pcsx2_compare.py
                // --clock frames), although loads take different times on the two.
                static const uint32_t frameAddr = []() {
                    const char *v = std::getenv("DOD_PAD_SCRIPT_FRAMES");
                    return v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 0)) : 0u;
                }();
                uint8_t *rdram = g_dodTestRdram.load(std::memory_order_acquire);
                if (frameAddr != 0u && rdram != nullptr && frameAddr + 4u <= 0x2000000u) // within the 32 MB of RAM
                {
                    static int64_t baseFrame = -1;
                    uint32_t frame = 0u;
                    std::memcpy(&frame, rdram + frameAddr, 4);
                    if (baseFrame < 0)
                    {
                        baseFrame = frame;
                        std::fprintf(stderr, "[dod-test] key script on game frames from 0x%X (frame %u)\n", frameAddr, frame);
                    }
                    now = static_cast<double>(static_cast<uint32_t>(frame - static_cast<uint32_t>(baseFrame))) / 60.0;
                }
            }
        }
        for (ScriptedPress &press : s_script)
        {
            if (now >= press.at && now < press.until)
            {
                clearBit(press.button);
                if (!press.logged)
                {
                    press.logged = true;
                    std::fprintf(stderr, "[pad] script t=%.3f %s\n", now, press.name);
                }
            }
        }
    }

    // Always drain the queue, including while a gamepad owns the input, so
    // discarded keyboard presses cannot become stale input on disconnection.
    snapshot.sampledAt = GetTime();
    snapshot.gamepad = useGamepad ? 1 : 0;
    static const bool traceKeys = std::getenv("DOD_PAD_TRACE") != nullptr;
    static unsigned keyTraceCount = 0;
    uint16_t queuedPresses = 0u;
    for (int key = GetKeyPressed(); key != 0; key = GetKeyPressed())
    {
        uint16_t mapped = 0u;
        for (const dod_input::KeyboardBinding &binding : g_keyboardBindings)
        {
            if (key == binding.keys[0] || key == binding.keys[1])
            {
                mapped = binding.button;
                break;
            }
        }
        if (!useGamepad)
            queuedPresses = static_cast<uint16_t>(queuedPresses | mapped);
        if (traceKeys && keyTraceCount < 128u)
        {
            std::fprintf(stderr, "[pad] key t=%.3f code=%d mapped=%04X gamepad=%d\n",
                         snapshot.sampledAt, key, static_cast<unsigned>(mapped), snapshot.gamepad);
            ++keyTraceCount;
        }
    }

    // A queued down/up remains visible for this complete host publication,
    // including multiple IOP polls before the host publishes another frame.
    const uint16_t frameButtons = static_cast<uint16_t>(btns & ~queuedPresses);
    data[2] = static_cast<uint8_t>(frameButtons);
    data[3] = static_cast<uint8_t>(frameButtons >> 8);
    std::lock_guard<std::mutex> lock(g_hostPadMutex);
    if (g_hostPadSnapshot.gamepad != snapshot.gamepad)
    {
        // Preserve gamepad priority: input retained from the previous source
        // must not leak through when the selected device changes.
        g_pendingHostPresses = 0u;
        g_previousHostHeld = 0u;
    }
    // Track physical held state only; a queued/frame-latched pulse must never
    // generate another rising edge when the next host frame releases it.
    const uint16_t held = static_cast<uint16_t>(~btns);
    g_pendingHostPresses = static_cast<uint16_t>(g_pendingHostPresses | queuedPresses |
                                                (held & ~g_previousHostHeld));
    g_previousHostHeld = held;
    g_hostPadSnapshot = snapshot;
}

static bool readHostPadSnapshot(uint8_t *data, size_t size, bool consumePresses)
{
    if (!data || size < 32)
        return false;

    std::lock_guard<std::mutex> lock(g_hostPadMutex);
    const HostPadSnapshot &snapshot = g_hostPadSnapshot;
    std::memcpy(data, snapshot.data, sizeof(snapshot.data));
    const uint16_t pending = g_pendingHostPresses;
    const uint16_t heldButtons = static_cast<uint16_t>(data[2] | (static_cast<uint16_t>(data[3]) << 8));
    const uint16_t btns = static_cast<uint16_t>(heldButtons & ~pending);
    data[2] = static_cast<uint8_t>(btns);
    data[3] = static_cast<uint8_t>(btns >> 8);
    if (consumePresses)
    {
        // A retained press may first be sampled several host frames later.
        // Preserve that delivered state through the current publication too,
        // so an IOP catch-up poll cannot overwrite it before the EE can read.
        g_hostPadSnapshot.data[2] = data[2];
        g_hostPadSnapshot.data[3] = data[3];
        g_pendingHostPresses = 0u;
    }

    // [DoD] Bounded, opt-in evidence for the host -> SIO2 button path.
    static const bool tracePad = std::getenv("DOD_PAD_TRACE") != nullptr;
    static int lastButtons = -1, lastGamepad = -1;
    static unsigned traceCount = 0;
    if (tracePad && traceCount < 1024u && (lastButtons != btns || lastGamepad != snapshot.gamepad))
    {
        std::fprintf(stderr, "[pad] host t=%.3f gamepad=%d buttons=%04X sticks=%u,%u,%u,%u pending=%04X consume=%d\n",
                     snapshot.sampledAt, snapshot.gamepad, static_cast<unsigned>(btns),
                     data[4], data[5], data[6], data[7], static_cast<unsigned>(pending), consumePresses ? 1 : 0);
        lastButtons = btns;
        lastGamepad = snapshot.gamepad;
        ++traceCount;
    }
    return true;
}

bool PSPadBackend::readState(int /*port*/, int /*slot*/, uint8_t *data, size_t size)
{
    // Existing HLE pad readers perform a complete input sample too.
    return readHostPadSnapshot(data, size, true);
}

// Configuration / device-presence queries must not acknowledge input events.
bool dodPeekHostPadInput(uint8_t *data, size_t size)
{
    return readHostPadSnapshot(data, size, false);
}

// Runner-only bridge for a normal, button-bearing SIO2 0x42 transaction.
// Sampling and consumption share the lock, so a later host press cannot be
// erased by a delayed acknowledgement of an older packet.
bool dodReadHostPadInputForPoll(uint8_t *data, size_t size)
{
    return readHostPadSnapshot(data, size, true);
}
