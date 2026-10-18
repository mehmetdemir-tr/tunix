#define TEST_NAME "drm"
#include "test.h"

#define EACCES 13
#define EBUSY  16
#define EINVAL 22

#define DRM_IOCTL_SET_MASTER  0x641eUL
#define DRM_IOCTL_DROP_MASTER 0x641fUL
#define DRM_IOCTL_GET_MAGIC   0x80046402UL
#define DRM_IOCTL_AUTH_MAGIC  0x40046411UL

static s64 auth_zero(int fd) {
    u32 magic = 0;
    return ioctl(fd, DRM_IOCTL_AUTH_MAGIC, &magic);
}

static void master_follows_the_file(void) {
    int first = (int)open("/dev/dri/card0", O_RDWR, 0);
    if (first == -ENOENT) {
        expect(1, "this machine has no display, so there is no card to test");
        return;
    }
    int second = (int)open("/dev/dri/card0", O_RDWR, 0);
    expect(first >= 0 && second >= 0, "the card opens twice");
    expect_eq(auth_zero(first), -EINVAL, "the first file is master, so magic 0 is just invalid");
    expect_eq(auth_zero(second), -EACCES, "the second file is not master");
    expect_eq(ioctl(second, DRM_IOCTL_DROP_MASTER, 0), -EINVAL,
              "dropping master on a file that is not master fails");
    expect_eq(auth_zero(first), -EINVAL, "and leaves the real master alone");
    expect_eq(ioctl(second, DRM_IOCTL_SET_MASTER, 0), -EBUSY,
              "a second file cannot take master while it is held");
    expect_eq(ioctl(first, DRM_IOCTL_DROP_MASTER, 0), 0, "the master drops it");
    expect_eq(auth_zero(first), -EACCES, "and is no longer master");
    expect_eq(ioctl(second, DRM_IOCTL_SET_MASTER, 0), 0, "now the second file can take it");
    expect_eq(auth_zero(second), -EINVAL, "and is master");
    expect_eq(auth_zero(first), -EACCES, "while the first stays demoted");
    u32 magic = 0;
    expect_eq(ioctl(first, DRM_IOCTL_GET_MAGIC, &magic), 0, "a client can ask for a magic");
    expect(magic != 0, "which is never 0");
    expect_eq(ioctl(second, DRM_IOCTL_AUTH_MAGIC, &magic), 0, "and the master can authenticate it");
    close(second);
    expect_eq(ioctl(first, DRM_IOCTL_SET_MASTER, 0), 0, "closing the master frees master");
    close(first);
}

static void run(int argc, char **argv) {
    (void)argc;
    (void)argv;
    master_follows_the_file();
}
