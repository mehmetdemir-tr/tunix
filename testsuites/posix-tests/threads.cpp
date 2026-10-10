#include <atomic>
#include <condition_variable>
#include <csignal>
#include <mutex>
#include <pthread.h>
#include <set>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "framework.hpp"

namespace {

thread_local int per_thread = 0;
std::atomic<pid_t> signalled_thread{0};

void note_thread(int) { signalled_thread = gettid(); }

}

TEST(threads, create_and_join_return_a_value) {
    pthread_t thread;
    CHECK_EQ(pthread_create(
                 &thread, nullptr,
                 [](void *argument) -> void * {
                     return reinterpret_cast<void *>(reinterpret_cast<long>(argument) * 2);
                 },
                 reinterpret_cast<void *>(21L)),
             0);
    void *result = nullptr;
    CHECK_EQ(pthread_join(thread, &result), 0);
    CHECK_EQ(reinterpret_cast<long>(result), 42L);
}

TEST(threads, threads_share_the_pid_but_not_the_tid) {
    pid_t pid = getpid();
    pid_t main_tid = gettid();
    pid_t other_pid = 0;
    pid_t other_tid = 0;
    std::thread thread([&] {
        other_pid = getpid();
        other_tid = gettid();
    });
    thread.join();
    CHECK_EQ(main_tid, pid);
    CHECK_EQ(other_pid, pid);
    CHECK(other_tid != main_tid);
    CHECK(other_tid > 0);
}

TEST(threads, a_mutex_protects_a_shared_counter) {
    long counter = 0;
    std::mutex lock;
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; t++) {
        threads.emplace_back([&] {
            for (int i = 0; i < 20000; i++) {
                std::lock_guard<std::mutex> guard(lock);
                counter++;
            }
        });
    }
    for (auto &thread : threads) thread.join();
    CHECK_EQ(counter, 80000L);
}

TEST(threads, atomics_add_up_across_threads) {
    std::atomic<long> counter{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; t++) {
        threads.emplace_back([&] {
            for (int i = 0; i < 50000; i++) counter.fetch_add(1, std::memory_order_relaxed);
        });
    }
    for (auto &thread : threads) thread.join();
    CHECK_EQ(counter.load(), 200000L);
}

TEST(threads, condition_variables_hand_work_back_and_forth) {
    std::mutex lock;
    std::condition_variable changed;
    int turn = 0;
    std::thread other([&] {
        for (int round = 0; round < 100; round++) {
            std::unique_lock<std::mutex> guard(lock);
            changed.wait(guard, [&] { return turn % 2 == 1; });
            turn++;
            changed.notify_all();
        }
    });
    for (int round = 0; round < 100; round++) {
        std::unique_lock<std::mutex> guard(lock);
        changed.wait(guard, [&] { return turn % 2 == 0; });
        turn++;
        changed.notify_all();
    }
    other.join();
    CHECK_EQ(turn, 200);
}

TEST(threads, atomic_wait_sleeps_until_notified) {
    std::atomic<int> flag{0};
    std::thread waker([&] {
        usleep(50000);
        flag.store(1);
        flag.notify_one();
    });
    flag.wait(0);
    CHECK_EQ(flag.load(), 1);
    waker.join();
}

TEST(threads, thread_local_storage_is_per_thread) {
    per_thread = 1;
    int seen = -1;
    std::thread thread([&] {
        seen = per_thread;
        per_thread = 2;
    });
    thread.join();
    CHECK_EQ(seen, 0);
    CHECK_EQ(per_thread, 1);
}

TEST(threads, many_threads_run_at_once) {
    std::atomic<int> started{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < 32; t++) {
        threads.emplace_back([&] {
            started++;
            while (!go) std::this_thread::yield();
        });
    }
    while (started < 32) std::this_thread::yield();
    go = true;
    for (auto &thread : threads) thread.join();
    CHECK_EQ(started.load(), 32);
}

TEST(threads, pthread_kill_reaches_the_chosen_thread) {
    struct sigaction action = {};
    action.sa_handler = note_thread;
    sigemptyset(&action.sa_mask);
    sigaction(SIGUSR1, &action, nullptr);
    std::atomic<pid_t> target_tid{0};
    std::atomic<bool> done{false};
    std::thread target([&] {
        target_tid = gettid();
        while (!done) usleep(1000);
    });
    while (target_tid == 0) std::this_thread::yield();
    CHECK_EQ(pthread_kill(target.native_handle(), SIGUSR1), 0);
    for (int i = 0; i < 1000 && signalled_thread == 0; i++) usleep(1000);
    done = true;
    target.join();
    CHECK_EQ(signalled_thread.load(), target_tid.load());
}

TEST(threads, cancellation_stops_a_sleeping_thread) {
    pthread_t thread;
    pthread_create(
        &thread, nullptr,
        [](void *) -> void * {
            for (;;) sleep(1);
            return nullptr;
        },
        nullptr);
    usleep(50000);
    CHECK_EQ(pthread_cancel(thread), 0);
    void *result = nullptr;
    CHECK_EQ(pthread_join(thread, &result), 0);
    CHECK(result == PTHREAD_CANCELED);
}

TEST(threads, a_detached_thread_finishes_on_its_own) {
    static std::atomic<int> finished{0};
    pthread_t thread;
    pthread_create(
        &thread, nullptr,
        [](void *) -> void * {
            finished = 1;
            return nullptr;
        },
        nullptr);
    CHECK_EQ(pthread_detach(thread), 0);
    for (int i = 0; i < 1000 && finished == 0; i++) usleep(1000);
    CHECK_EQ(finished.load(), 1);
}

TEST(threads, fork_from_a_thread_copies_only_that_thread) {
    std::atomic<int> code{-1};
    std::thread forker([&] {
        pid_t child = fork();
        if (child == 0) _exit(0);
        int status = 0;
        waitpid(child, &status, 0);
        code = WIFEXITED(status) ? WEXITSTATUS(status) : 99;
    });
    forker.join();
    CHECK_EQ(code.load(), 0);
}
