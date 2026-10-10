#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "framework.hpp"
#include "helpers.hpp"

using posix_tests::exit_code;

namespace {

struct Pty {
    int master = -1;
    int slave = -1;

    Pty() {
        master = posix_openpt(O_RDWR | O_NOCTTY);
        if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) return;
        const char *name = ptsname(master);
        if (name) slave = open(name, O_RDWR | O_NOCTTY);
    }
    ~Pty() {
        if (slave >= 0) close(slave);
        if (master >= 0) close(master);
    }
    Pty(const Pty &) = delete;
    Pty &operator=(const Pty &) = delete;
    bool ok() const { return master >= 0 && slave >= 0; }
};

std::string read_some(int fd, size_t wanted, int timeout_ms = 2000) {
    std::string text;
    while (text.size() < wanted) {
        struct pollfd entry = {fd, POLLIN, 0};
        if (poll(&entry, 1, timeout_ms) != 1) break;
        char buffer[256];
        ssize_t count = read(fd, buffer, sizeof buffer);
        if (count <= 0) break;
        text.append(buffer, (size_t)count);
    }
    return text;
}

}

TEST(pty, posix_openpt_gives_a_master_and_a_slave) {
    Pty pty;
    CHECK(pty.ok());
    CHECK_EQ(isatty(pty.slave), 1);
    CHECK_EQ(isatty(pty.master), 1);
    const char *name = ptsname(pty.master);
    CHECK(name != nullptr && std::strncmp(name, "/dev/pts/", 9) == 0);
    int pipes[2];
    CHECK_OK(pipe(pipes));
    CHECK_EQ(isatty(pipes[0]), 0);
}

TEST(pty, canonical_mode_delivers_whole_lines) {
    Pty pty;
    CHECK(pty.ok());
    CHECK_EQ(write(pty.master, "partial", 7), 7L);
    struct pollfd entry = {pty.slave, POLLIN, 0};
    CHECK_EQ(poll(&entry, 1, 100), 0);
    CHECK_EQ(write(pty.master, " line\n", 6), 6L);
    CHECK_EQ(read_some(pty.slave, 13), std::string("partial line\n"));
}

TEST(pty, echo_returns_typed_text_to_the_master) {
    Pty pty;
    CHECK(pty.ok());
    CHECK_EQ(write(pty.master, "hi\n", 3), 3L);
    CHECK_EQ(read_some(pty.master, 4), std::string("hi\r\n"));
}

TEST(pty, output_processing_turns_newlines_into_crlf) {
    Pty pty;
    CHECK(pty.ok());
    CHECK_EQ(write(pty.slave, "a\nb", 3), 3L);
    CHECK_EQ(read_some(pty.master, 4), std::string("a\r\nb"));
}

TEST(pty, raw_mode_delivers_single_bytes_without_echo) {
    Pty pty;
    CHECK(pty.ok());
    termios settings = {};
    CHECK_OK(tcgetattr(pty.slave, &settings));
    cfmakeraw(&settings);
    settings.c_cc[VMIN] = 1;
    settings.c_cc[VTIME] = 0;
    CHECK_OK(tcsetattr(pty.slave, TCSANOW, &settings));
    CHECK_EQ(write(pty.master, "x", 1), 1L);
    CHECK_EQ(read_some(pty.slave, 1), std::string("x"));
    struct pollfd entry = {pty.master, POLLIN, 0};
    CHECK_EQ(poll(&entry, 1, 100), 0);
}

TEST(pty, erase_removes_the_last_character) {
    Pty pty;
    CHECK(pty.ok());
    termios settings = {};
    CHECK_OK(tcgetattr(pty.slave, &settings));
    char erase = (char)settings.c_cc[VERASE];
    std::string typed = std::string("abx") + erase + "c\n";
    CHECK_EQ(write(pty.master, typed.data(), typed.size()), (ssize_t)typed.size());
    CHECK_EQ(read_some(pty.slave, 4), std::string("abc\n"));
}

TEST(pty, the_window_size_round_trips) {
    Pty pty;
    CHECK(pty.ok());
    winsize size = {};
    size.ws_row = 40;
    size.ws_col = 132;
    CHECK_OK(ioctl(pty.master, TIOCSWINSZ, &size));
    winsize back = {};
    CHECK_OK(ioctl(pty.slave, TIOCGWINSZ, &back));
    CHECK_EQ(back.ws_row, (unsigned short)40);
    CHECK_EQ(back.ws_col, (unsigned short)132);
}

TEST(pty, a_session_leader_takes_the_slave_as_its_terminal) {
    Pty pty;
    CHECK(pty.ok());
    const char *name = ptsname(pty.master);
    std::string path = name ? name : "";
    pid_t child = fork();
    if (child == 0) {
        setsid();
        int terminal = open(path.c_str(), O_RDWR);
        if (terminal < 0) _exit(1);
        if (ioctl(terminal, TIOCSCTTY, 0) != 0) _exit(2);
        if (tcgetpgrp(terminal) != getpgrp()) _exit(3);
        if (tcgetsid(terminal) != getsid(0)) _exit(4);
        int tty = open("/dev/tty", O_RDWR);
        _exit(tty >= 0 ? 0 : 5);
    }
    CHECK_EQ(exit_code(child), 0);
}

TEST(pty, ctrl_c_sends_sigint_to_the_foreground_group) {
    Pty pty;
    CHECK(pty.ok());
    const char *name = ptsname(pty.master);
    std::string path = name ? name : "";
    int ready[2];
    CHECK_OK(pipe(ready));
    pid_t child = fork();
    if (child == 0) {
        setsid();
        int terminal = open(path.c_str(), O_RDWR);
        ioctl(terminal, TIOCSCTTY, 0);
        signal(SIGINT, SIG_DFL);
        (void)!write(ready[1], "x", 1);
        for (;;) pause();
    }
    close(ready[1]);
    char byte;
    CHECK_EQ(read(ready[0], &byte, 1), 1L);
    termios settings = {};
    tcgetattr(pty.master, &settings);
    char interrupt = (char)settings.c_cc[VINTR];
    CHECK_EQ(write(pty.master, &interrupt, 1), 1L);
    int status = posix_tests::wait_status(child);
    CHECK(WIFSIGNALED(status));
    CHECK_EQ(WTERMSIG(status), SIGINT);
}

TEST(pty, closing_the_master_hangs_up_the_slave) {
    Pty pty;
    CHECK(pty.ok());
    close(pty.master);
    pty.master = -1;
    char byte;
    ssize_t result = read(pty.slave, &byte, 1);
    CHECK(result == 0 || (result == -1 && errno == EIO));
}
