#include "runtime/ps2_pad.h"
#include "ps2_host_backend.h"
#include "dod_input_settings.h"
#include "dod_gamepad.h"
#include "ps2_cheats.h"
#include "ps2_input_readers.h"
#include "ps2_pad_diagnostics.h"
#include <string_view>
#include <atomic>
#include <algorithm>
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

// Optional runner-owned test epoch and RAM. No game address is selected here.
std::atomic<double> g_dodTestNewGameAt{-1.0};
std::atomic<uint8_t *> g_dodTestRdram{nullptr};
std::atomic<uint64_t> g_padSessionGeneration{0};

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

            static constexpr struct { const char *name; uint16_t bit; } kButtons[] = {
                {"UP", PAD_UP}, {"DOWN", PAD_DOWN}, {"LEFT", PAD_LEFT}, {"RIGHT", PAD_RIGHT}, {"START", PAD_START},
                {"SELECT", PAD_SELECT}, {"CROSS", PAD_CROSS}, {"CIRCLE", PAD_CIRCLE}, {"SQUARE", PAD_SQUARE},
                {"TRIANGLE", PAD_TRIANGLE}, {"L1", PAD_L1}, {"R1", PAD_R1}, {"L2", PAD_L2}, {"R2", PAD_R2},
                {"L3", PAD_L3}, {"R3", PAD_R3}};
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
    ps2input::PressReaders g_hostReaders;
    void releaseCapturedInput() // caller holds g_hostPadMutex
    {
        auto* data = g_hostPadSnapshot.data;
        data[2] = data[3] = 0xFF;
        std::fill(data + 4, data + 8, kPadStickCenter);
        std::fill(data + 8, data + 32, 0);
        g_pendingHostPresses = g_previousHostHeld = 0;
        g_hostReaders.clearPending();
    }
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
        // Boot navigation is port-owned; setting an old auto-start variable alone
        // never guesses START/CIRCLE or a reference game's boot delay.
        static const std::vector<uint16_t> bootButtons = [] {
            std::vector<uint16_t> result;
            const char* env = std::getenv("PS2_PAD_BOOT_SEQUENCE");
            std::string_view rest = env ? env : "";
            while(!rest.empty()) {
                const auto comma=rest.find(','); const auto token=rest.substr(0,comma);
                uint16_t bit=0; for(const auto& b:kButtons) if(token==b.name) bit=b.bit;
                if(!bit || result.size()>=16) return std::vector<uint16_t>{};
                result.push_back(bit);
                if(comma==std::string_view::npos) break;
                rest.remove_prefix(comma+1);
                if(rest.empty()) return std::vector<uint16_t>{};
            }
            return result;
        }();
        const bool autostart = !bootButtons.empty();
        static uint64_t generation = UINT64_MAX;
        static int64_t baseFrame = -1;
        static double previousEpoch = -1;
        static bool closing = false;
        const auto currentGeneration = g_padSessionGeneration.load(std::memory_order_acquire);
        const double epoch = g_dodTestNewGameAt.load(std::memory_order_acquire);
        if(generation != currentGeneration || previousEpoch != epoch) {
            generation=currentGeneration;previousEpoch=epoch;baseFrame=-1;closing=false;
            for(auto& press:s_script) press.logged=false;
        }
        // DOD_TEST_EXIT_AFTER=<s>: the game exits that long after the new game started, so each automated run tests
        // the same length of play however long the boot took.
        static const double exitAfter = []() { const char *v = std::getenv("DOD_TEST_EXIT_AFTER"); return v ? std::atof(v) : 0.0; }();
        double now = GetTime();
        if (exitAfter > 0.0)
        {
            const double newGameAt = g_dodTestNewGameAt.load(std::memory_order_acquire);
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
            static const double autostartAt = []() {
                const char* v=std::getenv("DOD_TEST_AUTOSTART"); char* tail=nullptr;
                const double seconds=v ? std::strtod(v,&tail) : 0;
                return v && tail!=v && !*tail && std::isfinite(seconds) && seconds>=0 ? seconds : 0.0;
            }();
            const double newGameAt = g_dodTestNewGameAt.load(std::memory_order_acquire);
            if (newGameAt < 0.0)
            {
                if (now >= autostartAt)
                {
                    const double elapsed=now-autostartAt;
                    const double step=std::floor(elapsed/1.5);
                    if(std::fmod(elapsed,1.5)<0.15)
                        clearBit(bootButtons[static_cast<size_t>(std::fmod(step,static_cast<double>(bootButtons.size())))]);
                }
                now = -1.0; // no scripted press before the new game
            }
            else
            {
                now -= newGameAt;
                // An optional frame clock needs both an explicit RAM word and
                // its measured frequency. No game's counter or 60 Hz is assumed.
                static const uint32_t frameAddr = ps2input::diagnosticWordAddress(std::getenv("DOD_PAD_SCRIPT_FRAMES"));
                static const double frameHz = []() {
                    const char* v=std::getenv("PS2_PAD_SCRIPT_HZ");char* tail=nullptr;
                    const double hz=v?std::strtod(v,&tail):0;
                    return v && tail!=v && !*tail && std::isfinite(hz) && hz>0 && hz<=1000 ? hz : 0.0;
                }();
                uint8_t *rdram = g_dodTestRdram.load(std::memory_order_acquire);
                if (frameAddr != 0u && rdram != nullptr && frameHz > 0) // within the 32 MB of RAM
                {
                    uint32_t frame = 0u;
                    std::memcpy(&frame, rdram + frameAddr, 4);
                    if (baseFrame < 0)
                    {
                        baseFrame = frame;
                        std::fprintf(stderr, "[dod-test] key script on game frames from 0x%X (frame %u)\n", frameAddr, frame);
                    }
                    now = static_cast<double>(static_cast<uint32_t>(frame - static_cast<uint32_t>(baseFrame))) / frameHz;
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

    // The F2 cheat menu (ps2_cheats.cpp) navigates with this frame's buttons; while it is open the game gets a
    // released pad (sticks centred, no queued presses).
    ps2CheatsSetHostPad(static_cast<uint16_t>(btns & ~queuedPresses), useGamepad);
    const bool captured = ps2CheatsMenuOpen();
    if (captured)
    {
        btns = 0xFFFFu;
        queuedPresses = 0u;
        data[4] = data[5] = data[6] = data[7] = kPadStickCenter;
        std::fill(data + 8, data + 32, 0);
    }

    // A queued down/up remains visible for this complete host publication,
    // including multiple IOP polls before the host publishes another frame.
    const uint16_t frameButtons = static_cast<uint16_t>(btns & ~queuedPresses);
    data[2] = static_cast<uint8_t>(frameButtons);
    data[3] = static_cast<uint8_t>(frameButtons >> 8);
    std::lock_guard<std::mutex> lock(g_hostPadMutex);
    if (captured) releaseCapturedInput();
    if (g_hostPadSnapshot.gamepad != snapshot.gamepad)
    {
        // Preserve gamepad priority: input retained from the previous source
        // must not leak through when the selected device changes.
        g_pendingHostPresses = 0u;
        g_hostReaders.clearPending();
        g_previousHostHeld = 0u;
    }
    // Track physical held state only; a queued/frame-latched pulse must never
    // generate another rising edge when the next host frame releases it.
    const uint16_t held = static_cast<uint16_t>(~btns);
    g_hostReaders.publish(static_cast<uint16_t>(queuedPresses | (held & ~g_previousHostHeld)));
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
    if (ps2CheatsMenuOpen()) releaseCapturedInput();
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

ps2input::Reader ps2input::registerReader()
{
    std::lock_guard lock(g_hostPadMutex);
    return g_hostReaders.add();
}
bool ps2input::unregisterReader(Reader token)
{
    std::lock_guard lock(g_hostPadMutex);
    return g_hostReaders.remove(token);
}
bool ps2input::readLatest(Reader token, uint8_t* data, size_t size)
{
    if (!data || size < sizeof(g_hostPadSnapshot.data)) return false;
    std::lock_guard lock(g_hostPadMutex);
    uint16_t pending = 0;
    if (!g_hostReaders.consume(token, pending)) return false;
    std::memcpy(data, g_hostPadSnapshot.data, sizeof(g_hostPadSnapshot.data));
    if (ps2CheatsMenuOpen())
    {
        releaseCapturedInput();
        std::memcpy(data, g_hostPadSnapshot.data, sizeof(g_hostPadSnapshot.data));
        return true;
    }
    // Use physical held state, not the legacy SIO2 reader's publication latch.
    const uint16_t buttons = static_cast<uint16_t>(~(g_previousHostHeld | pending));
    data[2] = static_cast<uint8_t>(buttons);
    data[3] = static_cast<uint8_t>(buttons >> 8);
    return true;
}
void ps2input::resetSession()
{
    std::lock_guard lock(g_hostPadMutex);
    g_hostReaders.reset();
    g_hostPadSnapshot = {};
    g_pendingHostPresses = g_previousHostHeld = 0;
    g_padSessionGeneration.fetch_add(1, std::memory_order_release);
    g_dodTestNewGameAt.store(-1.0);
    g_dodTestRdram.store(nullptr);
}
