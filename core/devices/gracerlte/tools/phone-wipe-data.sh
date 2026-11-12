#!/bin/sh
# Empties iOS's data volume: writes the empty HFSX volume of build/datavol (core/tools/mkdatavol.py) to USERDATA from TWRP.
# Everything iOS kept between boots is gone afterwards. USERDATA is the only partition this writes.
#   core/devices/gracerlte/tools/phone-wipe-data.sh
set -eu
cd "$(dirname "$0")/../../../.."       # the repository's root; build/ there is ignored
VOL=build/datavol
NODE=/dev/block/sda23
BLOCKS=14134272
HEAD_SIZE=1036722176
HEAD_SHA=3f5c1c95494cffb8a44fd1e7b67c2a2eb156b9b4c7e8627f5ec4d5e127c606bf
[ "$(adb get-state)" = recovery ] || { echo "phone-wipe-data: the phone is not in TWRP" >&2; exit 1; }
DEV=$(adb shell readlink -f /dev/block/platform/155a0000.ufs/by-name/USERDATA | tr -d '\r')
[ "$DEV" = "$NODE" ] || { echo "phone-wipe-data: USERDATA is $DEV, not $NODE: stopping" >&2; exit 1; }
ROOM=$(adb shell "blockdev --getsize64 $DEV" | tr -d '\r')
[ "$ROOM" = $((BLOCKS * 4096)) ] || { echo "phone-wipe-data: USERDATA is $ROOM bytes, the volume was made for $((BLOCKS * 4096))" >&2; exit 1; }
adb shell "umount /data 2>/dev/null; umount /sdcard 2>/dev/null; true"
! adb shell "grep -q '^$DEV ' /proc/mounts" || { echo "phone-wipe-data: $DEV is still mounted" >&2; exit 1; }
adb push $VOL/userdata-head.img.gz /tmp/data-head.img.gz > /dev/null
adb push $VOL/userdata-tail.img /tmp/data-tail.img > /dev/null
adb shell "gzip -dc /tmp/data-head.img.gz | dd of=$DEV bs=4194304 conv=fsync 2>/dev/null; dd if=/tmp/data-tail.img of=$DEV bs=4096 seek=$((BLOCKS - 1)) conv=fsync 2>/dev/null; rm -f /tmp/data-head.img.gz /tmp/data-tail.img; sync; echo 3 > /proc/sys/vm/drop_caches"
GOT=$(adb shell "head -c $HEAD_SIZE $DEV | sha256sum" | cut -d' ' -f1)
[ "$GOT" = "$HEAD_SHA" ] || { echo "phone-wipe-data: USERDATA reads back $GOT, not $HEAD_SHA" >&2; exit 1; }
echo "phone-wipe-data: USERDATA is an empty data volume again"
