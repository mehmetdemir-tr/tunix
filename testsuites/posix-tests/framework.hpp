#pragma once

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>

namespace posix_tests {

struct Test {
    const char *suite;
    const char *name;
    void (*body)();
};

struct Registry {
    Test tests[512];
    int count;
    const Test *begin() const { return tests; }
    const Test *end() const { return tests + count; }
};

inline Registry &registry() {
    static Registry tests;
    return tests;
}

struct Registrar {
    Registrar(const char *suite, const char *name, void (*body)()) noexcept {
        Registry &all = registry();
        if (all.count < 512) all.tests[all.count++] = {suite, name, body};
    }
};

inline int &failures() {
    static int count = 0;
    return count;
}

inline void fail(const char *file, int line, const std::string &what) {
    std::printf("    %s:%d: %s\n", file, line, what.c_str());
    std::fflush(stdout);
    failures()++;
}

template <typename T> std::string show(const T &value) {
    if constexpr (std::is_convertible_v<T, std::string>) {
        return "\"" + std::string(value) + "\"";
    } else if constexpr (std::is_pointer_v<T>) {
        char text[32];
        std::snprintf(text, sizeof text, "%p", static_cast<const void *>(value));
        return text;
    } else {
        return std::to_string(value);
    }
}

}

#define POSIX_CONCAT2(a, b) a##b
#define POSIX_CONCAT(a, b)  POSIX_CONCAT2(a, b)

#define TEST(suite, name) \
    static void POSIX_CONCAT(test_, POSIX_CONCAT(suite, POSIX_CONCAT(_, name)))(); \
    static posix_tests::Registrar POSIX_CONCAT(registrar_, POSIX_CONCAT(suite, name))( \
        #suite, #name, POSIX_CONCAT(test_, POSIX_CONCAT(suite, POSIX_CONCAT(_, name)))); \
    static void POSIX_CONCAT(test_, POSIX_CONCAT(suite, POSIX_CONCAT(_, name)))()

#define CHECK(condition) \
    do { \
        if (!(condition)) posix_tests::fail(__FILE__, __LINE__, "CHECK(" #condition ")"); \
    } while (0)

#define CHECK_EQ(actual, wanted) \
    do { \
        auto posix_actual = (actual); \
        auto posix_wanted = (wanted); \
        if (!(posix_actual == posix_wanted)) \
            posix_tests::fail(__FILE__, __LINE__, \
                              #actual " is " + posix_tests::show(posix_actual) + ", wanted " + \
                                  posix_tests::show(posix_wanted)); \
    } while (0)

#define CHECK_OK(call) \
    do { \
        errno = 0; \
        if ((call) == -1) \
            posix_tests::fail(__FILE__, __LINE__, \
                              #call " failed: " + std::string(std::strerror(errno))); \
    } while (0)

#define CHECK_ERRNO(call, wanted) \
    do { \
        errno = 0; \
        long posix_result = (long)(call); \
        int posix_errno = errno; \
        if (posix_result != -1 || posix_errno != (wanted)) \
            posix_tests::fail(__FILE__, __LINE__, \
                              #call " gave " + std::to_string(posix_result) + " (" + \
                                  std::strerror(posix_errno) + "), wanted -1 (" + \
                                  std::strerror(wanted) + ")"); \
    } while (0)
