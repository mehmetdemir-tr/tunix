#include <climits>
#include <ctime>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "framework.hpp"
#include "helpers.hpp"

using posix_tests::exit_code;
using posix_tests::TempDir;

TEST(io, a_pipe_carries_data_and_ends_with_eof) {
    int pipes[2];
    CHECK_OK(pipe(pipes));
    CHECK_EQ(write(pipes[1], "data", 4), 4L);
    close(pipes[1]);
    char buffer[8] = {};
    CHECK_EQ(read(pipes[0], buffer, sizeof buffer), 4L);
    CHECK_EQ(std::string(buffer), std::string("data"));
    CHECK_EQ(read(pipes[0], buffer, sizeof buffer), 0L);
    close(pipes[0]);
}

TEST(io, writing_to_a_pipe_without_readers_is_epipe) {
    signal(SIGPIPE, SIG_IGN);
    int pipes[2];
    CHECK_OK(pipe(pipes));
    close(pipes[0]);
    CHECK_ERRNO(write(pipes[1], "x", 1), EPIPE);
    close(pipes[1]);
}

TEST(io, sigpipe_kills_a_writer_by_default) {
    int pipes[2];
    int reader_gone[2];
    CHECK_OK(pipe(pipes));
    CHECK_OK(pipe(reader_gone));
    pid_t child = fork();
    if (child == 0) {
        signal(SIGPIPE, SIG_DFL);
        close(pipes[0]);
        close(reader_gone[1]);
        char byte;
        (void)!read(reader_gone[0], &byte, 1);
        (void)!write(pipes[1], "x", 1);
        _exit(0);
    }
    close(pipes[0]);
    close(pipes[1]);
    close(reader_gone[0]);
    close(reader_gone[1]);
    int status = posix_tests::wait_status(child);
    CHECK(WIFSIGNALED(status));
    CHECK_EQ(WTERMSIG(status), SIGPIPE);
}

TEST(io, an_empty_nonblocking_pipe_is_eagain) {
    int pipes[2];
    CHECK_OK(pipe2(pipes, O_NONBLOCK));
    char byte;
    CHECK_ERRNO(read(pipes[0], &byte, 1), EAGAIN);
}

TEST(io, a_pipe_holds_at_least_pipe_buf_bytes) {
    int pipes[2];
    CHECK_OK(pipe2(pipes, O_NONBLOCK));
    char block[512] = {};
    long total = 0;
    while (total < 1 << 22) {
        ssize_t count = write(pipes[1], block, sizeof block);
        if (count < 0) break;
        total += count;
    }
    CHECK_EQ(errno, EAGAIN);
    CHECK(total >= PIPE_BUF);
}

TEST(io, small_pipe_writes_are_atomic) {
    int pipes[2];
    CHECK_OK(pipe(pipes));
    std::vector<pid_t> writers;
    for (int w = 0; w < 4; w++) {
        pid_t child = fork();
        if (child == 0) {
            char record[64];
            std::memset(record, 'a' + w, sizeof record);
            for (int i = 0; i < 100; i++) (void)!write(pipes[1], record, sizeof record);
            _exit(0);
        }
        writers.push_back(child);
    }
    close(pipes[1]);
    char record[64];
    int records = 0;
    bool clean = true;
    while (true) {
        size_t got = 0;
        while (got < sizeof record) {
            ssize_t count = read(pipes[0], record + got, sizeof record - got);
            if (count <= 0) break;
            got += (size_t)count;
        }
        if (got == 0) break;
        for (char c : record) clean = clean && c == record[0];
        records++;
    }
    for (pid_t writer : writers) posix_tests::wait_status(writer);
    CHECK_EQ(records, 400);
    CHECK(clean);
}

TEST(io, poll_sees_data_and_hangup) {
    int pipes[2];
    CHECK_OK(pipe(pipes));
    struct pollfd entry = {pipes[0], POLLIN, 0};
    CHECK_EQ(poll(&entry, 1, 0), 0);
    CHECK_EQ(write(pipes[1], "x", 1), 1L);
    CHECK_EQ(poll(&entry, 1, 1000), 1);
    CHECK(entry.revents & POLLIN);
    char byte;
    CHECK_EQ(read(pipes[0], &byte, 1), 1L);
    close(pipes[1]);
    CHECK_EQ(poll(&entry, 1, 1000), 1);
    CHECK(entry.revents & POLLHUP);
}

TEST(io, poll_waits_for_its_timeout) {
    int pipes[2];
    CHECK_OK(pipe(pipes));
    struct pollfd entry = {pipes[0], POLLIN, 0};
    struct timespec before, after;
    clock_gettime(CLOCK_MONOTONIC, &before);
    CHECK_EQ(poll(&entry, 1, 100), 0);
    clock_gettime(CLOCK_MONOTONIC, &after);
    long elapsed =
        (after.tv_sec - before.tv_sec) * 1000 + (after.tv_nsec - before.tv_nsec) / 1000000;
    CHECK(elapsed >= 90);
    CHECK(elapsed < 2000);
}

TEST(io, select_reports_a_readable_pipe) {
    int pipes[2];
    CHECK_OK(pipe(pipes));
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(pipes[0], &readable);
    struct timeval none = {0, 0};
    CHECK_EQ(select(pipes[0] + 1, &readable, nullptr, nullptr, &none), 0);
    CHECK_EQ(write(pipes[1], "x", 1), 1L);
    FD_ZERO(&readable);
    FD_SET(pipes[0], &readable);
    struct timeval second = {1, 0};
    CHECK_EQ(select(pipes[0] + 1, &readable, nullptr, nullptr, &second), 1);
    CHECK(FD_ISSET(pipes[0], &readable));
}

TEST(io, epoll_reports_the_ready_descriptor) {
    int pipes[2];
    CHECK_OK(pipe(pipes));
    int epoll = epoll_create1(EPOLL_CLOEXEC);
    CHECK(epoll >= 0);
    struct epoll_event event = {};
    event.events = EPOLLIN;
    event.data.u64 = 77;
    CHECK_OK(epoll_ctl(epoll, EPOLL_CTL_ADD, pipes[0], &event));
    struct epoll_event ready[4] = {};
    CHECK_EQ(epoll_wait(epoll, ready, 4, 0), 0);
    CHECK_EQ(write(pipes[1], "x", 1), 1L);
    CHECK_EQ(epoll_wait(epoll, ready, 4, 1000), 1);
    CHECK_EQ(ready[0].data.u64, (uint64_t)77);
    close(epoll);
}

TEST(io, dup_shares_the_file_offset) {
    TempDir dir;
    int fd = open("data", O_RDWR | O_CREAT, 0644);
    CHECK_EQ(write(fd, "abcdef", 6), 6L);
    int copy = dup(fd);
    CHECK_EQ(lseek(fd, 1, SEEK_SET), 1L);
    char byte = 0;
    CHECK_EQ(read(copy, &byte, 1), 1L);
    CHECK_EQ(byte, 'b');
    CHECK_EQ(lseek(fd, 0, SEEK_CUR), 2L);
    CHECK_OK(dup2(fd, 20));
    CHECK_EQ(lseek(20, 0, SEEK_CUR), 2L);
    CHECK_EQ(fcntl(20, F_GETFD) & FD_CLOEXEC, 0);
    close(copy);
    close(fd);
}

TEST(io, fcntl_toggles_nonblocking_mode) {
    int pipes[2];
    CHECK_OK(pipe(pipes));
    int flags = fcntl(pipes[0], F_GETFL);
    CHECK_EQ(flags & O_NONBLOCK, 0);
    CHECK_OK(fcntl(pipes[0], F_SETFL, flags | O_NONBLOCK));
    char byte;
    CHECK_ERRNO(read(pipes[0], &byte, 1), EAGAIN);
    CHECK_EQ(fcntl(pipes[0], F_GETFL) & O_ACCMODE, O_RDONLY);
}

TEST(io, readv_and_writev_scatter_and_gather) {
    int pipes[2];
    CHECK_OK(pipe(pipes));
    char one[] = "abc", two[] = "defgh";
    struct iovec out[2] = {{one, 3}, {two, 5}};
    CHECK_EQ(writev(pipes[1], out, 2), 8L);
    char first[4] = {}, second[5] = {};
    struct iovec in[2] = {{first, 4}, {second, 4}};
    CHECK_EQ(readv(pipes[0], in, 2), 8L);
    CHECK_EQ(std::string(first, 4), std::string("abcd"));
    CHECK_EQ(std::string(second, 4), std::string("efgh"));
}

TEST(io, a_fifo_connects_two_processes) {
    TempDir dir;
    CHECK_OK(mkfifo("fifo", 0600));
    struct stat info = {};
    CHECK_OK(stat("fifo", &info));
    CHECK(S_ISFIFO(info.st_mode));
    pid_t child = fork();
    if (child == 0) {
        int fd = open("fifo", O_WRONLY);
        _exit(fd >= 0 && write(fd, "through", 7) == 7 ? 0 : 1);
    }
    int fd = open("fifo", O_RDONLY);
    CHECK(fd >= 0);
    char buffer[8] = {};
    size_t got = 0;
    while (got < 7) {
        ssize_t count = read(fd, buffer + got, 7 - got);
        if (count <= 0) break;
        got += (size_t)count;
    }
    CHECK_EQ(std::string(buffer), std::string("through"));
    CHECK_EQ(exit_code(child), 0);
    close(fd);
}

TEST(io, dev_null_and_dev_zero_behave) {
    int null = open("/dev/null", O_RDWR);
    int zero = open("/dev/zero", O_RDONLY);
    CHECK(null >= 0);
    CHECK(zero >= 0);
    char buffer[16];
    std::memset(buffer, 1, sizeof buffer);
    CHECK_EQ(write(null, buffer, sizeof buffer), 16L);
    CHECK_EQ(read(null, buffer, sizeof buffer), 0L);
    CHECK_EQ(read(zero, buffer, sizeof buffer), 16L);
    CHECK(buffer[0] == 0 && buffer[15] == 0);
    close(null);
    close(zero);
}
