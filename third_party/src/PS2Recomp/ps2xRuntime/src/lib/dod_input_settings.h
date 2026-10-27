#pragma once

// DoD runner/private host-input contract. Codes match raylib KeyboardKey;
// this header deliberately does not depend on the runtime's public headers.
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <string>

namespace dod_input
{
    struct KeyboardBinding
    {
        uint16_t button;
        std::array<int, 2> keys;
    };
    using KeyboardBindings = std::array<KeyboardBinding, 16>;
    inline constexpr std::array<const char *, 16> settingNames = {
        "key_up", "key_down", "key_left", "key_right", "key_cross", "key_circle",
        "key_square", "key_triangle", "key_l1", "key_r1", "key_l2", "key_r2",
        "key_start", "key_select", "key_l3", "key_r3"};

    inline KeyboardBindings defaults()
    {
        return {{{0x0010u, {265, 87}}, {0x0040u, {264, 83}},
                 {0x0080u, {263, 65}}, {0x0020u, {262, 68}},
                 {0x4000u, {88, 32}}, {0x2000u, {67, 256}},
                 {0x8000u, {90, 320}}, {0x1000u, {86, 321}},
                 {0x0400u, {81, 0}}, {0x0800u, {69, 0}},
                 {0x0100u, {340, 0}}, {0x0200u, {344, 0}},
                 {0x0008u, {257, 0}}, {0x0001u, {258, 0}},
                 {0x0002u, {0, 0}}, {0x0004u, {0, 0}}}};
    }

    inline bool supportedKey(int key)
    {
        if (key == 0 || key == 32 || (key >= 48 && key <= 57) ||
            (key >= 65 && key <= 90) || (key >= 256 && key <= 269) ||
            (key >= 280 && key <= 284) || (key >= 290 && key <= 301) ||
            (key >= 320 && key <= 336) || (key >= 340 && key <= 348)) return true;
        switch (key)
        {
        case 39: case 44: case 45: case 46: case 47: case 59: case 61:
        case 91: case 92: case 93: case 96: return true;
        default: return false;
        }
    }

    inline bool integer(const std::string &text, int &value)
    {
        const char *start = text.c_str();
        char *end = nullptr;
        errno = 0;
        const long parsed = std::strtol(start, &end, 10);
        if (start == end || errno == ERANGE || parsed < std::numeric_limits<int>::min() ||
            parsed > std::numeric_limits<int>::max()) return false;
        while (*end && std::isspace(static_cast<unsigned char>(*end))) ++end;
        if (*end) return false;
        value = static_cast<int>(parsed);
        return true;
    }

    inline bool parsePair(const std::string &text, std::array<int, 2> &keys)
    {
        keys = {0, 0};
        const size_t comma = text.find(',');
        if (comma != std::string::npos && text.find(',', comma + 1) != std::string::npos) return false;
        if (!integer(text.substr(0, comma), keys[0]) || !supportedKey(keys[0])) return false;
        return comma == std::string::npos ||
            (integer(text.substr(comma + 1), keys[1]) && supportedKey(keys[1]));
    }

    // A malformed or ambiguous keyboard profile falls back as a whole, so a
    // partial override cannot silently steal a key from another PS2 button.
    inline bool load(const std::map<std::string, std::string> &ini,
                     KeyboardBindings &bindings, std::string &error)
    {
        KeyboardBindings candidate = defaults();
        bindings = defaults();
        error.clear();
        std::map<int, size_t> owners;
        for (size_t i = 0; i < candidate.size(); ++i)
        {
            const auto found = ini.find(settingNames[i]);
            if (found != ini.end() && !parsePair(found->second, candidate[i].keys))
            {
                error = std::string(settingNames[i]) + " must contain one or two supported key codes (0 disables a slot)";
                return false;
            }
            for (int key : candidate[i].keys)
            {
                if (key == 0) continue;
                const auto previous = owners.find(key);
                if (previous != owners.end() && previous->second != i)
                {
                    error = std::string("key ") + std::to_string(key) + " conflicts between " +
                        settingNames[previous->second] + " and " + settingNames[i];
                    return false;
                }
                owners[key] = i;
            }
        }
        bindings = candidate;
        return true;
    }

    // [DoD] Keyboard keys for the analog sticks (dod.ini key_lstick_up ... key_rstick_right, one or two keys each,
    // none by default): a held key pushes that stick fully in its direction. A key already used by a button, or a
    // malformed entry, drops only that stick direction, never the button profile.
    using StickBindings = std::array<std::array<int, 2>, 8>; // L up, down, left, right; R up, down, left, right
    inline constexpr std::array<const char *, 8> stickSettingNames = {
        "key_lstick_up", "key_lstick_down", "key_lstick_left", "key_lstick_right",
        "key_rstick_up", "key_rstick_down", "key_rstick_left", "key_rstick_right"};

    inline StickBindings loadSticks(const std::map<std::string, std::string> &ini, const KeyboardBindings &buttons,
                                    std::string &error)
    {
        StickBindings sticks{};
        error.clear();
        std::map<int, std::string> owners;
        for (const KeyboardBinding &binding : buttons)
            for (int key : binding.keys)
                if (key != 0) owners[key] = "a button";
        for (size_t i = 0; i < sticks.size(); ++i)
        {
            const auto found = ini.find(stickSettingNames[i]);
            std::array<int, 2> keys{};
            if (found == ini.end()) continue;
            if (!parsePair(found->second, keys))
            {
                error += std::string(stickSettingNames[i]) + " ignored (one or two supported key codes); ";
                continue;
            }
            bool clash = false;
            for (int key : keys)
                if (key != 0 && owners.count(key))
                {
                    error += std::string(stickSettingNames[i]) + " ignored: key " + std::to_string(key) + " already used by " +
                        owners[key] + "; ";
                    clash = true;
                }
            if (clash) continue;
            for (int key : keys)
                if (key != 0) owners[key] = stickSettingNames[i];
            sticks[i] = keys;
        }
        return sticks;
    }

    inline bool windowSize(const std::string &text, int &width, int &height)
    {
        const size_t split = text.find_first_of("xX");
        if (split == std::string::npos || !integer(text.substr(0, split), width) ||
            !integer(text.substr(split + 1), height)) return false;
        return width > 0 && height > 0 && width <= 16384 && height <= 16384;
    }
}

// Startup only: call before runtime.initialize()/the first window-thread poll.
void dodConfigureKeyboardInput(const dod_input::KeyboardBindings &bindings);
void dodConfigureKeyboardSticks(const dod_input::StickBindings &sticks);
