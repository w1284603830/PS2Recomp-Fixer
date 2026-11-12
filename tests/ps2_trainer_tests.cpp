// Headless tests of the optional F3 trainer. The production implementation is included so its private panel and
// registry can be exercised against recording GS-menu services; nothing is rendered and no game is loaded.
#ifndef PS2_TRAINER_SOURCE
#define PS2_TRAINER_SOURCE "../third_party/src/PS2Recomp/ps2xRuntime/src/lib/ps2_trainer.cpp"
#endif
#include PS2_TRAINER_SOURCE

#include <cstdio>
#include <string>
#include <vector>

static bool g_cheatsOpen = false;
bool ps2CheatsMenuOpen() { return g_cheatsOpen; }

namespace dodpcsx2
{
    static void (*g_menu)(const Pcsx2GsUi *, void *) = nullptr;
    static int g_setMenuCalls = 0;
    void setMenu(void (*fn)(const Pcsx2GsUi *, void *), void *)
    {
        g_menu = fn;
        ++g_setMenuCalls;
    }
    void menuInput(uint32_t, uint32_t, float) {}
}

namespace
{
    struct Recorder
    {
        std::vector<std::string> checkboxes, combos, texts;
        std::string clickCheckbox, pickCombo;
        int pickValue = -1;
        bool clickAllOff = false;
        bool closeButton = false;
    } rec;

    int beginPanel(const char *, int *open, float, float, uint32_t)
    {
        if (rec.closeButton)
            *open = 0;
        return 1;
    }
    int beginWindow(const char *title, int *open, float w, float h) { return beginPanel(title, open, w, h, 0u); }
    void endWindow() {}
    void text(const char *t) { rec.texts.push_back(t); }
    int checkbox(const char *label, int *value)
    {
        rec.checkboxes.push_back(label);
        if (rec.clickCheckbox == label)
        {
            *value = !*value;
            return 1;
        }
        return 0;
    }
    int combo(const char *label, int *current, const char *const *items, int count)
    {
        rec.combos.push_back(std::string(label) + "=" + (*current >= 0 && *current < count ? items[*current] : "?"));
        if (rec.pickCombo == label)
        {
            *current = rec.pickValue;
            return 1;
        }
        return 0;
    }
    int button(const char *label) { return rec.clickAllOff && std::string(label) == "All off"; }
    void noArgs() {}
    void intArg(int) {}
    void textArg(const char *) {}

    Pcsx2GsUi makeUi()
    {
        Pcsx2GsUi ui{};
        ui.version = 3;
        ui.begin_window = beginWindow;
        ui.end_window = endWindow;
        ui.text = text;
        ui.text_wrapped = text;
        ui.text_disabled = text;
        ui.heading = text;
        ui.separator_text = text;
        ui.checkbox = checkbox;
        ui.combo = combo;
        ui.button = button;
        ui.spacing = noArgs;
        ui.push_id = intArg;
        ui.pop_id = noArgs;
        ui.begin_panel = beginPanel;
        ui.tooltip = textArg;
        return ui;
    }

    void cheatsPanel(const Pcsx2GsUi *, void *) {}

    void frame(int key)
    {
        g_stubPressedKey = key;
        ps2TrainerHostTick();
        g_stubPressedKey = 0;
    }
}

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } } while (false)

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    const std::string mode = argv[1];
    static const char *const multipliers[] = {"x1 (original)", "x2", "x4"};
    if (mode == "registry")
    {
        static_assert(ps2trainer::kEnabled, "this binary tests the enabled trainer");
        CHECK(ps2trainer::addSwitch(nullptr) == -1);
        CHECK(ps2trainer::addSwitch("") == -1);
        ps2trainer::addSection("Player");
        const ps2trainer::Id hp = ps2trainer::addSwitch("Lock HP", "help text");
        const ps2trainer::Id dmg = ps2trainer::addChoice("Damage", multipliers, 3);
        CHECK(hp == 1 && dmg == 2);
        CHECK(ps2trainer::addChoice("One option", multipliers, 1) == -1);
        CHECK(ps2trainer::addChoice("No options", nullptr, 3) == -1);
        CHECK(!ps2trainer::on(hp) && ps2trainer::choice(dmg) == 0); // every launch starts off / option 0
        ps2trainer::set(hp, true);
        CHECK(ps2trainer::on(hp));
        ps2trainer::set(dmg, true); // set() only changes switches
        CHECK(ps2trainer::choice(dmg) == 0);
        ps2trainer::set(0, true); // a section has no value
        CHECK(!ps2trainer::on(0));
        for (ps2trainer::Id bad : {-1, -100, 256, 100000})
        {
            CHECK(!ps2trainer::on(bad) && ps2trainer::choice(bad) == 0);
            ps2trainer::set(bad, true);
        }
        ps2trainer::set(hp, false);
        CHECK(!ps2trainer::on(hp));
    }
    else if (mode == "limit")
    {
        for (int i = 0; i < kMaxEntries; ++i)
            CHECK(ps2trainer::addSwitch("switch") == i);
        CHECK(ps2trainer::addSwitch("one too many") == -1);
        CHECK(!ps2trainer::on(kMaxEntries - 1));
    }
    else if (mode == "env-list" || mode == "env-all" || mode == "env-invalid")
    {
        // Python sets PS2_TRAINER; switch numbers count switches only.
        ps2trainer::addSection("Section");
        const ps2trainer::Id a = ps2trainer::addSwitch("A");
        const ps2trainer::Id choice = ps2trainer::addChoice("Choice", multipliers, 3);
        const ps2trainer::Id b = ps2trainer::addSwitch("B");
        frame(0);
        if (mode == "env-list")
            CHECK(!ps2trainer::on(a) && ps2trainer::on(b));
        else if (mode == "env-all")
            CHECK(ps2trainer::on(a) && ps2trainer::on(b));
        else
            CHECK(!ps2trainer::on(a) && !ps2trainer::on(b));
        CHECK(ps2trainer::choice(choice) == 0);
        CHECK(!ps2TrainerMenuOpen());
    }
    else if (mode == "keys")
    {
        ps2trainer::addSwitch("A");
        frame(0);
        CHECK(!ps2TrainerMenuOpen() && dodpcsx2::g_menu == nullptr);
        frame(KEY_F3);
        CHECK(ps2TrainerMenuOpen() && dodpcsx2::g_menu == drawMenu);
        frame(KEY_ESCAPE);
        CHECK(!ps2TrainerMenuOpen() && dodpcsx2::g_menu == nullptr);
        frame(KEY_F3);
        CHECK(ps2TrainerMenuOpen());
        frame(KEY_F3);
        CHECK(!ps2TrainerMenuOpen() && dodpcsx2::g_menu == nullptr);

        // The F2 panel owns the shared GS menu slot: F3 is ignored while it is open.
        g_cheatsOpen = true;
        frame(KEY_F3);
        CHECK(!ps2TrainerMenuOpen() && dodpcsx2::g_menu == nullptr);
        g_cheatsOpen = false;

        // F2 opened over an open trainer panel: the trainer closes without clearing the F2 panel's menu slot.
        frame(KEY_F3);
        CHECK(ps2TrainerMenuOpen());
        dodpcsx2::setMenu(cheatsPanel, nullptr); // what the F2 tick (which runs first) does
        g_cheatsOpen = true;
        const int calls = dodpcsx2::g_setMenuCalls;
        frame(0);
        CHECK(!ps2TrainerMenuOpen());
        CHECK(dodpcsx2::g_setMenuCalls == calls && dodpcsx2::g_menu == cheatsPanel);
        g_cheatsOpen = false;
        dodpcsx2::setMenu(nullptr, nullptr); // the F2 panel closes
        frame(0);
        CHECK(!ps2TrainerMenuOpen() && dodpcsx2::g_menu == nullptr);
    }
    else if (mode == "auto-open")
    {
        // Python sets PS2_TRAINER_MENU_AT=5.
        frame(0);
        CHECK(!ps2TrainerMenuOpen());
        g_stubTime = 6.0;
        frame(0);
        CHECK(ps2TrainerMenuOpen() && dodpcsx2::g_menu == drawMenu);
        frame(0);
        CHECK(ps2TrainerMenuOpen()); // opens once, does not toggle every frame
    }
    else if (mode == "draw")
    {
        const Pcsx2GsUi ui = makeUi();
        drawMenu(&ui, nullptr);
        CHECK(rec.checkboxes.empty() && rec.combos.empty());
        CHECK(!rec.texts.empty()); // the empty-registry message

        ps2trainer::addSection("Player");
        const ps2trainer::Id hp = ps2trainer::addSwitch("Lock HP");
        const ps2trainer::Id dmg = ps2trainer::addChoice("Damage", multipliers, 3);
        rec = Recorder{};
        drawMenu(&ui, nullptr);
        CHECK(rec.checkboxes.size() == 1 && rec.checkboxes[0] == "Lock HP");
        CHECK(rec.combos.size() == 1 && rec.combos[0] == "Damage=x1 (original)");

        rec = Recorder{};
        rec.clickCheckbox = "Lock HP";
        rec.pickCombo = "Damage";
        rec.pickValue = 2;
        drawMenu(&ui, nullptr);
        CHECK(ps2trainer::on(hp) && ps2trainer::choice(dmg) == 2);

        rec = Recorder{};
        rec.pickCombo = "Damage";
        rec.pickValue = 3; // out of range: ignored
        drawMenu(&ui, nullptr);
        CHECK(ps2trainer::choice(dmg) == 2);
        rec.pickValue = -1;
        drawMenu(&ui, nullptr);
        CHECK(ps2trainer::choice(dmg) == 2);

        rec = Recorder{};
        rec.clickAllOff = true;
        drawMenu(&ui, nullptr);
        CHECK(!ps2trainer::on(hp) && ps2trainer::choice(dmg) == 0);

        // The panel's close button closes it on the next host frame.
        frame(KEY_F3);
        CHECK(ps2TrainerMenuOpen());
        rec = Recorder{};
        rec.closeButton = true;
        drawMenu(&ui, nullptr);
        CHECK(!ps2TrainerMenuOpen());
        frame(0);
        CHECK(dodpcsx2::g_menu == nullptr);
    }
    else
        CHECK(!"unknown mode");
    return 0;
}
