#include "MiniTest.h"
#include <stdexcept>

int main(int argc, char **argv)
{
    const std::string mode = argc > 1 ? argv[1] : "pass";
    if (mode != "zero")
        MiniTest::Case(mode == "other-suite" ? "Other" : "Smoke\"\\\n測試", [mode](TestCase &tc) {
            for (int i = 0; i < 10; ++i)
                tc.Run("extra " + std::to_string(i), [](TestCase &t) { t.IsTrue(true, "extra pass"); });
            tc.Run("passing case", [](TestCase &t) { t.IsTrue(true, "pass"); });
            tc.Run(mode == "new" ? "new regression" : "known failure", [mode](TestCase &t) {
                if (mode == "error")
                    throw std::runtime_error("unexpected exception");
                if (mode != "pass")
                    t.Fail("controlled failure");
                std::cout << std::hex; // Structured summary must use decimal regardless of test stream state.
            });
        });
    return MiniTest::Run();
}
