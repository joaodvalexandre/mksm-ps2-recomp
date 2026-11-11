// A minimal test harness for vu1n_tests (standard library only, like the generator).
#ifndef MKSM_VU1N_TEST_H
#define MKSM_VU1N_TEST_H

#include <cstdint>
#include <sstream>
#include <string>
#include <type_traits>

namespace test
{
    void addCase(const char *name, void (*run)());
    void countCheck();
    void fail(const char *file, int line, const std::string &message);

    struct Registrar
    {
        Registrar(const char *name, void (*run)()) { addCase(name, run); }
    };

    // Integers print as numbers (uint8_t would print as a character), enums as their value.
    template <typename T>
    void print(std::ostringstream &out, const T &value)
    {
        if constexpr (std::is_enum_v<T>)
            out << static_cast<long long>(value);
        else if constexpr (std::is_same_v<T, bool>)
            out << (value ? "true" : "false");
        else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>)
            out << static_cast<long long>(value);
        else if constexpr (std::is_integral_v<T>)
            out << static_cast<unsigned long long>(value) << " (0x" << std::hex << static_cast<unsigned long long>(value) << std::dec << ")";
        else
            out << value;
    }

    template <typename A, typename B>
    void checkEqual(const A &actual, const B &expected, const char *actualText, const char *expectedText, const char *file, int line)
    {
        countCheck();
        bool equal = false;
        if constexpr (std::is_integral_v<A> && std::is_integral_v<B>)
            equal = static_cast<long long>(actual) == static_cast<long long>(expected);
        else
            equal = actual == expected;
        if (equal)
            return;
        std::ostringstream out;
        out << actualText << " is ";
        print(out, actual);
        out << ", expected " << expectedText << " = ";
        print(out, expected);
        fail(file, line, out.str());
    }
}

#define TEST_CASE(name)                                         \
    static void name();                                         \
    static const test::Registrar registrar_##name(#name, name); \
    static void name()

#define CHECK(condition)                                 \
    do                                                   \
    {                                                    \
        test::countCheck();                              \
        if (!(condition))                                \
            test::fail(__FILE__, __LINE__, #condition);  \
    } while (0)

#define CHECK_EQ(actual, expected) test::checkEqual((actual), (expected), #actual, #expected, __FILE__, __LINE__)

#endif
