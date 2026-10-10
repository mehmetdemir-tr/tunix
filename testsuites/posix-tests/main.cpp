#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "framework.hpp"
#include "helpers.hpp"

namespace {

constexpr unsigned test_seconds = 20;

bool run_one(const posix_tests::Test &test) {
    std::printf("  %s.%s\n", test.suite, test.name);
    std::fflush(stdout);
    pid_t child = fork();
    if (child == 0) {
        alarm(test_seconds);
        test.body();
        std::fflush(stdout);
        _exit(posix_tests::failures() ? 1 : 0);
    }
    if (child < 0) {
        std::printf("    fork failed: %s\n", std::strerror(errno));
        return false;
    }
    int status = 0;
    while (waitpid(child, &status, 0) == -1 && errno == EINTR) {}
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return true;
    if (WIFSIGNALED(status)) {
        std::printf("    killed by %s%s\n", strsignal(WTERMSIG(status)),
                    WTERMSIG(status) == SIGALRM ? " (timed out)" : "");
    }
    std::printf("  FAIL %s.%s\n", test.suite, test.name);
    return false;
}

void prepare_machine() {
    mkdir("/dev", 0755);
    mkdir("/proc", 0755);
    mkdir("/tmp", 01777);
    if (access("/dev/null", F_OK) != 0) mount("devtmpfs", "/dev", "devtmpfs", 0, nullptr);
    mkdir("/dev/pts", 0755);
    if (access("/dev/pts/ptmx", F_OK) != 0) mount("devpts", "/dev/pts", "devpts", 0, nullptr);
    if (access("/proc/self", F_OK) != 0) mount("proc", "/proc", "proc", 0, nullptr);
    setenv("PATH", "/sbin:/bin", 1);
    setenv("HOME", "/tmp", 1);
    chdir("/tmp");
}

}

int main(int argc, char **argv) {
    posix_tests::self_path() = argv[0];
    if (argc > 1 && std::strcmp(argv[1], "--helper") == 0)
        return posix_tests::run_helper(argc, argv);

    bool init = getpid() == 1;
    if (init) prepare_machine();
    setvbuf(stdout, nullptr, _IOLBF, 0);

    const char *only = argc > 1 ? argv[1] : nullptr;
    int run = 0;
    int passed = 0;
    for (const auto &test : posix_tests::registry()) {
        if (only && std::strcmp(only, test.suite) != 0) continue;
        run++;
        if (run_one(test)) passed++;
    }
    std::printf("posix: %s %d/%d\n", passed == run ? "PASS" : "FAIL", passed, run);
    std::fflush(stdout);
    if (init) {
        sync();
        reboot(RB_POWER_OFF);
    }
    return passed == run ? 0 : 1;
}
