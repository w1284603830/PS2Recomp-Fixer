// F3 trainer (修改器) panel. Compiled only when CMake PS2X_ENABLE_TRAINER=ON (default OFF: ps2_trainer_disabled.cpp is
// built instead and none of this exists in the program). 預設屏蔽；如有需要可以使用（見 docs/TRAINER.md）。
//
// The base ships no game's trainer: the panel lists the switches and choices a port registered (ps2_trainer.h) and the
// port's own hooks read them. Values are lock-free atomics, so the EE thread's generated code can query them at any
// time; the panel only publishes the user's choice. Everything is off / option 0 at each launch.
// The panel is the GS's ImGui menu (pcsx2gs_api.h set_menu), docked at the right edge (the F2 cheat menu is on the
// left). The two panels share the GS menu slot: F3 does nothing while the F2 panel is open, and F2 pressed while this
// panel is open switches over to the F2 panel.
#include "ps2_trainer.h"

#include "ps2_cheats.h"
#include "gs/dod_pcsx2_gs.h"
#include "gs/pcsx2gs_api.h"
#include "ps2_host_backend.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#if !PS2X_TRAINER
#error "ps2_trainer.cpp is the enabled F3 trainer: build it with PS2X_TRAINER=1 (CMake -DPS2X_ENABLE_TRAINER=ON)"
#endif

namespace
{
    constexpr int kMaxEntries = 256;
    constexpr int kMaxOptions = 32;

    enum class Kind
    {
        Section,
        Switch,
        Choice,
    };

    struct Entry
    {
        Kind kind = Kind::Switch;
        std::string label;
        std::string help;
        std::vector<std::string> options;
    };

    // Guest-visible state: 0 = off / option 0 (the original behaviour). Static storage, zero before any registration.
    std::array<std::atomic<int>, kMaxEntries> s_values{};

    // Registration may run from static initialisers of a port's generated files: construct on first use.
    std::mutex &registryMutex()
    {
        static std::mutex mutex;
        return mutex;
    }
    std::vector<Entry> &registry()
    {
        static std::vector<Entry> entries;
        return entries;
    }

    std::atomic<bool> s_open{false};
    std::atomic<bool> s_justOpened{false};
    std::atomic<uint16_t> s_hostButtons{0xFFFFu};
    std::atomic<bool> s_hostGamepad{false};

    bool validId(ps2trainer::Id id) noexcept
    {
        return id >= 0 && id < kMaxEntries;
    }

    ps2trainer::Id add(Entry entry)
    {
        std::lock_guard<std::mutex> lock(registryMutex());
        std::vector<Entry> &entries = registry();
        if (static_cast<int>(entries.size()) >= kMaxEntries)
        {
            std::fprintf(stderr, "[trainer] more than %d entries; '%s' ignored\n", kMaxEntries, entry.label.c_str());
            return -1;
        }
        entries.push_back(std::move(entry));
        const ps2trainer::Id id = static_cast<ps2trainer::Id>(entries.size()) - 1;
        s_values[static_cast<size_t>(id)].store(0, std::memory_order_relaxed);
        return id;
    }

    // Caller holds registryMutex.
    void setValueLocked(ps2trainer::Id id, int value)
    {
        const std::vector<Entry> &entries = registry();
        if (!validId(id) || id >= static_cast<int>(entries.size()))
            return;
        const Entry &entry = entries[static_cast<size_t>(id)];
        if (entry.kind == Kind::Section)
            return;
        if (entry.kind == Kind::Switch)
            value = value != 0 ? 1 : 0;
        else if (value < 0 || value >= static_cast<int>(entry.options.size()))
            return;
        if (s_values[static_cast<size_t>(id)].exchange(value, std::memory_order_relaxed) == value)
            return;
        if (entry.kind == Kind::Switch)
            std::fprintf(stderr, "[trainer] %s: %s\n", entry.label.c_str(), value ? "on" : "off");
        else
            std::fprintf(stderr, "[trainer] %s: %s\n", entry.label.c_str(), entry.options[static_cast<size_t>(value)].c_str());
    }

    // GS thread, at every present while the panel is shown.
    void drawMenu(const Pcsx2GsUi *ui, void *)
    {
        int open = 1;
        const char *title = "Trainer###ps2trainer";
        const uint32_t flags = s_justOpened.exchange(false) ? PCSX2GS_PANEL_FOCUS : 0u; // no PANEL_LEFT: right edge
        const bool panel = ui->version >= 3u && ui->begin_panel;
        const int visible = panel ? ui->begin_panel(title, &open, 440.0f, 860.0f, flags) : ui->begin_window(title, &open, 440.0f, 860.0f);
        if (visible)
        {
            ui->text_disabled("F3 or Esc closes");
            ui->text_disabled("mouse / arrows + space / pad D-pad + A");
            std::lock_guard<std::mutex> lock(registryMutex());
            const std::vector<Entry> &entries = registry();
            if (entries.empty())
                ui->text_wrapped("This port registered no trainer switches (docs/TRAINER.md).");
            std::vector<const char *> items;
            for (size_t i = 0; i < entries.size(); ++i)
            {
                const Entry &e = entries[i];
                const ps2trainer::Id id = static_cast<ps2trainer::Id>(i);
                if (e.kind == Kind::Section)
                {
                    if (ui->version >= 2u && ui->separator_text)
                        ui->separator_text(e.label.c_str());
                    else
                        ui->heading(e.label.c_str());
                    continue;
                }
                ui->push_id(id);
                int value = s_values[i].load(std::memory_order_relaxed);
                if (e.kind == Kind::Switch)
                {
                    if (ui->checkbox(e.label.c_str(), &value))
                        setValueLocked(id, value);
                }
                else if (ui->version >= 2u && ui->combo)
                {
                    items.clear();
                    for (const std::string &option : e.options)
                        items.push_back(option.c_str());
                    if (ui->combo(e.label.c_str(), &value, items.data(), static_cast<int>(items.size())))
                        setValueLocked(id, value);
                }
                else
                {
                    const std::string line = e.label + ": " + e.options[static_cast<size_t>(value)];
                    ui->text(line.c_str());
                }
                if (panel && ui->tooltip && !e.help.empty())
                    ui->tooltip(e.help.c_str());
                ui->pop_id();
            }
            ui->spacing();
            if (!entries.empty() && ui->button("All off"))
                for (size_t i = 0; i < entries.size(); ++i)
                    setValueLocked(static_cast<ps2trainer::Id>(i), 0);
            ui->text_wrapped("Only for this run; everything is off again at the next launch.");
        }
        ui->end_window();
        if (!open)
            s_open.store(false, std::memory_order_relaxed);
    }

    // PS2_TRAINER=all | <n>,<n>... (tests): switches on at the first host frame, n = the switch's number counted from 0
    // among the switches (sections and choices do not count). PS2_TRAINER_MENU_AT=<seconds> opens the panel then.
    void applyTestEnv()
    {
        const char *v = std::getenv("PS2_TRAINER");
        if (!v || !*v)
            return;
        std::lock_guard<std::mutex> lock(registryMutex());
        const std::vector<Entry> &entries = registry();
        const bool all = std::strcmp(v, "all") == 0;
        for (const char *p = v; all || *p;)
        {
            long want = -1;
            if (!all)
            {
                char *end = nullptr;
                want = std::strtol(p, &end, 10);
                if (end == p || want < 0)
                {
                    std::fprintf(stderr, "[trainer] PS2_TRAINER='%s' is not 'all' or a list of switch numbers\n", v);
                    return;
                }
                p = *end == ',' ? end + 1 : end;
            }
            long n = 0;
            for (size_t i = 0; i < entries.size(); ++i)
                if (entries[i].kind == Kind::Switch && (all || n++ == want))
                    setValueLocked(static_cast<ps2trainer::Id>(i), 1);
            if (all)
                return;
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

    // As the F2 menu: the controller drives the panel only when it is the active device.
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
}

namespace ps2trainer
{
    Id addSwitch(const char *label, const char *help)
    {
        if (!label || !*label)
        {
            std::fprintf(stderr, "[trainer] a switch needs a label\n");
            return -1;
        }
        Entry e;
        e.kind = Kind::Switch;
        e.label = label;
        e.help = help ? help : "";
        return add(std::move(e));
    }

    Id addChoice(const char *label, const char *const *options, int count, const char *help)
    {
        if (!label || !*label || !options || count < 2 || count > kMaxOptions)
        {
            std::fprintf(stderr, "[trainer] choice '%s' needs a label and 2..%d options\n", label ? label : "", kMaxOptions);
            return -1;
        }
        Entry e;
        e.kind = Kind::Choice;
        e.label = label;
        e.help = help ? help : "";
        for (int i = 0; i < count; ++i)
            e.options.emplace_back(options[i] && *options[i] ? options[i] : "?");
        return add(std::move(e));
    }

    void addSection(const char *title)
    {
        Entry e;
        e.kind = Kind::Section;
        e.label = title ? title : "";
        add(std::move(e));
    }

    bool on(Id id) noexcept
    {
        return validId(id) && s_values[static_cast<size_t>(id)].load(std::memory_order_relaxed) != 0;
    }

    int choice(Id id) noexcept
    {
        return validId(id) ? s_values[static_cast<size_t>(id)].load(std::memory_order_relaxed) : 0;
    }

    void set(Id id, bool value) noexcept
    {
        std::lock_guard<std::mutex> lock(registryMutex());
        const std::vector<Entry> &entries = registry();
        if (validId(id) && id < static_cast<int>(entries.size()) && entries[static_cast<size_t>(id)].kind == Kind::Switch)
            setValueLocked(id, value ? 1 : 0);
    }
}

void ps2TrainerSetHostPad(uint16_t buttons, bool gamepad)
{
    s_hostButtons.store(buttons, std::memory_order_relaxed);
    s_hostGamepad.store(gamepad, std::memory_order_relaxed);
}

bool ps2TrainerMenuOpen()
{
    return s_open.load(std::memory_order_relaxed);
}

void ps2TrainerHostTick()
{
    static bool started = false;
    static bool shown = false;
    static double menuAt = []() { const char *v = std::getenv("PS2_TRAINER_MENU_AT"); return v ? std::atof(v) : 0.0; }();
    if (!started)
    {
        started = true;
        size_t count = 0;
        {
            std::lock_guard<std::mutex> lock(registryMutex());
            count = registry().size();
        }
        std::fprintf(stderr, "[trainer] F3 trainer enabled in this build, %zu entries registered\n", count);
        applyTestEnv();
    }
    const bool autoOpen = menuAt > 0.0 && GetTime() >= menuAt;
    if (autoOpen)
        menuAt = 0.0;

    if (ps2CheatsMenuOpen())
    {
        // The F2 panel took the shared GS menu slot and this frame's menu input (its tick runs first): leave both.
        if (s_open.exchange(false, std::memory_order_relaxed) || shown)
        {
            shown = false;
            std::fprintf(stderr, "[trainer] panel closed (F2 panel opened)\n");
        }
        return;
    }
    if (IsKeyPressed(KEY_F3) || autoOpen)
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
        std::fprintf(stderr, "[trainer] panel %s\n", open ? "open" : "closed");
    }
    if (open)
        dodpcsx2::menuInput(menuKeys(), menuPad(), GetMouseWheelMove());
}
