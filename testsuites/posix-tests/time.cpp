#include <csignal>
#include <ctime>
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>

#include "framework.hpp"

namespace {

long elapsed_ms(const timespec &before, const timespec &after) {
    return (after.tv_sec - before.tv_sec) * 1000 + (after.tv_nsec - before.tv_nsec) / 1000000;
}

volatile sig_atomic_t fired;

void on_timer(int) { fired = fired + 1; }

}

TEST(time, the_monotonic_clock_never_goes_back) {
    timespec previous = {};
    clock_gettime(CLOCK_MONOTONIC, &previous);
    bool forward = true;
    for (int i = 0; i < 100000; i++) {
        timespec now = {};
        clock_gettime(CLOCK_MONOTONIC, &now);
        forward = forward &&
            (now.tv_sec > previous.tv_sec ||
             (now.tv_sec == previous.tv_sec && now.tv_nsec >= previous.tv_nsec));
        previous = now;
    }
    CHECK(forward);
}

TEST(time, the_realtime_clock_is_after_2024) {
    timespec now = {};
    CHECK_OK(clock_gettime(CLOCK_REALTIME, &now));
    CHECK(now.tv_sec > 1704067200);
    timeval tv = {};
    CHECK_OK(gettimeofday(&tv, nullptr));
    CHECK(tv.tv_sec - now.tv_sec <= 1);
}

TEST(time, nanosleep_sleeps_at_least_as_long_as_asked) {
    timespec before = {}, after = {};
    clock_gettime(CLOCK_MONOTONIC, &before);
    timespec request = {0, 100000000};
    CHECK_OK(nanosleep(&request, nullptr));
    clock_gettime(CLOCK_MONOTONIC, &after);
    CHECK(elapsed_ms(before, after) >= 100);
    CHECK(elapsed_ms(before, after) < 2000);
}

TEST(time, clock_nanosleep_waits_for_an_absolute_time) {
    timespec target = {};
    clock_gettime(CLOCK_MONOTONIC, &target);
    timespec start = target;
    target.tv_nsec += 150000000;
    if (target.tv_nsec >= 1000000000) {
        target.tv_sec++;
        target.tv_nsec -= 1000000000;
    }
    CHECK_EQ(clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &target, nullptr), 0);
    timespec now = {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    CHECK(elapsed_ms(start, now) >= 150);
}

TEST(time, clock_getres_reports_a_resolution) {
    timespec resolution = {};
    CHECK_OK(clock_getres(CLOCK_MONOTONIC, &resolution));
    CHECK(resolution.tv_sec == 0);
    CHECK(resolution.tv_nsec > 0);
}

TEST(time, a_posix_timer_fires_a_signal) {
    fired = 0;
    struct sigaction action = {};
    action.sa_handler = on_timer;
    sigemptyset(&action.sa_mask);
    sigaction(SIGUSR1, &action, nullptr);
    sigevent event = {};
    event.sigev_notify = SIGEV_SIGNAL;
    event.sigev_signo = SIGUSR1;
    timer_t timer = {};
    CHECK_OK(timer_create(CLOCK_MONOTONIC, &event, &timer));
    itimerspec spec = {};
    spec.it_value.tv_nsec = 50000000;
    spec.it_interval.tv_nsec = 50000000;
    CHECK_OK(timer_settime(timer, 0, &spec, nullptr));
    for (int i = 0; i < 200 && fired < 3; i++) usleep(10000);
    CHECK(fired >= 3);
    CHECK_OK(timer_delete(timer));
}

TEST(time, a_cpu_time_clock_advances_while_spinning) {
    timespec before = {}, after = {};
    CHECK_OK(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &before));
    volatile unsigned long sink = 0;
    for (unsigned long i = 0; i < 50000000UL; i++) sink = sink + i;
    CHECK_OK(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &after));
    CHECK(elapsed_ms(before, after) > 0 || after.tv_nsec > before.tv_nsec);
}

TEST(time, getrusage_counts_cpu_time) {
    volatile unsigned long sink = 0;
    for (unsigned long i = 0; i < 50000000UL; i++) sink = sink + i;
    rusage usage = {};
    CHECK_OK(getrusage(RUSAGE_SELF, &usage));
    CHECK(usage.ru_utime.tv_sec > 0 || usage.ru_utime.tv_usec > 0);
}

TEST(time, localtime_and_mktime_agree) {
    time_t now = time(nullptr);
    struct tm parts = {};
    CHECK(gmtime_r(&now, &parts) != nullptr);
    CHECK_EQ(timegm(&parts), now);
    struct tm epoch = {};
    epoch.tm_year = 70;
    epoch.tm_mday = 1;
    CHECK_EQ(timegm(&epoch), (time_t)0);
}
