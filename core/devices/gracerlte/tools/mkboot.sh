#!/bin/sh
# Builds build/note-fe-boot.img: the shim with an XNUP payload behind it, as the kernel of the phone's own (Lineage) BOOT,
# which supplies the ramdisk and the dt.img S-Boot reads the device tree from.
#
#   [ARGS="boot-args"] core/devices/gracerlte/tools/mkboot.sh [payload items...]
#       items as xnuloader/mkpayload.py takes them (name=file); loader.efi is always in. With a kernel, its command line is
#       the kernel build's .config (make gracerlte_defconfig) with ARGS over it (name=value replaces, name= removes).
set -eu
cd "$(dirname "$0")/../../../.."       # the repository's root; build/ there is ignored
CORE=core
DEV=core/devices/gracerlte
# Lineage's BOOT as backed up from the phone: kept outside the repository, with the rest of the backups
TEMPLATE=${TEMPLATE:-../private/20261107T133304Z/boot.img}
OUT=build
mkdir -p "$OUT"

make -s -C $DEV/shim
make -s -C $DEV/shim test >/dev/null
[ -f $CORE/xnuloader/identity.h ] || $CORE/xnuloader/mkidentity.sh --placeholder
CLANG=/opt/homebrew/opt/llvm/bin/clang LLD_LINK=/opt/homebrew/opt/lld/bin/lld-link $CORE/xnuloader/build.sh >/dev/null
case " $* " in
*" kernel="*)
	# The MAX77854's MUIC switch (devices/gracerlte/drivers/power/hakios.max77854.c), which the USB log needs: S-Boot
	# leaves D+/D- switched off; the touch screen's supplies on the S2MPS16 (hakios.s2mps16.c), which S-Boot leaves off;
	# the DECON's VCLK at Samsung's 42 MHz, 60 frames a second (hakios.exynos8890_decon.c), where S-Boot leaves 26;
	# the GPU's supplies at 546 MHz's (hakios.exynos8890_g3d.c, with mali=), where S-Boot leaves 260, and its clock
	# after its load (mali_gov): 546 while frames come, else 260 (650 is left out: it caps the M1s at 1560 MHz).
	# /private/var kept between boots: an HFSX volume in USERDATA, the one partition the kernel writes (emptied by
	# tools/phone-wipe-data.sh). In RAM instead: ARGS="ufs_native_ro=1 ufs_native_write= var_hfs=".
	# The panel's brightness (hakios.s6e3ha5.c, panel=1) and iOS's slider taken to it (brightness_bridge=1).
	# A device driver's arguments, which the configuration does not know (tools/boot-args.py
	# reads only the common tree), so they are given here.
	CMDLINE=$(python3 $CORE/tools/hakios-config.py --root $CORE args --with "max77854=1 max77854_usbsw=1 s2mps16_tsp=1 decon_vclk42=1 mali_mhz=546 mali_gov=1 mali_gov_frame=546 wifi_pcie=6 brcmfmac=8 wifi_nvram=nvram.txt_murata_c0_plus ufs_native_ro=0 ufs_native_write=USERDATA var_hfs=ufs_USERDATA_data panel=1 brightness_bridge=1 ${ARGS:-}")
	echo "mkboot: boot-args: $CMDLINE"
	set -- "$@" --cmdline "$CMDLINE" ;;
esac
python3 $CORE/xnuloader/mkpayload.py -o "$OUT/payload.bin" --no-version loader.efi=$CORE/xnuloader/loader.efi "$@"

# the arm64 header's image_size covers the payload too: S-Boot places nothing of its own over what it was told of
python3 - "$OUT/kernel.bin" $DEV/shim/shim.bin "$OUT/payload.bin" <<'EOF'
import struct, sys
out, shim, payload = sys.argv[1:]
blob = bytearray(open(shim, 'rb').read() + open(payload, 'rb').read())
assert blob[56:60] == b'ARMd'
blob += b'\0' * (-len(blob) % 16)
struct.pack_into('<Q', blob, 16, (len(blob) + 0xFFFF) & ~0xFFFF)
open(out, 'wb').write(blob)
print(f'{out}: {len(blob)} bytes')
EOF
python3 $DEV/tools/samsung_bootimg.py --template "$TEMPLATE" --kernel "$OUT/kernel.bin" --output "$OUT/note-fe-boot.img"
