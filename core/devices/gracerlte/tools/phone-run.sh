#!/bin/sh
# One run: IMAGE into BOOT (phone-write.sh), a restart, the run's own restart back into TWRP, and what it left in TWRP's
# pstore, kept in build/runs/<time>/. A run with the USB log (usb_gadget=5) shows up as a /dev/cu.usbmodem*: what it
# sends is kept too, in usb.txt.
#   core/devices/gracerlte/tools/phone-run.sh [IMAGE]          (default build/note-fe-boot.img)
set -eu
cd "$(dirname "$0")/../../../.."       # the repository's root; build/ there is ignored
IMG=${1:-build/note-fe-boot.img}
OUT=build/runs/$(date +%Y%m%dT%H%M%S)
mkdir -p "$OUT"
core/devices/gracerlte/tools/phone-write.sh "$IMG"
twrp_ports=$(ls /dev/cu.usbmodem* 2>/dev/null || true)
adb shell reboot || true      # (adb drops the connection as the phone goes)

# The USB log. TWRP has a serial port of its own, and the kernel's may come up under the same name: so first TWRP's
# ports go, then the first port that appears is the kernel's, read until it goes away (the run's end). One descriptor
# both ways: macOS drops what came before the port was open, so the console's "log" asks for the log from its start.
(
	end=$(($(date +%s) + ${WAIT:-180}))
	for p in $twrp_ports; do
		while [ -e "$p" ] && [ "$(date +%s)" -lt "$end" ]; do sleep 1; done
	done
	while [ "$(date +%s)" -lt "$end" ]; do
		for d in /dev/cu.usbmodem*; do
			[ -e "$d" ] || continue
			exec 3<>"$d" || exit 0
			stty -f "$d" raw -echo 2>/dev/null || true
			printf 'log\r' >&3
			exec cat <&3 > "$OUT/usb.txt" 2>/dev/null
		done
		sleep 1
	done
) &
usb=$!

sleep 10
timeout ${WAIT:-180} adb wait-for-recovery || { echo "phone-run: no TWRP after ${WAIT:-180} s: the run did not come back" >&2; kill $usb 2>/dev/null; exit 1; }
kill $usb 2>/dev/null || true
adb shell 'for f in /sys/fs/pstore/*; do echo "== $f"; cat $f; echo; done' > "$OUT/pstore.txt"
echo "phone-run: back in TWRP; $OUT/pstore.txt"
[ -s "$OUT/usb.txt" ] && echo "phone-run: the USB log: $OUT/usb.txt ($(wc -c < "$OUT/usb.txt") bytes)"
exit 0
