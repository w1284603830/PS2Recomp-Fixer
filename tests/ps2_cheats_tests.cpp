// Include the production implementation so its private parser can be tested without exporting a test API.
#ifndef PS2_CHEATS_SOURCE
#define PS2_CHEATS_SOURCE "../third_party/src/PS2Recomp/ps2xRuntime/src/lib/ps2_cheats.cpp"
#endif
#include PS2_CHEATS_SOURCE

namespace dodpcsx2
{
    void setMenu(void (*)(const Pcsx2GsUi *, void *), void *) {}
    void menuInput(uint32_t, uint32_t, float) {}
}

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } } while (false)

int main(int argc, char **argv)
{
    CHECK(argc >= 2);
    Write w;
    std::string error;
    const std::string mode = argv[1];
    if (mode == "reject")
    {
        CHECK(argc == 3);
        CHECK(!parseWrite(argv[2], w, error));
        CHECK(!error.empty());
    }
    else if (mode == "decimal")
    {
        CHECK(parseWrite("0010,u16,0099,count=0010,stride=0008", w, error));
        CHECK(w.address == 10 && w.value == 99 && w.count == 10 && w.stride == 8);
    }
    else if (mode == "repeat")
    {
        CHECK(parseWrite("0x80001000,u32,0xFFFFFFFF,count=4294967295", w, error));
        CHECK(w.address == 0x1000 && w.value == UINT32_MAX && w.count == 1);
    }
    else if (mode == "bounds")
    {
        CHECK(parseWrite("0x21fffffc,u32,0x12345678", w, error));
        CHECK(w.address == kGuestRamSize - 4);
        CHECK(!parseWrite("0x1ffffff,u16,1", w, error));
        CHECK(!parseWrite("0,u32,1,count=4294967295,stride=4294967295", w, error));
        CHECK(parseWrite("0,u8,255,skip_zero,count=2,stride=8", w, error));
        CHECK(parseWrite("0,u32,1", w, error));
        CHECK(!w.skipZero && w.stride == 0 && w.count == 1);
    }
    else if (mode == "file")
    {
        loadFile(); // Python supplies PS2_CHEATS_FILE, including the documented inline comments.
        CHECK(s_entries.size() == 3);
        CHECK(s_entries[0].section && s_entries[0].label == "Player");
        CHECK(s_entries[1].label == "Health #1; reserve");
        CHECK(s_entries[1].help == "Keep # and ; in help.");
        CHECK(s_entries[1].note == "Keep # and ; in notes.");
        CHECK(s_entries[1].writes.size() == 1 && s_entries[2].writes.size() == 1);
        std::vector<uint8_t> ram(kGuestRamSize + 16, 0);
        ram[17] = 4;
        ram[21] = 5;
        setCheat(1, true);
        setCheat(2, true);
        applyWrites(ram.data());
        CHECK(ram[0] == 0 && ram[16] == 0 && ram[17] == 99 && ram[21] == 99);
        CHECK(ram[kGuestRamSize - 1] == 255 && ram[kGuestRamSize] == 0);
        setCheat(2, false);
        ram[17] = 6;
        applyWrites(ram.data());
        CHECK(ram[17] == 6);
    }
    else
        CHECK(false);
    return 0;
}
