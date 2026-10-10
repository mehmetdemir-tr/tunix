#include <atomic>
#include <csignal>
#include <ctime>
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>

#include "framework.hpp"
#include "helpers.hpp"

using posix_tests::wait_status;

namespace {

volatile sig_atomic_t delivered;
volatile sig_atomic_t queued_value;
volatile sig_atomic_t queued_code;

void count_signal(int) { delivered = delivered + 1; }

void record_info(int, siginfo_t *info, void *) {
    delivered = delivered + 1;
    queued_value = info->si_value.sival_int;
    queued_code = info->si_code;
}

void install(int number, void (*handler)(int), int flags = 0) {
    struct sigaction action = {};
    action.sa_handler = handler;
    action.sa_flags = flags;
    sigemptyset(&action.sa_mask);
    sigaction(number, &action, nullptr);
}

void block(int number, int how) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, number);
    sigprocmask(how, &set, nullptr);
}

}

TEST(signals, a_handler_runs_when_the_signal_arrives) {
    delivered = 0;
    install(SIGUSR1, count_signal);
    CHECK_OK(raise(SIGUSR1));
    CHECK_EQ((int)delivered, 1);
}

TEST(signals, a_blocked_signal_stays_pending_until_unblocked) {
    delivered = 0;
    install(SIGUSR1, count_signal);
    block(SIGUSR1, SIG_BLOCK);
    CHECK_OK(raise(SIGUSR1));
    CHECK_EQ((int)delivered, 0);
    sigset_t pending;
    CHECK_OK(sigpending(&pending));
    CHECK_EQ(sigismember(&pending, SIGUSR1), 1);
    block(SIGUSR1, SIG_UNBLOCK);
    CHECK_EQ((int)delivered, 1);
}

TEST(signals, standard_signals_do_not_queue) {
    delivered = 0;
    install(SIGUSR1, count_signal);
    block(SIGUSR1, SIG_BLOCK);
    raise(SIGUSR1);
    raise(SIGUSR1);
    raise(SIGUSR1);
    block(SIGUSR1, SIG_UNBLOCK);
    CHECK_EQ((int)delivered, 1);
}

TEST(signals, realtime_signals_queue_every_instance) {
    delivered = 0;
    int number = SIGRTMIN + 1;
    install(number, count_signal);
    block(number, SIG_BLOCK);
    union sigval value = {};
    for (int i = 0; i < 3; i++) CHECK_OK(sigqueue(getpid(), number, value));
    block(number, SIG_UNBLOCK);
    CHECK_EQ((int)delivered, 3);
}

TEST(signals, sigqueue_carries_a_value_to_siginfo) {
    delivered = 0;
    struct sigaction action = {};
    action.sa_sigaction = record_info;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    CHECK_OK(sigaction(SIGUSR2, &action, nullptr));
    union sigval value = {};
    value.sival_int = 1234;
    CHECK_OK(sigqueue(getpid(), SIGUSR2, value));
    CHECK_EQ((int)delivered, 1);
    CHECK_EQ((int)queued_value, 1234);
    CHECK_EQ((int)queued_code, SI_QUEUE);
}

TEST(signals, sigtimedwait_takes_a_pending_sigchld) {
    block(SIGCHLD, SIG_BLOCK);
    pid_t child = fork();
    if (child == 0) _exit(0);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGCHLD);
    struct timespec timeout = {5, 0};
    siginfo_t info = {};
    CHECK_EQ(sigtimedwait(&set, &info, &timeout), SIGCHLD);
    CHECK_EQ(info.si_pid, child);
    CHECK_EQ(info.si_code, CLD_EXITED);
    wait_status(child);
}

TEST(signals, sigtimedwait_times_out) {
    block(SIGUSR1, SIG_BLOCK);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    struct timespec timeout = {0, 50000000};
    CHECK_ERRNO(sigtimedwait(&set, nullptr, &timeout), EAGAIN);
}

TEST(signals, sigsuspend_waits_for_a_signal) {
    delivered = 0;
    install(SIGUSR1, count_signal);
    block(SIGUSR1, SIG_BLOCK);
    pid_t parent = getpid();
    pid_t child = fork();
    if (child == 0) {
        usleep(50000);
        kill(parent, SIGUSR1);
        _exit(0);
    }
    sigset_t open;
    sigemptyset(&open);
    CHECK_ERRNO(sigsuspend(&open), EINTR);
    CHECK_EQ((int)delivered, 1);
    wait_status(child);
}

TEST(signals, a_signal_interrupts_a_blocking_read) {
    delivered = 0;
    install(SIGUSR1, count_signal);
    int pipes[2];
    CHECK_OK(pipe(pipes));
    pthread_t self = pthread_self();
    pthread_t poker;
    pthread_create(
        &poker, nullptr,
        [](void *target) -> void * {
            usleep(100000);
            pthread_kill(*static_cast<pthread_t *>(target), SIGUSR1);
            return nullptr;
        },
        &self);
    char byte;
    CHECK_ERRNO(read(pipes[0], &byte, 1), EINTR);
    CHECK_EQ((int)delivered, 1);
    pthread_join(poker, nullptr);
}

TEST(signals, sa_restart_resumes_a_blocking_read) {
    delivered = 0;
    install(SIGUSR1, count_signal, SA_RESTART);
    static int pipes[2];
    CHECK_OK(pipe(pipes));
    pthread_t self = pthread_self();
    pthread_t poker;
    pthread_create(
        &poker, nullptr,
        [](void *target) -> void * {
            usleep(100000);
            pthread_kill(*static_cast<pthread_t *>(target), SIGUSR1);
            usleep(100000);
            (void)!write(pipes[1], "x", 1);
            return nullptr;
        },
        &self);
    char byte = 0;
    CHECK_EQ(read(pipes[0], &byte, 1), 1);
    CHECK_EQ(byte, 'x');
    CHECK_EQ((int)delivered, 1);
    pthread_join(poker, nullptr);
}

TEST(signals, the_default_action_kills_another_process) {
    pid_t child = fork();
    if (child == 0) {
        for (;;) pause();
    }
    CHECK_OK(kill(child, SIGUSR2));
    int status = wait_status(child);
    CHECK(WIFSIGNALED(status));
    CHECK_EQ(WTERMSIG(status), SIGUSR2);
}

TEST(signals, an_ignored_signal_is_discarded) {
    pid_t child = fork();
    if (child == 0) {
        signal(SIGUSR1, SIG_IGN);
        raise(SIGUSR1);
        _exit(3);
    }
    int status = wait_status(child);
    CHECK(WIFEXITED(status));
    CHECK_EQ(WEXITSTATUS(status), 3);
}

TEST(signals, abort_ends_the_process_with_sigabrt) {
    pid_t child = fork();
    if (child == 0) abort();
    int status = wait_status(child);
    CHECK(WIFSIGNALED(status));
    CHECK_EQ(WTERMSIG(status), SIGABRT);
}

TEST(signals, the_mask_is_inherited_across_fork) {
    block(SIGUSR1, SIG_BLOCK);
    pid_t child = fork();
    if (child == 0) {
        sigset_t mask;
        sigprocmask(SIG_BLOCK, nullptr, &mask);
        _exit(sigismember(&mask, SIGUSR1) == 1 ? 0 : 1);
    }
    CHECK_EQ(posix_tests::exit_code(child), 0);
}

TEST(signals, handlers_survive_fork_but_pending_signals_do_not) {
    delivered = 0;
    install(SIGUSR1, count_signal);
    block(SIGUSR1, SIG_BLOCK);
    raise(SIGUSR1);
    pid_t child = fork();
    if (child == 0) {
        sigset_t pending;
        sigpending(&pending);
        struct sigaction current = {};
        sigaction(SIGUSR1, nullptr, &current);
        bool ok = sigismember(&pending, SIGUSR1) == 0 && current.sa_handler == count_signal;
        _exit(ok ? 0 : 1);
    }
    CHECK_EQ(posix_tests::exit_code(child), 0);
    block(SIGUSR1, SIG_UNBLOCK);
    CHECK_EQ((int)delivered, 1);
}
