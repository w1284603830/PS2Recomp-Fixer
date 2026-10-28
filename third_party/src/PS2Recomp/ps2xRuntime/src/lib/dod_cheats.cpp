// [DoD] In-game cheat menu (F2): the user's Action Replay codes for Shin Onimusha: Dawn of Dreams (SLPM-66275),
// decrypted with the AR1 key (work/cheats-20261028/ar_decrypt.py, decrypted.txt). Fourteen of them rewrite game
// instructions (a branch made a no-op or redirected, an add / load replaced); the recompiled functions do not read their
// instructions from RAM, so those are switches inside the generated code (g_dodCheats, added by
// tools/scripts/apply_dod_cheats.py at exactly those instructions; off = the original code). The item counts are RAM
// writes once per host frame, as an emulator applies a code. Everything starts off at each launch.
// The panel is the GS's ImGui menu (pcsx2gs_api.h set_menu): docked at the left, scaled with the image height, mouse
// read from Win32 in image pixels (dodpcsx2::menuInput), keyboard / controller navigation; the game gets a released pad
// while it is open.
#include "dod_cheats.h"

#include "gs/dod_pcsx2_gs.h"
#include "gs/pcsx2gs_api.h"
#include "ps2_host_backend.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" volatile unsigned char g_dodCheats[32] = {};

namespace
{
    struct Entry
    {
        int id;             // -1: a section title
        const char *label;  // UTF-8 (Simplified Chinese, as the game's text)
        const char *help;   // hover / focus help (may be null)
        const char *note;   // shown under the item (warnings)
    };

    const Entry kEntries[] = {
        {-1, "一闪", nullptr, nullptr},
        {kDodCheatChainCount, "连锁一闪次数无限制", "连锁一闪不再有次数上限。", nullptr},
        {kDodCheatChainRange, "连锁一闪：不限敌人距离",
         "连锁一闪可以接到任意距离的敌人。原作者建议搭配“一闪距离变更”（设为 100）使用，本菜单没有该项。", nullptr},
        {kDodCheatChainSameEnemy, "连锁一闪：可重复攻击同一敌人", "在幻魔空间里可能会碍事。", nullptr},
        {kDodCheatOtherIssenRange, "其他一闪：不限敌人距离", "通常一闪以外的一闪也能接到任意距离的敌人。", nullptr},
        {kDodCheatBassari, "一闪爽快感辅助", "原作者的“バッサリ感”辅助：建议与上面四项一起开启。", nullptr},
        {kDodCheatDeflectIssen, "R1 锁定后攻击即为弹一闪", "按住 R1 锁定敌人，再按攻击就是弹一闪。", nullptr},
        {-1, "能力槽", nullptr, nullptr},
        {kDodCheatSpirit, "心力常时回复", "心力一直回满。", nullptr},
        {kDodCheatOniPower, "鬼力常时回复", "鬼战术用完立刻回满。", nullptr},
        {kDodCheatOugi, "奥义随时可用", "奥义槽一直是满的。", nullptr},
        {-1, "道具与强化", nullptr, nullptr},
        {kDodCheatItems, "道具数量保持 99（前 10 格）", "已有道具的数量一直是 99；空格不会变出道具。", nullptr},
        {kDodCheatMoneyMax, "金钱变动时直接最大", "得到或花掉金钱时，金钱直接变为最大值。", nullptr},
        {kDodCheatKibushiMax, "鬼武侠点数变动时直接最大", "鬼武侠点数变动时直接变为最大值。", nullptr},
        {kDodCheatSoulsKeep, "强化武器时不消耗魂", "强化武器时，分配出去的魂不会减少。", nullptr},
        {kDodCheatUpgradeOnce, "武具强化：分配一次魂就升级", "武具强化时，分配一次魂就升一级。", nullptr},
        {kDodCheatSkill99, "技强化获得点数 99", "技强化时得到的点数固定为 99。", "原作者注明可能会黑屏，开启前请先存档。"},
    };

    std::atomic<bool> s_open{false};        // F2 / Esc on the window thread, the close button on the GS thread
    std::atomic<bool> s_justOpened{false};  // the next drawn frame gives the panel the focus
    std::atomic<uint16_t> s_hostButtons{0xFFFFu};
    std::atomic<bool> s_hostGamepad{false};

    const char *labelOf(int id)
    {
        for (const Entry &e : kEntries)
            if (e.id == id)
                return e.label;
        return "?";
    }

    void setCheat(int id, bool on)
    {
        if (id < 0 || id >= static_cast<int>(kDodCheatCount) || (g_dodCheats[id] != 0) == on)
            return;
        g_dodCheats[id] = on ? 1u : 0u;
        std::fprintf(stderr, "[cheats] %s: %s\n", labelOf(id), on ? "on" : "off");
    }

    // GS thread, at every present while the menu is shown.
    void drawMenu(const Pcsx2GsUi *ui, void *)
    {
        int open = 1;
        const char *title = "金手指###dodcheats";
        const uint32_t flags = PCSX2GS_PANEL_LEFT | (s_justOpened.exchange(false) ? PCSX2GS_PANEL_FOCUS : 0u);
        const bool panel = ui->version >= 3u && ui->begin_panel;
        const int visible = panel ? ui->begin_panel(title, &open, 440.0f, 860.0f, flags) : ui->begin_window(title, &open, 440.0f, 860.0f);
        if (visible)
        {
            ui->text_disabled("F2 或 Esc 关闭");
            ui->text_disabled("鼠标／方向键＋空格／手柄十字键＋A");
            for (const Entry &e : kEntries)
            {
                if (e.id < 0)
                {
                    ui->separator_text(e.label);
                    continue;
                }
                ui->push_id(e.id);
                int value = g_dodCheats[e.id] ? 1 : 0;
                if (ui->checkbox(e.label, &value))
                    setCheat(e.id, value != 0);
                if (panel && ui->tooltip && e.help)
                    ui->tooltip(e.help);
                if (e.note)
                    ui->text_colored(0xFF4A8CF5u, e.note); // orange (R in the low byte)
                ui->pop_id();
            }
            ui->spacing();
            if (ui->button("全部关闭"))
                for (int id = 0; id < static_cast<int>(kDodCheatCount); ++id)
                    setCheat(id, false);
            ui->text_wrapped("只在这次运行有效；重新启动游戏后全部恢复关闭。");
        }
        ui->end_window();
        if (!open)
            s_open.store(false, std::memory_order_relaxed);
    }

    // DOD_CHEATS=all | <index>,<index>... (tests): switches on at start; DOD_CHEAT_MENU_AT=<seconds>: opens the menu then.
    void applyTestEnv()
    {
        const char *v = std::getenv("DOD_CHEATS");
        if (!v || !*v)
            return;
        if (std::strcmp(v, "all") == 0)
        {
            for (int id = 0; id < static_cast<int>(kDodCheatCount); ++id)
                setCheat(id, true);
            return;
        }
        for (const char *p = v; *p;)
        {
            char *end = nullptr;
            const long id = std::strtol(p, &end, 10);
            if (end == p)
                break;
            setCheat(static_cast<int>(id), true);
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
}

void dodCheatsSetHostPad(uint16_t buttons, bool gamepad)
{
    s_hostButtons.store(buttons, std::memory_order_relaxed);
    s_hostGamepad.store(gamepad, std::memory_order_relaxed);
}

bool dodCheatsMenuOpen()
{
    return s_open.load(std::memory_order_relaxed);
}

void dodCheatsHostTick(uint8_t *rdram)
{
    static bool started = false;
    static bool shown = false;
    static double menuAt = []() { const char *v = std::getenv("DOD_CHEAT_MENU_AT"); return v ? std::atof(v) : 0.0; }();
    if (!started)
    {
        started = true;
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

    // RAM codes: the first ten item slots' counts (8-bit at 0x8D729C + 8 n; the code writes 0xC7 into all ten, this
    // keeps the 99 its title says and leaves empty slots empty).
    if (rdram && g_dodCheats[kDodCheatItems])
        for (uint32_t n = 0; n < 10u; ++n)
        {
            uint8_t &count = rdram[0x8D729Cu + 8u * n];
            if (count != 0u && count != 99u)
                count = 99u;
        }
}
