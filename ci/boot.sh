#!/bin/sh
set -eu

. testsuites/boot.sh

work=$(mktemp -d "${TMPDIR:-/tmp}/tunix-ci-boot.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM

tunix_root "$work/root"
$cc -std=gnu11 -Wall -Wextra -Werror -O2 -static -nostdlib -nostartfiles \
	-ffreestanding -fno-stack-protector -fno-pic -fno-pie -no-pie -fno-builtin \
	-fno-asynchronous-unwind-tables utils/ci-boot/init.c -o "$work/root/sbin/init"
ROOT_SLACK_MIB=16 tunix_image "$work/tunix.img" "$work/root"
tunix_boot "$work/tunix.img" "$work/serial.log" "${TIMEOUT:-60}"

if grep -aq '^ci-boot: PASS' "$work/serial.log"; then
	echo "ci-boot ($arch): PASS"
else
	echo "ci-boot ($arch): FAIL, last serial lines:"
	tail -30 "$work/serial.log"
	exit 1
fi
