# Mounting

Tunix has one filesystem tree. `mount(2)` and `umount2(2)` change what a path
in it reaches.

## What a mount is here

A directory can carry a filesystem mounted over it. `vfs_find_child()` steps
onto the mounted root whenever a path walk reaches such a directory — the same
place hard links are resolved — so nothing above the VFS has to know a mount
was crossed.

The mounted root takes the mountpoint's **name** and the mountpoint's
**parent**. That is what makes `..` leave the mount and `vfs_node_path()`
spell the path the caller actually walked, without a single special case in
either. The mountpoint node keeps its own children; they are simply
unreachable until the filesystem is unmounted, exactly as on Linux.

A tmpfs root is marked `VFS_VOLATILE`, which is what stops the ext4 driver
writing any of it to the disk. That flag, not the mount, is what makes it a
temporary filesystem.

## What can be mounted

| Type | Meaning |
|---|---|
| `tmpfs`, `ramfs` | a fresh, empty, RAM-only tree |
| `--bind` (`MS_BIND`) | show a directory that already exists at a second place |
| `ext2`, `ext3`, `ext4` | a filesystem on a block device, read through the ext4 driver |
| `vfat`, `fat`, `msdos` | a FAT filesystem on a block device |
| `proc`, `sysfs`, `devtmpfs` | already mounted where they belong; mounting one elsewhere is refused, since only their own drivers can build them |
| anything else | `ENODEV` |

`/proc/mounts` is the real table, root first:

```
/dev/sda2 / ext4 rw 0 0
tmpfs /tmp tmpfs rw 0 0
tmpfs /var/tmp tmpfs rw 0 0
tmpfs /run tmpfs rw 0 0
devtmpfs /dev devtmpfs rw 0 0
sysfs /sys sysfs rw 0 0
proc /proc proc rw 0 0
```

The trees the system boots with are in the table because that is what they are
to userspace, but they have no mountpoint to restore and `umount` refuses them
with `EPERM`.

## From the shell

`mount` and `umount` are first-party tools, not util-linux:

```sh
mount                                   # print /proc/mounts
mount -t tmpfs tmpfs /mnt/scratch
mount --bind /etc /mnt/etc
mount -o remount,ro /mnt/scratch
umount /mnt/scratch
```

An unknown `-o` name is an error rather than being ignored — silently dropping
`ro` would mount read-write.

## Limits

- Only one filesystem per mountpoint: mounting over an existing mount is
  refused with `EBUSY` rather than stacked, because two entries with the same
  target would leave `umount` unable to say which it meant.
- `MS_RDONLY` makes an ext filesystem read-only when it is mounted; on other
  mounts it, `MS_NOSUID`, `MS_NODEV` and `MS_NOEXEC` are recorded and reported
  in `/proc/mounts`, but **nothing enforces them**.
- Mounts are global. There are no mount namespaces, so one process's mount is
  every process's mount, and only root may call `mount`.
- A bind mount resolves paths correctly, but `getcwd` inside one reports the
  directory's original path, since the VFS derives a path from the node rather
  than from the walk that reached it.
