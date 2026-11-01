// Generic in-game cheat menu (F2). No game's cheats live here: the entries are read from `cheats.ini` (format in
// docs/CHEATS.md). Every entry is a checkbox; while it is on, its RAM writes are applied once per host frame, as an
// emulator applies a RAM cheat code. Everything starts off at each launch.
// The panel is the GS's ImGui menu (pcsx2gs_api.h set_menu): docked at the left, scaled with the image height, mouse
// read from Win32 in image pixels (dodpcsx2::menuInput), keyboard / controller navigation; the game gets a released pad
// while it is open. Labels are UTF-8 text from the file (the GS merges the first CJK font Windows has).
#include "ps2_cheats.h"

#include "gs/dod_pcsx2_gs.h"
#include "gs/pcsx2gs_api.h"
#include "ps2_host_backend.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
// Declared by hand: <windows.h> clashes with raylib's names (Rectangle, CloseWindow, ...).
extern "C" __declspec(dllimport) unsigned long __stdcall GetModuleFileNameA(void *module, char *name, unsigned long size);
#endif

namespace
{
    constexpr uint32_t kGuestRamSize = 32u * 1024u * 1024u;
    constexpr size_t kMaxCheats = 512;
    constexpr size_t kMaxWritesPerCheat = 64;

    struct Write
    {
        uint32_t address = 0;
        uint8_t size = 4;       // 1, 2 or 4 bytes, little endian
        uint32_t value = 0;
        uint32_t count = 1;     // repeated `count` times, `stride` bytes apart
        uint32_t stride = 0;
        bool skipZero = false;  // leave a slot alone while it holds zero (an empty inventory slot stays empty)
    };

    struct Entry
    {
        bool section = false;   // a title line, not a checkbox
        std::string label;
        std::string help;
        std::string note;
        std::vector<Write> writes;
    };

    std::vector<Entry> s_entries;
    std::unique_ptr<std::atomic<bool>[]> s_on; // parallel to s_entries
    std::string s_loadedFrom;
    std::string s_loadMessage;

    std::atomic<bool> s_open{false};        // F2 / Esc on the window thread, the close button on the GS thread
    std::atomic<bool> s_justOpened{false};  // the next drawn frame gives the panel the focus
    std::atomic<uint16_t> s_hostButtons{0xFFFFu};
    std::atomic<bool> s_hostGamepad{false};

    std::string trim(const std::string &s)
    {
        size_t b = 0, e = s.size();
        while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
            ++b;
        while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
            --e;
        return s.substr(b, e - b);
    }

    bool fileExists(const std::string &path)
    {
        if (FILE *f = std::fopen(path.c_str(), "rb"))
        {
            std::fclose(f);
            return true;
        }
        return false;
    }

    std::string exeDirectory()
    {
#ifdef _WIN32
        char buf[1024] = {};
        const unsigned long n = GetModuleFileNameA(nullptr, buf, static_cast<unsigned long>(sizeof(buf)));
        if (n > 0 && n < sizeof(buf))
        {
            std::string p(buf, n);
            const size_t slash = p.find_last_of("\\/");
            if (slash != std::string::npos)
                return p.substr(0, slash + 1);
        }
#endif
        return std::string();
    }

    // WRITE line: ADDRESS,SIZE,VALUE[,count=N][,stride=S][,skip_zero]
    // SIZE is u8 / u16 / u32 (or 1 / 2 / 4); numbers are decimal or 0x hex.
    bool parseWrite(const std::string &text, Write &out, std::string &error)
    {
        std::vector<std::string> parts;
        size_t start = 0;
        for (;;)
        {
            const size_t comma = text.find(',', start);
            parts.push_back(trim(text.substr(start, comma == std::string::npos ? std::string::npos : comma - start)));
            if (comma == std::string::npos)
                break;
            start = comma + 1;
        }
        if (parts.size() < 3)
        {
            error = "write needs ADDRESS,SIZE,VALUE";
            return false;
        }
        char *end = nullptr;
        const unsigned long address = std::strtoul(parts[0].c_str(), &end, 0);
        if (end == parts[0].c_str() || *end)
        {
            error = "bad address";
            return false;
        }
        const std::string &sz = parts[1];
        if (sz == "u8" || sz == "1")
            out.size = 1;
        else if (sz == "u16" || sz == "2")
            out.size = 2;
        else if (sz == "u32" || sz == "4")
            out.size = 4;
        else
        {
            error = "size must be u8, u16 or u32";
            return false;
        }
        const unsigned long long value = std::strtoull(parts[2].c_str(), &end, 0);
        if (end == parts[2].c_str() || *end)
        {
            error = "bad value";
            return false;
        }
        out.address = static_cast<uint32_t>(address) & 0x1FFFFFFFu; // KSEG / cached mirrors map to the same RAM
        out.value = static_cast<uint32_t>(value);
        for (size_t i = 3; i < parts.size(); ++i)
        {
            const std::string &opt = parts[i];
            if (opt == "skip_zero")
                out.skipZero = true;
            else if (opt.rfind("count=", 0) == 0)
                out.count = static_cast<uint32_t>(std::strtoul(opt.c_str() + 6, nullptr, 0));
            else if (opt.rfind("stride=", 0) == 0)
                out.stride = static_cast<uint32_t>(std::strtoul(opt.c_str() + 7, nullptr, 0));
            else
            {
                error = "unknown option '" + opt + "'";
                return false;
            }
        }
        if (out.count == 0)
            out.count = 1;
        const uint64_t last = static_cast<uint64_t>(out.address) + static_cast<uint64_t>(out.count - 1) * out.stride + out.size;
        if (last > kGuestRamSize)
        {
            error = "address range is outside the 32 MB of guest RAM";
            return false;
        }
        return true;
    }

    void loadFile()
    {
        std::string path;
        if (const char *env = std::getenv("PS2_CHEATS_FILE"); env && *env)
            path = env;
        else
        {
            const std::string beside = exeDirectory() + "cheats.ini";
            path = fileExists(beside) ? beside : std::string("cheats.ini");
        }
        FILE *f = std::fopen(path.c_str(), "rb");
        if (!f)
        {
            s_loadMessage = "No cheats.ini found (see docs/CHEATS.md).";
            std::fprintf(stderr, "[cheats] no cheat file (%s)\n", path.c_str());
            return;
        }
        std::string data;
        char chunk[4096];
        for (size_t n; (n = std::fread(chunk, 1, sizeof(chunk), f)) > 0;)
            data.append(chunk, n);
        std::fclose(f);
        if (data.size() >= 3 && static_cast<uint8_t>(data[0]) == 0xEF && static_cast<uint8_t>(data[1]) == 0xBB &&
            static_cast<uint8_t>(data[2]) == 0xBF)
            data.erase(0, 3); // UTF-8 byte order mark

        size_t lineNo = 0;
        for (size_t pos = 0; pos <= data.size();)
        {
            size_t eol = data.find('\n', pos);
            if (eol == std::string::npos)
                eol = data.size();
            const std::string line = trim(data.substr(pos, eol - pos));
            pos = eol + 1;
            ++lineNo;
            if (line.empty() || line[0] == ';' || line[0] == '#')
                continue;
            if (line[0] == '[' && line.back() == ']')
            {
                if (s_entries.size() >= kMaxCheats)
                    break;
                Entry e;
                const std::string name = trim(line.substr(1, line.size() - 2));
                if (name.rfind("section:", 0) == 0)
                {
                    e.section = true;
                    e.label = trim(name.substr(8));
                }
                else
                    e.label = name;
                s_entries.push_back(std::move(e));
                continue;
            }
            const size_t eq = line.find('=');
            if (eq == std::string::npos || s_entries.empty() || s_entries.back().section)
            {
                std::fprintf(stderr, "[cheats] %s:%zu: ignored line\n", path.c_str(), lineNo);
                continue;
            }
            const std::string key = trim(line.substr(0, eq));
            const std::string val = trim(line.substr(eq + 1));
            Entry &e = s_entries.back();
            if (key == "help")
                e.help = val;
            else if (key == "note")
                e.note = val;
            else if (key == "write")
            {
                Write w;
                std::string error;
                if (e.writes.size() >= kMaxWritesPerCheat)
                    error = "too many write lines for one entry";
                else if (parseWrite(val, w, error))
                {
                    e.writes.push_back(w);
                    continue;
                }
                std::fprintf(stderr, "[cheats] %s:%zu: %s\n", path.c_str(), lineNo, error.c_str());
            }
            else
                std::fprintf(stderr, "[cheats] %s:%zu: unknown key '%s'\n", path.c_str(), lineNo, key.c_str());
        }
        s_on.reset(new std::atomic<bool>[s_entries.size()]);
        for (size_t i = 0; i < s_entries.size(); ++i)
            s_on[i].store(false, std::memory_order_relaxed);
        s_loadedFrom = path;
        size_t cheats = 0;
        for (const Entry &e : s_entries)
            cheats += e.section ? 0u : 1u;
        std::fprintf(stderr, "[cheats] %zu entries from %s\n", cheats, path.c_str());
        if (cheats == 0)
            s_loadMessage = "cheats.ini has no entries.";
    }

    void setCheat(size_t index, bool on)
    {
        if (index >= s_entries.size() || s_entries[index].section || s_on[index].load(std::memory_order_relaxed) == on)
            return;
        s_on[index].store(on, std::memory_order_relaxed);
        std::fprintf(stderr, "[cheats] %s: %s\n", s_entries[index].label.c_str(), on ? "on" : "off");
    }

    // GS thread, at every present while the menu is shown.
    void drawMenu(const Pcsx2GsUi *ui, void *)
    {
        int open = 1;
        const char *title = "Cheats###ps2cheats";
        const uint32_t flags = PCSX2GS_PANEL_LEFT | (s_justOpened.exchange(false) ? PCSX2GS_PANEL_FOCUS : 0u);
        const bool panel = ui->version >= 3u && ui->begin_panel;
        const int visible = panel ? ui->begin_panel(title, &open, 440.0f, 860.0f, flags) : ui->begin_window(title, &open, 440.0f, 860.0f);
        if (visible)
        {
            ui->text_disabled("F2 or Esc closes");
            ui->text_disabled("mouse / arrows + space / pad D-pad + A");
            if (!s_loadMessage.empty())
                ui->text_wrapped(s_loadMessage.c_str());
            for (size_t i = 0; i < s_entries.size(); ++i)
            {
                const Entry &e = s_entries[i];
                if (e.section)
                {
                    ui->separator_text(e.label.c_str());
                    continue;
                }
                ui->push_id(static_cast<int>(i));
                int value = s_on[i].load(std::memory_order_relaxed) ? 1 : 0;
                if (ui->checkbox(e.label.c_str(), &value))
                    setCheat(i, value != 0);
                if (panel && ui->tooltip && !e.help.empty())
                    ui->tooltip(e.help.c_str());
                if (!e.note.empty())
                    ui->text_colored(0xFF4A8CF5u, e.note.c_str()); // orange (R in the low byte)
                ui->pop_id();
            }
            ui->spacing();
            if (s_on && ui->button("All off"))
                for (size_t i = 0; i < s_entries.size(); ++i)
                    setCheat(i, false);
            ui->text_wrapped("Only for this run; every cheat is off again at the next launch.");
        }
        ui->end_window();
        if (!open)
            s_open.store(false, std::memory_order_relaxed);
    }

    // PS2_CHEATS=all | <n>,<n>... (tests): switches on at start, n = the entry's number counted from 0 among the
    // checkboxes (section titles do not count); PS2_CHEAT_MENU_AT=<seconds>: opens the menu then.
    void applyTestEnv()
    {
        const char *v = std::getenv("PS2_CHEATS");
        if (!v || !*v || !s_on)
            return;
        if (std::strcmp(v, "all") == 0)
        {
            for (size_t i = 0; i < s_entries.size(); ++i)
                setCheat(i, true);
            return;
        }
        for (const char *p = v; *p;)
        {
            char *end = nullptr;
            const long want = std::strtol(p, &end, 10);
            if (end == p)
                break;
            long n = 0;
            for (size_t i = 0; i < s_entries.size(); ++i)
                if (!s_entries[i].section && n++ == want)
                {
                    setCheat(i, true);
                    break;
                }
            p = *end == ',' ? end + 1 : end;
        }
    }

    uint32_t menuKeys()
    {
        uint32_t keys = 0u;
        const auto key = [&](int raylibKey, uint32_t bit) {
            if (IsKeyDown(raylibKey))
                keys |= bit;
        };
        key(KEY_UP, PCSX2GS_KEY_UP);
        key(KEY_DOWN, PCSX2GS_KEY_DOWN);
        key(KEY_LEFT, PCSX2GS_KEY_LEFT);
        key(KEY_RIGHT, PCSX2GS_KEY_RIGHT);
        key(KEY_ENTER, PCSX2GS_KEY_ENTER);
        key(KEY_KP_ENTER, PCSX2GS_KEY_ENTER);
        key(KEY_SPACE, PCSX2GS_KEY_SPACE);
        key(KEY_ESCAPE, PCSX2GS_KEY_ESCAPE);
        key(KEY_TAB, PCSX2GS_KEY_TAB);
        key(KEY_LEFT_SHIFT, PCSX2GS_KEY_SHIFT);
        key(KEY_RIGHT_SHIFT, PCSX2GS_KEY_SHIFT);
        key(KEY_LEFT_CONTROL, PCSX2GS_KEY_CTRL);
        key(KEY_RIGHT_CONTROL, PCSX2GS_KEY_CTRL);
        return keys;
    }

    // The controller's buttons (only when it is the active device: the keyboard's own mapping to pad buttons would
    // move the cursor twice) as the menu's: D-pad, A / cross = accept, B / circle = back, L1 / R1.
    uint32_t menuPad()
    {
        if (!s_hostGamepad.load(std::memory_order_relaxed))
            return 0u;
        const uint16_t held = static_cast<uint16_t>(~s_hostButtons.load(std::memory_order_relaxed));
        uint32_t pad = 0u;
        if (held & 0x0010u) pad |= PCSX2GS_PAD_UP;
        if (held & 0x0040u) pad |= PCSX2GS_PAD_DOWN;
        if (held & 0x0080u) pad |= PCSX2GS_PAD_LEFT;
        if (held & 0x0020u) pad |= PCSX2GS_PAD_RIGHT;
        if (held & 0x4000u) pad |= PCSX2GS_PAD_ACCEPT;
        if (held & 0x2000u) pad |= PCSX2GS_PAD_BACK;
        if (held & 0x0400u) pad |= PCSX2GS_PAD_L1;
        if (held & 0x0800u) pad |= PCSX2GS_PAD_R1;
        return pad;
    }

    void applyWrites(uint8_t *rdram)
    {
        for (size_t i = 0; i < s_entries.size(); ++i)
        {
            if (s_entries[i].section || !s_on[i].load(std::memory_order_relaxed))
                continue;
            for (const Write &w : s_entries[i].writes)
                for (uint32_t n = 0; n < w.count; ++n)
                {
                    uint8_t *p = rdram + w.address + static_cast<size_t>(n) * w.stride;
                    uint32_t current = 0;
                    std::memcpy(&current, p, w.size);
                    if (w.skipZero && current == 0u)
                        continue;
                    if (current != w.value)
                        std::memcpy(p, &w.value, w.size); // little endian host, as the EE
                }
        }
    }
}

void ps2CheatsSetHostPad(uint16_t buttons, bool gamepad)
{
    s_hostButtons.store(buttons, std::memory_order_relaxed);
    s_hostGamepad.store(gamepad, std::memory_order_relaxed);
}

bool ps2CheatsMenuOpen()
{
    return s_open.load(std::memory_order_relaxed);
}

void ps2CheatsHostTick(uint8_t *rdram)
{
    static bool started = false;
    static bool shown = false;
    static double menuAt = []() { const char *v = std::getenv("PS2_CHEAT_MENU_AT"); return v ? std::atof(v) : 0.0; }();
    if (!started)
    {
        started = true;
        loadFile();
        applyTestEnv();
    }
    const bool autoOpen = menuAt > 0.0 && GetTime() >= menuAt;
    if (autoOpen)
        menuAt = 0.0;
    if (IsKeyPressed(KEY_F2) || autoOpen)
    {
        const bool open = autoOpen || !s_open.load(std::memory_order_relaxed);
        s_open.store(open, std::memory_order_relaxed);
        if (open)
            s_justOpened.store(true, std::memory_order_relaxed);
    }
    else if (shown && IsKeyPressed(KEY_ESCAPE))
        s_open.store(false, std::memory_order_relaxed);

    const bool open = s_open.load(std::memory_order_relaxed);
    if (open != shown)
    {
        dodpcsx2::setMenu(open ? drawMenu : nullptr, nullptr);
        if (!open)
            dodpcsx2::menuInput(0u, 0u, 0.0f); // nothing held when it opens again
        shown = open;
        std::fprintf(stderr, "[cheats] menu %s\n", open ? "open" : "closed");
    }
    if (open)
        dodpcsx2::menuInput(menuKeys(), menuPad(), GetMouseWheelMove());

    if (rdram && s_on)
        applyWrites(rdram);
}
