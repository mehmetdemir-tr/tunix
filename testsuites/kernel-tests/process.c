#define TEST_NAME "process"
#include "test.h"

#define CLONE_PIDFD           0x1000
#define CLONE_PARENT_SETTID   0x100000
#define SYS_pidfd_send_signal 424
#define SYS_clone3            435
#define EINVAL                22

static volatile int signals_seen;

static void count_signal(int number) {
    (void)number;
    signals_seen++;
}

static void fork_and_wait(void) {
    s64 parent = getpid();
    s64 child = fork();
    if (child == 0) exit(getppid() == parent ? 7 : 1);
    expect(child > 1, "fork hands the parent a new pid");
    int status = -1;
    expect_eq(waitpid(child, &status, 0), child, "waitpid reaps that child");
    expect_eq(exited_with(status), 7, "the child saw its parent and exited with 7");
    expect_eq(waitpid(-1, &status, WNOHANG), -ECHILD, "nothing is left to reap");
}

static void many_children(void) {
    enum { CHILDREN = 32 };
    for (int i = 0; i < CHILDREN; i++)
        if (fork() == 0) exit(i);
    int seen = 0;
    s64 sum = 0;
    int status;
    while (waitpid(-1, &status, 0) > 0) {
        seen++;
        sum += exited_with(status);
    }
    expect_eq(seen, CHILDREN, "32 children are all reaped");
    expect_eq(sum, CHILDREN * (CHILDREN - 1) / 2, "every exit status arrives intact");
}

static void exec_program(void) {
    s64 child = fork();
    if (child == 0) {
        char *argv[] = {"/sbin/init", "exec-child", 0};
        char *envp[] = {0};
        execve(argv[0], argv, envp);
        exit(99);
    }
    int status;
    waitpid(child, &status, 0);
    expect_eq(exited_with(status), 42, "execve replaces the child and passes argv");

    char *argv[] = {"/nonexistent", 0};
    char *envp[] = {0};
    expect_eq(execve(argv[0], argv, envp), -ENOENT, "execve of a missing file fails");
}

static void signals(void) {
    expect_eq(signal(SIGUSR1, (u64)count_signal), 0, "a SIGUSR1 handler installs");
    kill(getpid(), SIGUSR1);
    expect_eq(signals_seen, 1, "the handler runs once for a signal sent to itself");

    s64 child = fork();
    if (child == 0)
        for (;;) sleep_ms(1000);
    sleep_ms(20);
    int status = 0;
    expect_eq(waitpid(child, &status, WNOHANG), 0, "a sleeping child is still running");
    expect_eq(kill(child, SIGKILL), 0, "SIGKILL is delivered");
    expect_eq(waitpid(child, &status, 0), child, "the killed child is reaped");
    expect_eq(killed_by(status), SIGKILL, "and its status says SIGKILL");
}

static void clone_pidfd(void) {
    int pidfd = -1;
    s64 child = SYSCALL(SYS_clone, CLONE_PIDFD | SIGCHLD, 0, (s64)&pidfd, 0, 0, 0);
    if (child == 0)
        for (;;) sleep_ms(1000);
    expect(child > 1, "clone with CLONE_PIDFD forks");
    expect(pidfd > 2, "and stores a pidfd for the child");
    expect_eq(SYSCALL(SYS_pidfd_send_signal, pidfd, SIGKILL, 0, 0, 0, 0), 0,
              "the pidfd signals that child");
    int status = 0;
    expect_eq(waitpid(child, &status, 0), child, "the child is reaped");
    expect_eq(killed_by(status), SIGKILL, "and was killed through its pidfd");
    expect_eq(close(pidfd), 0, "the pidfd closes");
    expect_eq(
        SYSCALL(SYS_clone, CLONE_PIDFD | CLONE_PARENT_SETTID | SIGCHLD, 0, (s64)&pidfd, 0, 0, 0),
        -EINVAL, "clone refuses CLONE_PIDFD with CLONE_PARENT_SETTID");

    u64 args[11] = {CLONE_PIDFD, (u64)&pidfd, 0, 0, SIGCHLD};
    pidfd = -1;
    child = SYSCALL(SYS_clone3, (s64)args, sizeof args, 0, 0, 0, 0);
    if (child == 0) exit(9);
    expect(child > 1, "clone3 with CLONE_PIDFD forks");
    expect(pidfd > 2, "and stores a pidfd too");
    expect_eq(waitpid(child, &status, 0), child, "that child is reaped");
    expect_eq(exited_with(status), 9, "with its exit status");
    expect_eq(close(pidfd), 0, "and its pidfd closes");
}

static void run(int argc, char **argv) {
    if (argc > 1 && streq(argv[1], "exec-child")) exit(42);
    expect_eq(getpid(), 1, "the test runs as pid 1");
    fork_and_wait();
    many_children();
    exec_program();
    signals();
    clone_pidfd();
}
