#!/bin/sh
set -eu

. testsuites/boot.sh

tests=${*:-process memory files pipes sockets time}

work=$(mktemp -d "${TMPDIR:-/tmp}/tunix-tests.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM

failed=
for name in $tests; do
	source=testsuites/kernel-tests/$name.c
	test -f "$source" || { echo "run.sh: no test called $name" >&2; exit 1; }
	dir=$work/$name
	tunix_root "$dir/root"
	$cc -std=gnu11 -Wall -Wextra -Werror -O2 -static -nostdlib -nostartfiles \
		-ffreestanding -fno-stack-protector -fno-pic -fno-pie -no-pie -fno-builtin \
		-fno-asynchronous-unwind-tables "$source" -o "$dir/root/sbin/init"
	tunix_image "$dir/tunix.img" "$dir/root"
	tunix_boot "$dir/tunix.img" "$dir/serial.log" "${TIMEOUT:-120}"
	if grep -a "^$name: " "$dir/serial.log" | tee "$dir/result" | grep -q "^$name: PASS"; then
		grep -a "^$name: PASS" "$dir/result"
	else
		grep -a "^$name: FAIL" "$dir/result" || { echo "$name: no result, last serial lines:"; tail -20 "$dir/serial.log"; }
		failed="$failed $name"
	fi
done

if [ -n "$failed" ]; then
	echo "failed:$failed"
	exit 1
fi
echo "all passed"
