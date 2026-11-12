#!/bin/sh
# Writes IMAGE to one of the phone's partitions from TWRP and reads it back. BOOT and SYSTEM (iOS's system image,
# decided 2026-11-08) are the only partitions this ever writes. Through TWRP's /tmp in 64 MB pieces: it is RAM
# (1.8 GB), smaller than the system image.
#   core/devices/gracerlte/tools/phone-write.sh IMAGE [BOOT|SYSTEM]     (default BOOT; Lineage's own: ../private/20261107T133304Z/boot.img)
set -eu
IMG=$1
PART=${2:-BOOT}
case $PART in
	BOOT) NODE=/dev/block/sda7 ;;
	SYSTEM) NODE=/dev/block/sda16 ;;
	*) echo "phone-write: $PART is not a partition this writes (BOOT, SYSTEM)" >&2; exit 1 ;;
esac
[ "$(adb get-state)" = recovery ] || { echo "phone-write: the phone is not in TWRP" >&2; exit 1; }
DEV=$(adb shell readlink -f /dev/block/platform/155a0000.ufs/by-name/$PART | tr -d '\r')
[ "$DEV" = "$NODE" ] || { echo "phone-write: $PART is $DEV, not $NODE: stopping" >&2; exit 1; }
SIZE=$(wc -c < "$IMG" | tr -d ' ')
ROOM=$(adb shell "blockdev --getsize64 $DEV" | tr -d '\r')
[ "$SIZE" -le "$ROOM" ] || { echo "phone-write: $IMG ($SIZE bytes) is larger than $PART ($ROOM)" >&2; exit 1; }
WANT=$(shasum -a 256 "$IMG" | cut -d' ' -f1)
PIECE=$((64 << 20))
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"; adb shell rm -f /tmp/part-write.img >/dev/null 2>&1 || true' EXIT
i=0
while [ $((i * PIECE)) -lt "$SIZE" ]; do
	dd if="$IMG" of="$TMP/piece" bs=$PIECE skip=$i count=1 2>/dev/null
	adb push "$TMP/piece" /tmp/part-write.img > "$TMP/push.log" 2>&1 || { cat "$TMP/push.log" >&2; exit 1; }
	adb shell "dd if=/tmp/part-write.img of=$DEV bs=4194304 seek=$((i * PIECE / 4194304)) conv=fsync 2>/dev/null"
	i=$((i + 1))
	[ "$SIZE" -le "$PIECE" ] || printf '\rphone-write: %s: %d of %d MB' "$PART" $((i * 64 < SIZE >> 20 ? i * 64 : SIZE >> 20)) $((SIZE >> 20))
done
[ "$SIZE" -le "$PIECE" ] || echo
adb shell "sync; echo 3 > /proc/sys/vm/drop_caches"
GOT=$(adb shell "head -c $SIZE $DEV | sha256sum" | cut -d' ' -f1)
[ "$GOT" = "$WANT" ] || { echo "phone-write: $PART reads back $GOT, not $WANT" >&2; exit 1; }
echo "phone-write: $PART = $IMG ($SIZE bytes, sha256 $WANT)"
