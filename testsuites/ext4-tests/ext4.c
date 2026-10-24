#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

static int checks;
static int failures;

static void check(int ok, const char *format, ...) {
    checks++;
    if (ok) return;
    failures++;
    va_list args;
    va_start(args, format);
    printf("  FAIL ");
    vprintf(format, args);
    printf(" (errno %d)\n", errno);
    va_end(args);
    fflush(stdout);
}

static uint8_t pattern(uint64_t offset, uint32_t seed) {
    uint64_t value = (offset + seed) * 0x9E3779B97F4A7C15ULL;
    return (uint8_t)(value >> 56);
}

static void fill(uint8_t *buffer, size_t length, uint64_t offset, uint32_t seed) {
    for (size_t index = 0; index < length; index++) buffer[index] = pattern(offset + index, seed);
}

static int same(const uint8_t *buffer, size_t length, uint64_t offset, uint32_t seed) {
    for (size_t index = 0; index < length; index++)
        if (buffer[index] != pattern(offset + index, seed)) return 0;
    return 1;
}

static uint8_t block[65536];

static int write_at(int fd, uint64_t offset, size_t length, uint32_t seed) {
    fill(block, length, offset, seed);
    return pwrite(fd, block, length, (off_t)offset) == (ssize_t)length;
}

static int read_check(int fd, uint64_t offset, size_t length, uint32_t seed) {
    memset(block, 0, length);
    return pread(fd, block, length, (off_t)offset) == (ssize_t)length &&
        same(block, length, offset, seed);
}

static int zeros_at(int fd, uint64_t offset, size_t length) {
    memset(block, 0xAA, length);
    if (pread(fd, block, length, (off_t)offset) != (ssize_t)length) return 0;
    for (size_t index = 0; index < length; index++)
        if (block[index]) return 0;
    return 1;
}

static int count_entries(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) return -1;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)))
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) count++;
    closedir(dir);
    return count;
}

static void name_of(char *out, size_t size, const char *dir, int index) {
    snprintf(out, size, "%s/entry-%05d-with-a-fairly-long-name-to-fill-blocks", dir, index);
}

static void many_files(const char *dir, int count, int remove_every) {
    char path[512];
    check(mkdir(dir, 0755) == 0, "mkdir %s", dir);
    for (int index = 0; index < count; index++) {
        name_of(path, sizeof path, dir, index);
        int fd = open(path, O_CREAT | O_WRONLY | O_EXCL, 0644);
        if (fd < 0) {
            check(0, "create %s", path);
            return;
        }
        char text[32];
        int length = snprintf(text, sizeof text, "%d\n", index);
        check(write(fd, text, (size_t)length) == length, "write %s", path);
        close(fd);
    }
    for (int index = 0; remove_every && index < count; index += remove_every) {
        name_of(path, sizeof path, dir, index);
        check(unlink(path) == 0, "unlink %s", path);
    }
}

static void verify_many(const char *dir, int count, int remove_every) {
    char path[512];
    int expected = 0;
    for (int index = 0; index < count; index++) {
        name_of(path, sizeof path, dir, index);
        int removed = remove_every && index % remove_every == 0;
        int fd = open(path, O_RDONLY);
        if (removed) {
            check(fd < 0, "%s should be gone", path);
            if (fd >= 0) close(fd);
            continue;
        }
        expected++;
        if (fd < 0) {
            check(0, "open %s", path);
            continue;
        }
        char text[32] = {0};
        check(read(fd, text, sizeof text - 1) > 0 && atoi(text) == index, "content of %s", path);
        close(fd);
    }
    check(count_entries(dir) == expected, "%s holds %d entries", dir, expected);
}

#define SPARSE_BLOCKS 1200U

static void sparse_file(const char *path, uint32_t block_size) {
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    check(fd >= 0, "create %s", path);
    if (fd < 0) return;
    for (uint32_t index = SPARSE_BLOCKS; index-- > 0;)
        if (index % 2 == 0)
            check(write_at(fd, (uint64_t)index * block_size, block_size, 7), "sparse write %u",
                  index);
    check(fsync(fd) == 0, "fsync %s", path);
    close(fd);
}

static void verify_sparse(const char *path, uint32_t block_size, uint32_t blocks) {
    int fd = open(path, O_RDONLY);
    check(fd >= 0, "open %s", path);
    if (fd < 0) return;
    for (uint32_t index = 0; index < blocks; index++) {
        uint64_t offset = (uint64_t)index * block_size;
        if (index % 2 == 0)
            check(read_check(fd, offset, block_size, 7), "sparse block %u of %s", index, path);
        else check(zeros_at(fd, offset, block_size), "hole %u of %s", index, path);
    }
    struct stat info;
    check(fstat(fd, &info) == 0 &&
              info.st_size == (off_t)((uint64_t)(blocks - 1U) * block_size + block_size),
          "size of %s", path);
    close(fd);
}

static void big_file(const char *path, uint64_t size, uint32_t seed) {
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    check(fd >= 0, "create %s", path);
    if (fd < 0) return;
    for (uint64_t at = 0; at < size; at += sizeof block) {
        size_t chunk = size - at < sizeof block ? (size_t)(size - at) : sizeof block;
        check(write_at(fd, at, chunk, seed), "write %s at %llu", path, (unsigned long long)at);
    }
    close(fd);
}

static void verify_big(const char *path, uint64_t size, uint32_t seed) {
    int fd = open(path, O_RDONLY);
    check(fd >= 0, "open %s", path);
    if (fd < 0) return;
    for (uint64_t at = 0; at < size; at += sizeof block) {
        size_t chunk = size - at < sizeof block ? (size_t)(size - at) : sizeof block;
        if (!read_check(fd, at, chunk, seed)) {
            check(0, "content of %s at %llu", path, (unsigned long long)at);
            break;
        }
    }
    struct stat info;
    check(fstat(fd, &info) == 0 && (uint64_t)info.st_size == size, "size of %s", path);
    close(fd);
}

static const char long_target[] =
    "/a/very/long/symlink/target/that/does/not/fit/in/the/inode/itself/because/it/is/longer/"
    "than/sixty/bytes/and/so/needs/a/block";

static void special_files(const char *dir) {
    char path[256];
    check(mkdir(dir, 0750) == 0, "mkdir %s", dir);
    snprintf(path, sizeof path, "%s/fifo", dir);
    check(mkfifo(path, 0640) == 0, "mkfifo");
    snprintf(path, sizeof path, "%s/null", dir);
    check(mknod(path, S_IFCHR | 0666, makedev(1, 3)) == 0, "mknod char");
    snprintf(path, sizeof path, "%s/disk", dir);
    check(mknod(path, S_IFBLK | 0660, makedev(259, 300)) == 0, "mknod block");
    snprintf(path, sizeof path, "%s/short", dir);
    check(symlink("target", path) == 0, "short symlink");
    snprintf(path, sizeof path, "%s/long", dir);
    check(symlink(long_target, path) == 0, "long symlink");
    snprintf(path, sizeof path, "%s/file", dir);
    int fd = open(path, O_CREAT | O_WRONLY, 0600);
    check(fd >= 0 && write(fd, "hello\n", 6) == 6, "small file");
    if (fd >= 0) close(fd);
    char alias[256];
    snprintf(alias, sizeof alias, "%s/hard", dir);
    check(link(path, alias) == 0, "hard link");
    check(setxattr(path, "user.colour", "blue", 4, 0) == 0, "user xattr");
    static char large[3000];
    memset(large, 'x', sizeof large);
    check(setxattr(path, "user.large", large, sizeof large, 0) == 0, "large xattr");
    check(setxattr(path, "trusted.secret", "42", 2, 0) == 0, "trusted xattr");
    struct {
        uint32_t version;
        struct {
            uint16_t tag;
            uint16_t perm;
            uint32_t id;
        } entries[5];
    } acl = {2,
             {{1, 6, 0xFFFFFFFFU},
              {2, 4, 1000},
              {4, 4, 0xFFFFFFFFU},
              {0x10, 4, 0xFFFFFFFFU},
              {0x20, 0, 0xFFFFFFFFU}}};
    check(setxattr(path, "system.posix_acl_access", &acl, sizeof acl, 0) == 0, "posix acl");
    struct timespec times[2] = {{1234567890, 123456789}, {2000000000, 987654321}};
    check(utimensat(AT_FDCWD, path, times, 0) == 0, "utimensat");
    check(chown(path, 100000, 200000) == 0, "32-bit owner");
}

static void verify_special(const char *dir) {
    char path[256];
    struct stat info;
    snprintf(path, sizeof path, "%s/fifo", dir);
    check(lstat(path, &info) == 0 && S_ISFIFO(info.st_mode) && (info.st_mode & 07777) == 0640,
          "fifo kept");
    snprintf(path, sizeof path, "%s/null", dir);
    check(lstat(path, &info) == 0 && S_ISCHR(info.st_mode) && major(info.st_rdev) == 1 &&
              minor(info.st_rdev) == 3,
          "char device kept");
    snprintf(path, sizeof path, "%s/disk", dir);
    check(lstat(path, &info) == 0 && S_ISBLK(info.st_mode) && major(info.st_rdev) == 259 &&
              minor(info.st_rdev) == 300,
          "block device kept");
    char target[512] = {0};
    snprintf(path, sizeof path, "%s/short", dir);
    check(readlink(path, target, sizeof target) == 6 && !memcmp(target, "target", 6),
          "short symlink kept");
    snprintf(path, sizeof path, "%s/long", dir);
    memset(target, 0, sizeof target);
    check(readlink(path, target, sizeof target) == (ssize_t)strlen(long_target) &&
              !strcmp(target, long_target),
          "long symlink kept");
    snprintf(path, sizeof path, "%s/file", dir);
    check(stat(path, &info) == 0 && info.st_nlink == 2, "hard link count");
    check(info.st_uid == 100000 && info.st_gid == 200000, "32-bit owner kept");
    check(info.st_atim.tv_sec == 1234567890 && info.st_atim.tv_nsec == 123456789, "atime ns kept");
    check(info.st_mtim.tv_sec == 2000000000 && info.st_mtim.tv_nsec == 987654321, "mtime ns kept");
    char value[4096] = {0};
    check(getxattr(path, "user.colour", value, sizeof value) == 4 && !memcmp(value, "blue", 4),
          "user xattr kept");
    check(getxattr(path, "user.large", value, sizeof value) == 3000 && value[0] == 'x' &&
              value[2999] == 'x',
          "large xattr kept");
    check(getxattr(path, "trusted.secret", value, sizeof value) == 2 && !memcmp(value, "42", 2),
          "trusted xattr kept");
    ssize_t acl = getxattr(path, "system.posix_acl_access", value, sizeof value);
    check(acl == 4 + 5 * 8, "acl kept (%zd bytes)", acl);
}

static void orphan_open(const char *path) {
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    check(fd >= 0, "create orphan");
    if (fd < 0) return;
    check(write_at(fd, 0, 40000, 3), "write orphan");
    check(unlink(path) == 0, "unlink open file");
    sync();
    check(read_check(fd, 0, 40000, 3), "orphan still readable");
    close(fd);
}

static void truncations(const char *path) {
    big_file(path, 3U * 1024U * 1024U + 1234U, 11);
    int fd = open(path, O_RDWR);
    check(fd >= 0, "open truncations");
    if (fd < 0) return;
    check(ftruncate(fd, 1000000) == 0, "shrink");
    check(ftruncate(fd, 5000000) == 0, "grow sparse");
    check(read_check(fd, 0, 65536, 11), "kept head");
    check(zeros_at(fd, 1000000, 4096), "zeroed tail after regrow");
    close(fd);
}

static void renames(const char *dir) {
    char from[256];
    char to[256];
    check(mkdir(dir, 0755) == 0, "mkdir %s", dir);
    snprintf(from, sizeof from, "%s/a", dir);
    snprintf(to, sizeof to, "%s/b", dir);
    check(mkdir(from, 0755) == 0 && mkdir(to, 0755) == 0, "mkdir a b");
    snprintf(from, sizeof from, "%s/a/sub", dir);
    check(mkdir(from, 0755) == 0, "mkdir sub");
    snprintf(to, sizeof to, "%s/b/moved", dir);
    check(rename(from, to) == 0, "rename directory across parents");
    snprintf(from, sizeof from, "%s/a", dir);
    check(rmdir(from) == 0, "rmdir a");
}

static void verify_renames(const char *dir) {
    char path[256];
    struct stat info;
    snprintf(path, sizeof path, "%s/b/moved", dir);
    check(stat(path, &info) == 0 && S_ISDIR(info.st_mode), "moved directory kept");
    snprintf(path, sizeof path, "%s/b", dir);
    check(stat(path, &info) == 0 && info.st_nlink == 3, "parent link count");
    snprintf(path, sizeof path, "%s/a", dir);
    check(stat(path, &info) != 0, "removed directory gone");
}

static void linux_disk_checks(void) {
    char value[256] = {0};
    int replayed = open("/mnt/journalled", O_RDONLY);
    char data[1024] = {0};
    check(replayed >= 0 && read(replayed, data, sizeof data) == 1024 && data[0] == 'B' &&
              data[1023] == 'B',
          "the transaction Linux left in the journal was replayed");
    if (replayed >= 0) close(replayed);
    int fd = open("/mnt/inline-small", O_RDONLY);
    check(fd >= 0 && read(fd, value, sizeof value) == 13 && !memcmp(value, "inline data!\n", 13),
          "inline file content");
    if (fd >= 0) close(fd);
    check(count_entries("/mnt/inline-dir") == 3, "inline directory entries");
    check(count_entries("/mnt/hashed") == 600, "linux htree directory entries");
    check(getxattr("/mnt/xattr-file", "user.made-by", value, sizeof value) == 5 &&
              !memcmp(value, "linux", 5),
          "linux xattr");
    fd = open("/mnt/prealloc", O_RDWR);
    check(fd >= 0, "open preallocated file");
    if (fd >= 0) {
        check(zeros_at(fd, 0, 65536), "unwritten extent reads zero");
        check(write_at(fd, 20000, 5000, 9), "write into unwritten extent");
        check(fsync(fd) == 0, "fsync prealloc");
        close(fd);
    }
    fd = open("/mnt/inline-small", O_WRONLY | O_APPEND);
    check(fd >= 0, "open inline file for append");
    if (fd >= 0) {
        check(write_at(fd, 13, 20000, 4), "grow inline file");
        close(fd);
    }
    for (int index = 0; index < 40; index++) {
        char path[128];
        snprintf(path, sizeof path, "/mnt/inline-dir/new-%02d", index);
        int created = open(path, O_CREAT | O_WRONLY, 0644);
        check(created >= 0, "create %s", path);
        if (created >= 0) close(created);
    }
    many_files("/mnt/deep", 3000, 7);
    check(unlink("/mnt/hashed/f-123") == 0, "unlink from linux htree");
}

static void verify_linux_disk(void) {
    int fd = open("/mnt/prealloc", O_RDONLY);
    check(fd >= 0, "open preallocated file");
    if (fd >= 0) {
        check(zeros_at(fd, 0, 20000), "zeros before written part");
        check(read_check(fd, 20000, 5000, 9), "written part of preallocated file");
        check(zeros_at(fd, 25000, 30000), "zeros after written part");
        close(fd);
    }
    fd = open("/mnt/inline-small", O_RDONLY);
    char head[16] = {0};
    check(fd >= 0 && read(fd, head, 13) == 13 && !memcmp(head, "inline data!\n", 13),
          "expanded inline head");
    if (fd >= 0) {
        check(read_check(fd, 13, 20000, 4), "expanded inline tail");
        close(fd);
    }
    check(count_entries("/mnt/inline-dir") == 43, "expanded inline directory");
    check(count_entries("/mnt/hashed") == 599, "linux htree after unlink");
    verify_many("/mnt/deep", 3000, 7);
}

struct variant {
    const char *device;
    int writable;
    int ea_inode;
};

static const struct variant variants[] = {
    {"/dev/sdc1", 1, 0}, {"/dev/sdc2", 1, 0}, {"/dev/sdc3", 1, 0}, {"/dev/sdc4", 1, 0},
    {"/dev/sdc5", 1, 1}, {"/dev/sdc6", 0, 0}, {"/dev/sdc7", 0, 0}, {"/dev/sdc8", 0, 0},
};

#define MATRIX_BLOB (2U * 1024U * 1024U + 3U)
#define MATRIX_KEEP (1024U * 1024U + 5U)

static void matrix(int current) {
    mkdir("/mnt2", 0755);
    for (size_t index = 0; index < sizeof variants / sizeof variants[0]; index++) {
        const struct variant *variant = &variants[index];
        if (mount(variant->device, "/mnt2", "ext4", 0, NULL) != 0) {
            check(0, "mount %s", variant->device);
            continue;
        }
        char text[16] = {0};
        int fd = open("/mnt2/hello", O_RDONLY);
        check(fd >= 0 && read(fd, text, sizeof text) == 6 && !memcmp(text, "hello\n", 6),
              "read the seeded file on %s", variant->device);
        if (fd >= 0) close(fd);
        struct statfs info;
        if (!variant->writable) {
            check(statfs("/mnt2", &info) == 0 && (info.f_flags & ST_RDONLY), "%s is read-only",
                  variant->device);
            errno = 0;
            fd = open("/mnt2/new", O_CREAT | O_WRONLY, 0644);
            check(fd < 0 && errno == EROFS, "creating on %s is refused", variant->device);
            if (fd >= 0) close(fd);
            check(umount("/mnt2") == 0, "umount %s", variant->device);
            continue;
        }
        check(statfs("/mnt2", &info) == 0 && !(info.f_flags & ST_RDONLY), "%s is writable",
              variant->device);
        uint32_t seed = 100U + (uint32_t)index;
        if (current == 1) {
            if (variant->ea_inode) {
                static char big[8192];
                ssize_t length = getxattr("/mnt2/hello", "user.big", big, sizeof big);
                check(length == 6000 && big[0] == 'z' && big[5999] == 'z',
                      "xattr kept in an EA inode on %s", variant->device);
                check(removexattr("/mnt2/hello", "user.big") == 0, "drop the EA inode xattr");
            }
            many_files("/mnt2/dir", 300, 4);
            big_file("/mnt2/blob", MATRIX_BLOB, seed);
            check(truncate("/mnt2/blob", MATRIX_KEEP) == 0, "truncate on %s", variant->device);
            check(symlink(long_target, "/mnt2/link") == 0, "symlink on %s", variant->device);
            check(setxattr("/mnt2/blob", "user.variant", variant->device, strlen(variant->device),
                           0) == 0,
                  "xattr on %s", variant->device);
            fd = open("/mnt2/dir/to-move", O_CREAT | O_WRONLY, 0644);
            if (fd >= 0) close(fd);
            check(rename("/mnt2/dir/to-move", "/mnt2/moved") == 0, "rename on %s", variant->device);
        } else {
            verify_many("/mnt2/dir", 300, 4);
            check(access("/mnt2/moved", F_OK) == 0, "renamed file on %s", variant->device);
            verify_big("/mnt2/blob", MATRIX_KEEP, seed);
            char target[512] = {0};
            check(readlink("/mnt2/link", target, sizeof target) == (ssize_t)strlen(long_target),
                  "symlink kept on %s", variant->device);
            char value[64] = {0};
            check(getxattr("/mnt2/blob", "user.variant", value, sizeof value) ==
                      (ssize_t)strlen(variant->device),
                  "xattr kept on %s", variant->device);
            if (variant->ea_inode)
                check(getxattr("/mnt2/hello", "user.big", value, sizeof value) < 0,
                      "EA inode xattr stays removed");
            char path[512];
            for (int entry = 0; entry < 300; entry++) {
                name_of(path, sizeof path, "/mnt2/dir", entry);
                unlink(path);
            }
            check(rmdir("/mnt2/dir") == 0, "remove the directory on %s", variant->device);
            check(unlink("/mnt2/blob") == 0 && unlink("/mnt2/link") == 0 &&
                      unlink("/mnt2/moved") == 0,
                  "remove files on %s", variant->device);
        }
        check(umount("/mnt2") == 0, "umount %s", variant->device);
    }
}

static void crash_writer(void) {
    printf("ext4: crash writer running\n");
    fflush(stdout);
    for (unsigned round = 0;; round++) {
        char path[128];
        snprintf(path, sizeof path, "/churn/r%u", round % 64);
        mkdir("/churn", 0755);
        int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
        if (fd >= 0) {
            write_at(fd, 0, 65536, round);
            close(fd);
        }
        if (round % 3 == 0) unlink(path);
        if (round % 10 == 0) {
            snprintf(path, sizeof path, "/churn/d%u", round % 50);
            mkdir(path, 0755);
            rmdir(path);
        }
    }
}

static void prepare(void) {
    mkdir("/dev", 0755);
    mkdir("/proc", 0755);
    mkdir("/tmp", 01777);
    mkdir("/mnt", 0755);
    if (access("/dev/null", F_OK) != 0) mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    if (access("/proc/self", F_OK) != 0) mount("proc", "/proc", "proc", 0, NULL);
}

static int phase(void) {
    char text[8] = {0};
    int fd = open("/ext4-phase", O_RDONLY);
    if (fd < 0) return 1;
    ssize_t length = read(fd, text, sizeof text - 1);
    close(fd);
    return length > 0 ? atoi(text) : 1;
}

static void set_phase(int value) {
    int fd = open("/ext4-phase", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    char text[8];
    int length = snprintf(text, sizeof text, "%d\n", value);
    if (fd >= 0) {
        if (write(fd, text, (size_t)length) != length) failures++;
        close(fd);
    }
}

int main(void) {
    prepare();
    int current = phase();
    struct statfs info;
    check(statfs("/", &info) == 0 && info.f_type == 0xEF53, "root is ext4");
    uint32_t block_size = (uint32_t)info.f_bsize;
    int mounted = mount("/dev/sdb", "/mnt", "ext4", 0, NULL) == 0;
    check(mounted, "mount the second disk");

    if (current == 1) {
        many_files("/many", 2500, 5);
        sparse_file("/sparse", block_size);
        big_file("/big", 40U * 1024U * 1024U + 777U, 5);
        special_files("/special");
        orphan_open("/orphan");
        truncations("/trunc");
        renames("/moves");
        if (mounted) linux_disk_checks();
        matrix(1);
        set_phase(2);
    } else if (current == 2) {
        verify_many("/many", 2500, 5);
        verify_sparse("/sparse", block_size, SPARSE_BLOCKS - 1U);
        verify_big("/big", 40U * 1024U * 1024U + 777U, 5);
        verify_special("/special");
        verify_renames("/moves");
        int fd = open("/trunc", O_RDONLY);
        check(fd >= 0 && read_check(fd, 0, 65536, 11) && zeros_at(fd, 1000000, 65536),
              "truncated file");
        struct stat st;
        check(fd >= 0 && fstat(fd, &st) == 0 && st.st_size == 5000000, "truncated size");
        if (fd >= 0) close(fd);
        check(access("/orphan", F_OK) != 0, "orphan gone");
        if (mounted) verify_linux_disk();
        matrix(2);
        check(unlink("/big") == 0, "delete big file");
        set_phase(3);
    } else if (current == 3) {
        set_phase(4);
        sync();
        if (mounted) umount("/mnt");
        crash_writer();
    } else {
        check(access("/many", F_OK) == 0, "data survived the crash");
        int count = count_entries("/churn");
        check(count >= 0, "churn directory readable after replay");
        set_phase(5);
    }

    if (mounted) check(umount("/mnt") == 0, "umount the second disk");
    sync();
    printf("ext4: phase %d %s %d/%d\n", current, failures ? "FAIL" : "PASS", checks - failures,
           checks);
    fflush(stdout);
    reboot(RB_POWER_OFF);
    return 0;
}
