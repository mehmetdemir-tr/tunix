#!/bin/sh
set -eu

. testsuites/boot.sh

work=$(mktemp -d "${TMPDIR:-/tmp}/tunix-ext4-tests.XXXXXX")
[ -n "${KEEP:-}" ] || trap 'rm -rf "$work"' EXIT INT TERM

tunix_root "$work/root"
$cc -std=gnu11 -O2 -Wall -Wextra -Werror -static testsuites/ext4-tests/ext4.c -o "$work/root/sbin/init"
ROOT_SLACK_MIB=160 tunix_image "$work/tunix.img" "$work/root"

seed=$work/seed
mkdir -p "$seed/inline-dir" "$seed/hashed"
printf 'inline data!\n' >"$seed/inline-small"
for name in a b c; do printf '%s\n' "$name" >"$seed/inline-dir/$name"; done
index=0
while [ $index -lt 600 ]; do
	printf '%s\n' "$index" >"$seed/hashed/f-$index"
	index=$((index + 1))
done
printf 'victim\n' >"$seed/orphan-victim"
head -c 1024 /dev/zero | tr '\0' 'A' >"$seed/journalled"
head -c 1024 /dev/zero | tr '\0' 'B' >"$work/replayed"
: >"$seed/xattr-file"
head -c 8192 /dev/zero | tr '\0' 'p' >"$seed/prealloc"
truncate -s 96M "$work/disk2.img"
mkfs.ext4 -q -F -b 1024 -I 256 -N 16384 -O inline_data -d "$seed" "$work/disk2.img"
e2fsck -fyD "$work/disk2.img" >/dev/null 2>&1 || [ $? -lt 4 ]
victim=$(debugfs -R "stat /orphan-victim" "$work/disk2.img" 2>/dev/null | sed -n 's/^Inode: \([0-9]*\).*/\1/p')
debugfs -w -f - "$work/disk2.img" >/dev/null 2>&1 <<EOF
ea_set /xattr-file user.made-by linux
punch /prealloc 0 7
fallocate /prealloc 0 63
sif /prealloc size 65536
unlink /orphan-victim
sif <$victim> links_count 0
ssv last_orphan $victim
EOF
debugfs -R "ex /prealloc" "$work/disk2.img" 2>/dev/null | grep -q Uninit ||
	{ echo "ext4: debugfs did not make an unwritten extent" >&2; exit 1; }
journalled=$(debugfs -R "bmap /journalled 0" "$work/disk2.img" 2>/dev/null)
printf 'jo -c\njw -b %s %s\njc\n' "$journalled" "$work/replayed" |
	debugfs -w -f - "$work/disk2.img" >/dev/null 2>&1
dumpe2fs -h "$work/disk2.img" 2>/dev/null | grep -q needs_recovery ||
	{ echo "ext4: debugfs did not leave a transaction to replay" >&2; exit 1; }

matrix=$work/matrix
mkdir -p "$matrix/seed"
printf 'hello\n' >"$matrix/seed/hello"
set -- \
	"mkfs.ext2 -b 1024 -I 128" \
	"mkfs.ext3 -O ^metadata_csum,^64bit" \
	"mkfs.ext4 -b 2048 -O ^has_journal,^metadata_csum,^64bit,uninit_bg,^orphan_file,^metadata_csum_seed" \
	"mkfs.ext4 -O meta_bg,^resize_inode,sparse_super2,mmp,large_dir" \
	"mkfs.ext4 -I 512 -O ea_inode,^orphan_file" \
	"mkfs.ext4 -O bigalloc -C 16384" \
	"mkfs.ext4 -O casefold" \
	"mkfs.ext4 -O quota"
table="label: gpt"
position=2048
number=1
for make in "$@"; do
	part=$matrix/part$number.img
	truncate -s 48M "$part"
	$make -q -F -d "$matrix/seed" "$part" 2>/dev/null
	e2fsck -fy "$part" >/dev/null 2>&1 || [ $? -lt 4 ]
	table="$table
start=$position, size=98304"
	position=$((position + 98304))
	number=$((number + 1))
done
big=$(head -c 6000 /dev/zero | tr '\0' 'z')
debugfs -w -R "ea_set /hello user.big $big" "$matrix/part5.img" >/dev/null 2>&1
truncate -s $(((position + 2048) * 512)) "$work/disk3.img"
printf '%s\n' "$table" | sfdisk --quiet "$work/disk3.img"
number=1
while [ $number -le 8 ]; do
	start=$(sfdisk -d "$work/disk3.img" | grep "disk3.img$number " | sed -E 's/.*start= *([0-9]+).*/\1/')
	dd if="$matrix/part$number.img" of="$work/disk3.img" bs=512 seek="$start" conv=notrunc status=none
	number=$((number + 1))
done

boot() {
	if [ "$arch" = aarch64 ]; then
		timeout "$2" qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a72 -smp 2 -m 1G \
			-kernel "$kernel" -append root=LABEL=tunix-root \
			-drive "format=raw,file=$work/tunix.img,if=none,id=disk0" -device nvme,drive=disk0,serial=tunix \
			-drive "format=raw,file=$work/disk2.img,if=none,id=disk1" -device nvme,drive=disk1,serial=second \
			-drive "format=raw,file=$work/disk3.img,if=none,id=disk2" -device nvme,drive=disk2,serial=third \
			-display none -no-reboot -serial "file:$1" >/dev/null 2>&1 &
	else
		timeout "$2" qemu-system-x86_64 -machine q35,accel=kvm:tcg -cpu max -smp 2 -m 1G \
			-drive "format=raw,file=$work/tunix.img,if=none,id=disk0" -device ide-hd,drive=disk0,bus=ide.0 \
			-drive "format=raw,file=$work/disk2.img,if=none,id=disk1" -device ide-hd,drive=disk1,bus=ide.1 \
			-drive "format=raw,file=$work/disk3.img,if=none,id=disk2" -device ide-hd,drive=disk2,bus=ide.2 \
			-display none -no-reboot -serial "file:$1" >/dev/null 2>&1 &
	fi
	qemu=$!
}

fsck_all() {
	line=$(sfdisk -d "$work/tunix.img" | grep "tunix.img2")
	start=$(printf '%s\n' "$line" | sed -E 's/.*start= *([0-9]+).*/\1/')
	size=$(printf '%s\n' "$line" | sed -E 's/.*size= *([0-9]+).*/\1/')
	dd if="$work/tunix.img" of="$work/root.ext4" bs=512 skip="$start" count="$size" status=none
	number=1
	while [ $number -le 8 ]; do
		line=$(sfdisk -d "$work/disk3.img" | grep "disk3.img$number ")
		start=$(printf '%s\n' "$line" | sed -E 's/.*start= *([0-9]+).*/\1/')
		dd if="$work/disk3.img" of="$work/part$number.ext4" bs=512 skip="$start" count=98304 status=none
		number=$((number + 1))
	done
	for image in root.ext4 disk2.img part1.ext4 part2.ext4 part3.ext4 part4.ext4 part5.ext4 \
		part6.ext4 part7.ext4 part8.ext4; do
		if ! e2fsck -fn "$work/$image" >"$work/fsck.log" 2>&1; then
			echo "ext4: e2fsck found problems in $image after $1:"
			cat "$work/fsck.log"
			failed=1
		fi
	done
}

replay_on_linux() {
	fsck_all "$1"
	block_size=$(dumpe2fs -h "$work/root.ext4" 2>/dev/null | sed -n 's/^Block size: *//p')
	journal=$(debugfs -R "bmap <8> 0" "$work/root.ext4" 2>/dev/null)
	sequence=$(debugfs -R "logdump -O" "$work/root.ext4" 2>&1 |
		sed -n 's/^Found expected sequence \([0-9]*\), type 1.*/\1/p' | tail -1)
	if [ -z "$sequence" ]; then
		echo "ext4: no transaction written by Tunix is left in the journal"
		failed=1
		return
	fi
	python3 -I testsuites/ext4-tests/rewind.py "$work/root.ext4" $((journal * block_size)) "$sequence"
	debugfs -w -R "feature needs_recovery" "$work/root.ext4" >/dev/null 2>&1
	e2fsck -fy "$work/root.ext4" >"$work/replay.log" 2>&1 || true
	if grep -qi 'checksum error\|corrupt\|invalid' "$work/replay.log" ||
		! e2fsck -fn "$work/root.ext4" >/dev/null 2>&1; then
		echo "ext4: Linux could not replay transaction $sequence written by Tunix:"
		cat "$work/replay.log"
		failed=1
	else
		echo "ext4: Linux replayed transaction $sequence written by Tunix"
	fi
}

failed=
run_phase() {
	boot "$work/serial$1.log" "${TIMEOUT:-300}"
	wait "$qemu" || true
	if grep -aq "^ext4: phase $1 PASS" "$work/serial$1.log"; then
		grep -a "^ext4: phase $1" "$work/serial$1.log"
	else
		grep -a -E '^  FAIL|^ext4: |EXT4|JBD2' "$work/serial$1.log" || tail -30 "$work/serial$1.log"
		echo "ext4: phase $1 did not pass"
		failed=1
	fi
	grep -a -E 'EXT4|JBD2' "$work/serial$1.log" || true
	fsck_all "phase $1"
}

run_phase 1
grep -aq 'JBD2: replaying' "$work/serial1.log" ||
	{ echo "ext4: the journal Linux left on the second disk was not replayed"; failed=1; }
run_phase 2
replay_on_linux "phase 2"
rm -f "$work/root.ext4"

boot "$work/serial3.log" 120
waited=0
until grep -aq 'crash writer running' "$work/serial3.log" 2>/dev/null; do
	sleep 1
	waited=$((waited + 1))
	[ $waited -lt 100 ] || break
done
sleep "${CRASH_AFTER:-7}"
kill -9 "$qemu" 2>/dev/null || true
wait "$qemu" 2>/dev/null || true
grep -aq 'crash writer running' "$work/serial3.log" || { echo "ext4: the crash writer never started"; failed=1; }

run_phase 4
grep -aq 'JBD2: replaying' "$work/serial4.log" && echo "ext4: the journal was replayed after the crash"

if [ -n "$failed" ]; then
	echo "ext4: FAIL"
	exit 1
fi
echo "ext4: all phases passed and e2fsck is clean"
