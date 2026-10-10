#include <climits>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <set>
#include <string>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

#include "framework.hpp"
#include "helpers.hpp"

using posix_tests::exit_code;
using posix_tests::TempDir;

namespace {

bool write_file(const char *path, const std::string &text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    bool ok = write(fd, text.data(), text.size()) == (ssize_t)text.size();
    return close(fd) == 0 && ok;
}

std::string read_file(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return "<missing>";
    std::string text;
    char buffer[256];
    ssize_t count;
    while ((count = read(fd, buffer, sizeof buffer)) > 0) text.append(buffer, (size_t)count);
    close(fd);
    return text;
}

std::set<std::string> list(const char *path) {
    std::set<std::string> names;
    DIR *directory = opendir(path);
    if (!directory) return names;
    while (struct dirent *entry = readdir(directory)) names.insert(entry->d_name);
    closedir(directory);
    return names;
}

}

TEST(files, write_read_and_seek) {
    TempDir dir;
    CHECK(dir.ok());
    int fd = open("data", O_RDWR | O_CREAT | O_EXCL, 0644);
    CHECK(fd >= 0);
    CHECK_EQ(write(fd, "hello world", 11), 11L);
    CHECK_EQ(lseek(fd, 6, SEEK_SET), 6L);
    char buffer[16] = {};
    CHECK_EQ(read(fd, buffer, sizeof buffer), 5L);
    CHECK_EQ(std::string(buffer), std::string("world"));
    CHECK_EQ(lseek(fd, 0, SEEK_END), 11L);
    CHECK_EQ(lseek(fd, -5, SEEK_CUR), 6L);
    close(fd);
}

TEST(files, o_excl_refuses_an_existing_file) {
    TempDir dir;
    CHECK(write_file("data", "x"));
    CHECK_ERRNO(open("data", O_WRONLY | O_CREAT | O_EXCL, 0644), EEXIST);
}

TEST(files, the_umask_clears_permission_bits) {
    TempDir dir;
    mode_t old = umask(027);
    int fd = open("data", O_WRONLY | O_CREAT, 0666);
    umask(old);
    CHECK(fd >= 0);
    struct stat info = {};
    CHECK_OK(fstat(fd, &info));
    CHECK_EQ(info.st_mode & 0777, (mode_t)0640);
    close(fd);
}

TEST(files, ftruncate_grows_with_zeros_and_shrinks) {
    TempDir dir;
    int fd = open("data", O_RDWR | O_CREAT, 0644);
    CHECK_EQ(write(fd, "abcdef", 6), 6L);
    CHECK_OK(ftruncate(fd, 3));
    struct stat info = {};
    CHECK_OK(fstat(fd, &info));
    CHECK_EQ(info.st_size, (off_t)3);
    CHECK_OK(ftruncate(fd, 8192));
    char buffer[4] = {1, 1, 1, 1};
    CHECK_EQ(pread(fd, buffer, 4, 4000), 4L);
    CHECK(buffer[0] == 0 && buffer[3] == 0);
    close(fd);
    CHECK_EQ(read_file("data").substr(0, 3), std::string("abc"));
}

TEST(files, pread_and_pwrite_leave_the_offset_alone) {
    TempDir dir;
    int fd = open("data", O_RDWR | O_CREAT, 0644);
    CHECK_EQ(write(fd, "0123456789", 10), 10L);
    CHECK_EQ(lseek(fd, 2, SEEK_SET), 2L);
    CHECK_EQ(pwrite(fd, "XY", 2, 7), 2L);
    char buffer[3] = {};
    CHECK_EQ(pread(fd, buffer, 2, 7), 2L);
    CHECK_EQ(std::string(buffer), std::string("XY"));
    CHECK_EQ(lseek(fd, 0, SEEK_CUR), 2L);
    close(fd);
}

TEST(files, o_append_always_writes_at_the_end) {
    TempDir dir;
    CHECK(write_file("log", "a"));
    int first = open("log", O_WRONLY | O_APPEND);
    int second = open("log", O_WRONLY | O_APPEND);
    CHECK_EQ(write(first, "b", 1), 1L);
    CHECK_EQ(write(second, "c", 1), 1L);
    CHECK_EQ(write(first, "d", 1), 1L);
    close(first);
    close(second);
    CHECK_EQ(read_file("log"), std::string("abcd"));
}

TEST(files, readdir_lists_every_entry) {
    TempDir dir;
    CHECK_OK(mkdir("sub", 0755));
    CHECK(write_file("sub/one", "1"));
    CHECK(write_file("sub/two", "2"));
    CHECK_OK(mkdir("sub/three", 0755));
    std::set<std::string> wanted = {".", "..", "one", "two", "three"};
    CHECK(list("sub") == wanted);
}

TEST(files, rename_replaces_the_target) {
    TempDir dir;
    CHECK(write_file("old", "new contents"));
    CHECK(write_file("target", "old contents"));
    CHECK_OK(rename("old", "target"));
    CHECK_EQ(read_file("target"), std::string("new contents"));
    CHECK_ERRNO(access("old", F_OK), ENOENT);
}

TEST(files, rename_moves_between_directories) {
    TempDir dir;
    CHECK_OK(mkdir("a", 0755));
    CHECK_OK(mkdir("b", 0755));
    CHECK(write_file("a/file", "x"));
    CHECK_OK(rename("a/file", "b/file"));
    CHECK_EQ(read_file("b/file"), std::string("x"));
    CHECK_OK(rename("a", "b/a"));
    CHECK_OK(access("b/a", F_OK));
}

TEST(files, hard_links_share_the_inode) {
    TempDir dir;
    CHECK(write_file("first", "shared"));
    CHECK_OK(link("first", "second"));
    struct stat one = {}, two = {};
    CHECK_OK(stat("first", &one));
    CHECK_OK(stat("second", &two));
    CHECK_EQ(one.st_ino, two.st_ino);
    CHECK_EQ(one.st_nlink, (nlink_t)2);
    CHECK_OK(unlink("first"));
    CHECK_EQ(read_file("second"), std::string("shared"));
    CHECK_OK(stat("second", &two));
    CHECK_EQ(two.st_nlink, (nlink_t)1);
}

TEST(files, symlinks_resolve_and_readlink_returns_the_target) {
    TempDir dir;
    CHECK(write_file("real", "data"));
    CHECK_OK(symlink("real", "alias"));
    char target[64] = {};
    CHECK_EQ(readlink("alias", target, sizeof target - 1), 4L);
    CHECK_EQ(std::string(target), std::string("real"));
    struct stat followed = {}, link_itself = {};
    CHECK_OK(stat("alias", &followed));
    CHECK_OK(lstat("alias", &link_itself));
    CHECK(S_ISREG(followed.st_mode));
    CHECK(S_ISLNK(link_itself.st_mode));
    CHECK_EQ(read_file("alias"), std::string("data"));
}

TEST(files, a_dangling_symlink_is_enoent) {
    TempDir dir;
    CHECK_OK(symlink("nowhere", "dangling"));
    CHECK_ERRNO(open("dangling", O_RDONLY), ENOENT);
    struct stat info = {};
    CHECK_OK(lstat("dangling", &info));
}

TEST(files, getcwd_follows_chdir) {
    TempDir dir;
    CHECK_OK(mkdir("deeper", 0755));
    CHECK_OK(chdir("deeper"));
    char path[PATH_MAX] = {};
    CHECK(getcwd(path, sizeof path) != nullptr);
    CHECK_EQ(std::string(path), dir.path() + "/deeper");
    CHECK_OK(chdir(".."));
    CHECK(getcwd(path, sizeof path) != nullptr);
    CHECK_EQ(std::string(path), dir.path());
}

TEST(files, realpath_removes_dots_and_symlinks) {
    TempDir dir;
    CHECK_OK(mkdir("a", 0755));
    CHECK_OK(mkdir("a/b", 0755));
    CHECK_OK(symlink("a/b", "short"));
    char resolved[PATH_MAX] = {};
    CHECK(realpath("short/../b/./", resolved) != nullptr);
    CHECK_EQ(std::string(resolved), dir.path() + "/a/b");
}

TEST(files, errors_have_the_right_errno) {
    TempDir dir;
    CHECK_OK(mkdir("full", 0755));
    CHECK(write_file("full/file", "x"));
    CHECK_ERRNO(open("missing", O_RDONLY), ENOENT);
    CHECK_ERRNO(mkdir("full", 0755), EEXIST);
    CHECK_ERRNO(rmdir("full"), ENOTEMPTY);
    CHECK_ERRNO(unlink("full"), EISDIR);
    CHECK_ERRNO(open("full", O_WRONLY), EISDIR);
    CHECK_ERRNO(open("full/file/below", O_RDONLY), ENOTDIR);
    CHECK_ERRNO(rmdir("full/file"), ENOTDIR);
}

TEST(files, access_checks_existence_and_permission) {
    TempDir dir;
    CHECK(write_file("script", "#!/bin/sh\n"));
    CHECK_OK(chmod("script", 0755));
    CHECK_OK(access("script", F_OK));
    CHECK_OK(access("script", R_OK | X_OK));
    struct stat info = {};
    CHECK_OK(stat("script", &info));
    CHECK_EQ(info.st_mode & 0777, (mode_t)0755);
}

TEST(files, futimens_sets_the_modification_time) {
    TempDir dir;
    int fd = open("data", O_WRONLY | O_CREAT, 0644);
    struct timespec times[2] = {{1000000000, 0}, {1234567890, 0}};
    CHECK_OK(futimens(fd, times));
    struct stat info = {};
    CHECK_OK(fstat(fd, &info));
    CHECK_EQ((long)info.st_mtim.tv_sec, 1234567890L);
    CHECK_EQ((long)info.st_atim.tv_sec, 1000000000L);
    close(fd);
}

TEST(files, record_locks_exclude_other_processes) {
    TempDir dir;
    int fd = open("locked", O_RDWR | O_CREAT, 0644);
    CHECK_EQ(write(fd, "0123456789", 10), 10L);
    struct flock lock = {};
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 5;
    CHECK_OK(fcntl(fd, F_SETLK, &lock));
    pid_t parent = getpid();
    pid_t child = fork();
    if (child == 0) {
        int other = open("locked", O_RDWR);
        struct flock attempt = {};
        attempt.l_type = F_WRLCK;
        attempt.l_whence = SEEK_SET;
        attempt.l_start = 2;
        attempt.l_len = 1;
        int busy = fcntl(other, F_SETLK, &attempt) == -1 && (errno == EAGAIN || errno == EACCES);
        struct flock query = attempt;
        int owner =
            fcntl(other, F_GETLK, &query) == 0 && query.l_type == F_WRLCK && query.l_pid == parent;
        attempt.l_start = 7;
        int free_range = fcntl(other, F_SETLK, &attempt) == 0;
        _exit(busy && owner && free_range ? 0 : 1);
    }
    CHECK_EQ(exit_code(child), 0);
    close(fd);
}

TEST(files, statvfs_reports_the_root_filesystem) {
    struct statvfs info = {};
    CHECK_OK(statvfs("/", &info));
    CHECK(info.f_bsize > 0);
    CHECK(info.f_blocks > 0);
    CHECK(info.f_bfree <= info.f_blocks);
}

TEST(files, at_functions_work_relative_to_a_directory) {
    TempDir dir;
    CHECK_OK(mkdir("base", 0755));
    int base = open("base", O_RDONLY | O_DIRECTORY);
    CHECK(base >= 0);
    int fd = openat(base, "inside", O_WRONLY | O_CREAT, 0644);
    CHECK(fd >= 0);
    close(fd);
    CHECK_OK(mkdirat(base, "nested", 0755));
    CHECK_OK(renameat(base, "inside", base, "nested/moved"));
    struct stat info = {};
    CHECK_OK(fstatat(base, "nested/moved", &info, 0));
    CHECK_OK(unlinkat(base, "nested/moved", 0));
    CHECK_OK(unlinkat(base, "nested", AT_REMOVEDIR));
    close(base);
    CHECK(list("base") == (std::set<std::string>{".", ".."}));
}

TEST(files, a_large_file_round_trips) {
    TempDir dir;
    std::string block(65536, '\0');
    for (size_t i = 0; i < block.size(); i++) block[i] = (char)(i * 7 + 3);
    int fd = open("large", O_RDWR | O_CREAT, 0644);
    for (int i = 0; i < 64; i++)
        CHECK_EQ(write(fd, block.data(), block.size()), (ssize_t)block.size());
    CHECK_OK(fsync(fd));
    struct stat info = {};
    CHECK_OK(fstat(fd, &info));
    CHECK_EQ(info.st_size, (off_t)(64 * 65536));
    std::string back(65536, '\0');
    CHECK_EQ(pread(fd, back.data(), back.size(), 40 * 65536), (ssize_t)back.size());
    CHECK(back == block);
    close(fd);
}
