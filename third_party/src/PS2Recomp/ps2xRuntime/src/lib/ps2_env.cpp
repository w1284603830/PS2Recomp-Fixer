// The runtime's opt-in switches and diagnostics grew up as DOD_* environment variables (the first port's name). Every
// one of them can also be set as PS2_<same name>: PS2_PAD_TRACE=1 is DOD_PAD_TRACE=1. ps2EnvAliasesInstall() copies the
// PS2_ ones to their DOD_ names once at start (a PS2_ value wins over a DOD_ one), so every getenv("DOD_...") in the
// runtime, the IOP and the GS sees them. Variables the runtime reads under their PS2_ name (PS2_CHEATS_FILE,
// PS2_GS_GPARAM...) are copied too; the extra DOD_ copy is harmless.
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
extern "C" __declspec(dllimport) char *__stdcall GetEnvironmentStringsA(void);
extern "C" __declspec(dllimport) int __stdcall FreeEnvironmentStringsA(char *block);
extern "C" __declspec(dllimport) int __stdcall SetEnvironmentVariableA(const char *name, const char *value);
#else
extern char **environ;
#endif

void ps2EnvAliasesInstall()
{
    static bool done = false;
    if (done)
        return;
    done = true;

    std::vector<std::pair<std::string, std::string>> aliases; // DOD_ name, value
    const auto consider = [&](const char *entry) {
        if (std::strncmp(entry, "PS2_", 4) != 0)
            return;
        const char *eq = std::strchr(entry, '=');
        if (!eq || eq == entry + 4)
            return;
        aliases.emplace_back(std::string("DOD_") + std::string(entry + 4, eq), std::string(eq + 1));
    };
#ifdef _WIN32
    if (char *block = GetEnvironmentStringsA())
    {
        for (const char *p = block; *p; p += std::strlen(p) + 1)
            consider(p);
        FreeEnvironmentStringsA(block);
    }
#else
    for (char **e = environ; e && *e; ++e)
        consider(*e);
#endif
    for (const auto &[name, value] : aliases)
    {
#ifdef _WIN32
        _putenv_s(name.c_str(), value.c_str());           // the CRT's copy (getenv)
        SetEnvironmentVariableA(name.c_str(), value.c_str()); // and the process's (another module's CRT)
#else
        setenv(name.c_str(), value.c_str(), 1);
#endif
    }
}
