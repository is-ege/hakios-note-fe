# iOS research port for the Galaxy Note FE

Samsung **SM-N935F / gracerlte / Exynos 8890 / Mali-T880 MP12**, running an iOS 16.5 userspace on HakiOS's XNU.

This repository is a fork of the Galaxy S9+ port, [danstronics/ios-s9plus-exynos](https://github.com/danstronics/ios-s9plus-exynos),
with its full history. The kernel, loader, user space and tools are the S9 port's, extended where the Note FE needed it. The
Note FE itself lives in [`core/devices/gracerlte/`](core/devices/gracerlte/): its configuration, its SoC layer (Exynos 8890),
its drivers, its boot shim, the scripts that build and run it on the phone, and [`PLAN.md`](core/devices/gracerlte/PLAN.md),
the port's working log. The S9+ release material (`installer/`, `profiles/`, `INSTALL.md`, `TESTING.md`, `metal-wip/`) is
kept as it came, so that the S9 port's changes can still be merged in (`git fetch upstream`).

## What works (12 November 2026)

| Part | State |
| --- | --- |
| Boot | A shim in the phone's BOOT starts the loader without UEFI; TWRP stays in RECOVERY and every run returns to it |
| Display | 1035 × 1840 drawn, enlarged by the panel's own scaler (S6E3HA5, FHD mode), 60 Hz; brightness slider works |
| GPU | Mali-T880 through Mesa/Panfrost (OpenGL ES); the window server draws every frame on it. No Metal |
| UI | SpringBoard, Settings and apps; home screen swipes at about 57 fps |
| CPU | The four big cores, 728 to 2288 MHz at Samsung's voltages, cooled by the SoC's own temperatures. The small cores are not used |
| Touch, buttons | Work (power, volume, Home) |
| Storage | iOS's system in the SYSTEM partition; `/private/var` kept between boots in USERDATA |
| Battery | Level and charging shown (read only; the charger is never written) |
| Wi-Fi | BCM4359 joins networks, with Samsung's firmware from your own stock image |
| Wallpapers | Chosen and edited in Settings without Metal; some gallery previews are placeholders |
| Not working | Audio, cameras, cellular, vibration, Metal |

## Building and running

On macOS, from the repository's root, with the phone in TWRP and connected over USB:

```sh
(cd core && make gracerlte_defconfig && make kernel)
(cd core && GL=1 REST=1 NO_AIR=1 ALONE=1 RC=rc.launchd DRIVERS=softpipe,panfrost userland/build.sh)
core/devices/gracerlte/tools/mkboot.sh kernel=core/hakios-kernel ramdisk=core/userland/root.apfs
core/devices/gracerlte/tools/phone-run.sh          # writes BOOT, boots it, keeps the logs in build/runs/
```

It needs inputs that are never in this repository: the iPhone X 16.5 (20F66) IPSW, unpacked by `core/sysimage/unpack.sh`; a
backup of the phone's own Lineage BOOT as the image template (`../private/`, outside the repository, or `TEMPLATE=`); and the
Wi-Fi firmware from the phone's stock firmware, in `core/fw-wifi/`.

The scripts write only BOOT, SYSTEM and USERDATA. The bootloader, PARAM and EFS are never written.

## Sources and attribution

This work builds on **HakiOS, Scion, Apple XNU, Mesa/Panfrost** and the **Galaxy S9+ port** it is forked from; see
[`SOURCES.md`](SOURCES.md), [`NOTICE`](NOTICE) and the notices under [`core/`](core/). Samsung's public kernel source for the
8890 (LineageOS `android_kernel_samsung_universal8890`) was read for register facts; nothing of it is copied. This is an
independent research project, not an Apple or Samsung product. No Apple software, Samsung firmware, partition backup or
personal data is published.
