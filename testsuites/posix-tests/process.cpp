#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include "framework.hpp"
#include "helpers.hpp"

extern char **environ;

using posix_tests::exit_code;
using posix_tests::spawn_helper;
using posix_tests::wait_status;

TEST(process, fork_returns_the_child_and_its_status) {
    pid_t child = fork();
    if (child == 0) _exit(7);
    CHECK(child > 0);
    int status = 0;
    CHECK_EQ(waitpid(child, &status, 0), child);
    CHECK(WIFEXITED(status));
    CHECK_EQ(WEXITSTATUS(status), 7);
}

TEST(process, child_sees_its_parent) {
    pid_t parent = getpid();
    pid_t child = fork();
    if (child == 0) _exit(getppid() == parent ? 0 : 1);
    CHECK_EQ(exit_code(child), 0);
}

TEST(process, wait_without_children_is_echild) {
    CHECK_ERRNO(waitpid(-1, nullptr, WNOHANG), ECHILD);
}

TEST(process, wnohang_returns_zero_while_the_child_runs) {
    int pipes[2];
    CHECK_OK(pipe(pipes));
    pid_t child = fork();
    if (child == 0) {
        char byte;
        close(pipes[1]);
        _exit(read(pipes[0], &byte, 1) == 0 ? 0 : 1);
    }
    close(pipes[0]);
    CHECK_EQ(waitpid(child, nullptr, WNOHANG), 0);
    close(pipes[1]);
    CHECK_EQ(exit_code(child), 0);
}

TEST(process, wait_reports_the_killing_signal) {
    pid_t child = fork();
    if (child == 0) {
        kill(getpid(), SIGTERM);
        _exit(0);
    }
    int status = wait_status(child);
    CHECK(WIFSIGNALED(status));
    CHECK_EQ(WTERMSIG(status), SIGTERM);
}

TEST(process, exec_runs_a_program_and_returns_its_status) {
    CHECK_EQ(exit_code(spawn_helper("exit", "42")), 42);
}

TEST(process, exec_passes_the_arguments) {
    CHECK_EQ(exit_code(spawn_helper("argc", "a", "b", "c")), 3);
}

TEST(process, exec_passes_the_environment) {
    CHECK_OK(setenv("TUNIX_POSIX_TEST", "yes", 1));
    CHECK_EQ(exit_code(spawn_helper("env", "TUNIX_POSIX_TEST", "yes")), 0);
}

TEST(process, close_on_exec_closes_only_marked_descriptors) {
    int marked = open("/dev/null", O_RDONLY | O_CLOEXEC);
    int plain = open("/dev/null", O_RDONLY);
    CHECK(marked >= 0);
    CHECK(plain >= 0);
    CHECK_EQ(exit_code(spawn_helper("fd-open", std::to_string(marked).c_str())), 1);
    CHECK_EQ(exit_code(spawn_helper("fd-open", std::to_string(plain).c_str())), 0);
}

TEST(process, exec_of_a_missing_file_is_enoent) {
    pid_t child = fork();
    if (child == 0) {
        execl("/no/such/program", "program", (char *)nullptr);
        _exit(errno == ENOENT ? 0 : 1);
    }
    CHECK_EQ(exit_code(child), 0);
}

TEST(process, posix_spawn_runs_a_program) {
    pid_t child = 0;
    const char *self = posix_tests::self_path().c_str();
    char *argv[] = {const_cast<char *>(self), const_cast<char *>("--helper"),
                    const_cast<char *>("exit"), const_cast<char *>("5"), nullptr};
    CHECK_EQ(posix_spawn(&child, self, nullptr, nullptr, argv, environ), 0);
    CHECK_EQ(exit_code(child), 5);
}

TEST(process, setsid_makes_a_new_session_and_group) {
    pid_t child = fork();
    if (child == 0) {
        pid_t session = setsid();
        bool ok = session == getpid() && getpgrp() == getpid() && getsid(0) == getpid();
        _exit(ok ? 0 : 1);
    }
    CHECK_EQ(exit_code(child), 0);
}

TEST(process, setpgid_moves_a_child_into_its_own_group) {
    int pipes[2];
    CHECK_OK(pipe(pipes));
    pid_t child = fork();
    if (child == 0) {
        char byte;
        close(pipes[1]);
        _exit(read(pipes[0], &byte, 1) == 1 && getpgrp() == getpid() ? 0 : 1);
    }
    close(pipes[0]);
    CHECK_OK(setpgid(child, child));
    CHECK_EQ(getpgid(child), child);
    CHECK_EQ(write(pipes[1], "x", 1), 1);
    CHECK_EQ(exit_code(child), 0);
}

TEST(process, kill_of_a_group_reaches_every_member) {
    int ready[2];
    CHECK_OK(pipe(ready));
    pid_t leader = fork();
    if (leader == 0) {
        setpgid(0, 0);
        signal(SIGUSR1, SIG_IGN);
        pid_t member = fork();
        if (member == 0) {
            signal(SIGUSR1, SIG_DFL);
            (void)!write(ready[1], "x", 1);
            for (;;) pause();
        }
        int status = wait_status(member);
        _exit(WIFSIGNALED(status) && WTERMSIG(status) == SIGUSR1 ? 0 : 1);
    }
    setpgid(leader, leader);
    close(ready[1]);
    char byte = 0;
    CHECK_EQ(read(ready[0], &byte, 1), 1);
    CHECK_OK(kill(-leader, SIGUSR1));
    CHECK_EQ(exit_code(leader), 0);
}

TEST(process, file_descriptor_limit_is_enforced) {
    struct rlimit limit = {};
    CHECK_OK(getrlimit(RLIMIT_NOFILE, &limit));
    limit.rlim_cur = 16;
    CHECK_OK(setrlimit(RLIMIT_NOFILE, &limit));
    int last = 0;
    for (int i = 0; i < 64; i++) {
        int fd = dup(0);
        if (fd == -1) break;
        last = fd;
    }
    CHECK(last < 16);
    CHECK_ERRNO(dup(0), EMFILE);
}

TEST(process, proc_self_follows_a_change_of_user) {
    if (geteuid() != 0) return;
    pid_t child = fork();
    if (child == 0) {
        if (setgid(65534) != 0 || setuid(65534) != 0) _exit(1);
        int fd = open("/dev/null", O_RDONLY);
        char path[64];
        std::snprintf(path, sizeof path, "/proc/self/fdinfo/%d", fd);
        int info = open(path, O_RDONLY);
        if (info < 0) _exit(2);
        char text[256];
        _exit(read(info, text, sizeof text) > 0 ? 0 : 3);
    }
    CHECK_EQ(exit_code(child), 0);
}

TEST(process, uname_describes_the_system) {
    struct utsname name = {};
    CHECK_OK(uname(&name));
    CHECK(name.sysname[0] != '\0');
    CHECK(name.release[0] != '\0');
    CHECK(name.machine[0] != '\0');
}

TEST(process, exit_handlers_run_on_exit) {
    int pipes[2];
    CHECK_OK(pipe(pipes));
    pid_t child = fork();
    if (child == 0) {
        static int fd;
        fd = pipes[1];
        std::atexit([] { (void)!write(fd, "x", 1); });
        std::exit(0);
    }
    close(pipes[1]);
    char byte = 0;
    CHECK_EQ(read(pipes[0], &byte, 1), 1);
    CHECK_EQ(byte, 'x');
    CHECK_EQ(exit_code(child), 0);
}
