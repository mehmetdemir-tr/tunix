#!/bin/sh
set -eu

. testsuites/boot.sh

work=$(mktemp -d "${TMPDIR:-/tmp}/tunix-posix-tests.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM

tunix_root "$work/root"
$cxx -std=c++20 -O2 -Wall -Wextra -Werror -static -pthread \
	testsuites/posix-tests/*.cpp -o "$work/root/sbin/init"
ROOT_SLACK_MIB=128 tunix_image "$work/tunix.img" "$work/root"
tunix_boot "$work/tunix.img" "$work/serial.log" "${TIMEOUT:-600}"

grep -a -E '^  FAIL |^    |^posix: ' "$work/serial.log" || true
if grep -aq '^posix: PASS' "$work/serial.log"; then
	exit 0
fi
grep -aq '^posix: ' "$work/serial.log" || { echo "posix: no result, last serial lines:"; tail -30 "$work/serial.log"; }
exit 1
