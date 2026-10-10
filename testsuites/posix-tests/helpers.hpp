#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <ftw.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace posix_tests {

inline std::string &self_path() {
    static std::string path = "/sbin/init";
    return path;
}

inline int number(const char *text) { return (int)std::strtol(text, nullptr, 10); }

inline int run_helper(int argc, char **argv) {
    if (argc < 3) return 100;
    std::string mode = argv[2];
    if (mode == "exit" && argc > 3) return number(argv[3]);
    if (mode == "fd-open" && argc > 3) return fcntl(number(argv[3]), F_GETFD) == -1 ? 1 : 0;
    if (mode == "env" && argc > 4) {
        const char *value = std::getenv(argv[3]);
        return value && std::strcmp(value, argv[4]) == 0 ? 0 : 1;
    }
    if (mode == "argc") return argc - 3;
    if (mode == "write" && argc > 3) {
        ssize_t length = (ssize_t)std::strlen(argv[3]);
        return write(1, argv[3], (size_t)length) == length ? 0 : 1;
    }
    return 100;
}

class TempDir {
  public:
    TempDir() {
        char pattern[] = "/tmp/posix.XXXXXX";
        if (mkdtemp(pattern)) path_ = pattern;
        if (!path_.empty() && chdir(path_.c_str()) != 0) path_.clear();
    }
    ~TempDir() {
        if (path_.empty()) return;
        if (chdir("/") != 0) return;
        nftw(
            path_.c_str(),
            [](const char *file, const struct stat *, int, struct FTW *) { return remove(file); },
            16, FTW_DEPTH | FTW_PHYS);
    }
    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;
    const std::string &path() const { return path_; }
    bool ok() const { return !path_.empty(); }

  private:
    std::string path_;
};

inline int wait_status(pid_t pid) {
    int status = 0;
    while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) return -1;
    }
    return status;
}

inline int exit_code(pid_t pid) {
    int status = wait_status(pid);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

template <typename... Args> pid_t spawn_helper(Args... args) {
    pid_t pid = fork();
    if (pid == 0) {
        execl(self_path().c_str(), self_path().c_str(), "--helper", args..., (char *)nullptr);
        _exit(127);
    }
    return pid;
}

}
