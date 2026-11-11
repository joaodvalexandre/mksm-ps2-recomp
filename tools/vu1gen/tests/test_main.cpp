// vu1n_tests: unit tests of the generator's parts and of the runtime part of the translated programs.
//
//   vu1n_tests [substring]      runs the cases whose name contains the substring (all without one)
#include "test.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
    struct Case
    {
        const char *name;
        void (*run)();
    };

    std::vector<Case> &cases()
    {
        static std::vector<Case> list;
        return list;
    }

    unsigned g_checks = 0;
    unsigned g_failures = 0;
    const char *g_current = "";
}

namespace test
{
    void addCase(const char *name, void (*run)())
    {
        cases().push_back({name, run});
    }

    void countCheck()
    {
        ++g_checks;
    }

    void fail(const char *file, int line, const std::string &message)
    {
        ++g_failures;
        std::printf("FAILED %s (%s:%d): %s\n", g_current, file, line, message.c_str());
    }
}

int main(int argc, char **argv)
{
    const char *filter = argc > 1 ? argv[1] : "";
    unsigned ran = 0;
    unsigned failedCases = 0;
    for (const Case &testCase : cases())
    {
        if (std::strstr(testCase.name, filter) == nullptr)
            continue;
        const unsigned before = g_failures;
        g_current = testCase.name;
        testCase.run();
        ++ran;
        if (g_failures != before)
            ++failedCases;
    }
    std::printf("vu1n_tests: %u cases, %u checks, %u failed checks in %u cases\n", ran, g_checks, g_failures, failedCases);
    return g_failures == 0 && ran != 0 ? 0 : 1;
}
