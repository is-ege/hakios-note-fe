# HakiOS on Galaxy Note FE (SM-N935F, gracerlte): rough port plan

Reference: [danstronics/ios-s9plus-exynos](https://github.com/danstronics/ios-s9plus-exynos) (S9+, Exynos 9810), built on
Scion-Project's HakiOS fork. The S9 boots S-Boot → BOOT → Mu-Silicium UEFI → HakiOS loader → XNU. Here it is
**S-Boot → BOOT (Lineage's boot.img, our kernel) → `shim/` (no UEFI) → HakiOS loader → XNU → iOS 16.5 userspace**.
Each test writes BOOT from TWRP (`tools/phone-run.sh`), and every run ends with a restart back into TWRP.

## What we are porting to

Measured on the phone on 2026-11-07.

| | Note FE (8890) | S9+ (9810), the reference | Can we reuse it? |
|---|---|---|---|
| CPUs | 4× Exynos M1 + 4× A53 (ARMv8.0, aes/pmull/sha1/sha2/crc32). **Boots on an A53** (cpu0-3) | 4× M3 + 4× A55, boots on an A55 | The `isa_floor` patch already advertises ARMv8.0 to userspace, and the `bootcpu=` handoff exists |
| Page size | **A53 has no 16K translation granule** (ARM TRM). M1: **unknown** | A55 and M3 support 16K | **Gate: see Phase 0** |
| GIC / SMP | GIC-400 (v2) @ `0x11001000`, PSCI | GICv2, PSCI | Yes |
| RAM | 4 GB: `0x80000000` + `0x880000000` | 6 GB, sparse banks (patch 1512) | Mostly. **`0xF0000000` is Samsung's `modem_if` reserved region**, and the shim drops the payload exactly there, so it must move |
| GPU | **Mali-T880 MP12 r2p0** (Midgard) @ `0x14AC0000` | Mali-G72 (Bifrost) | Partly. Mesa Panfrost supports T880, but the kernel driver needs Midgard's MMU page-table format and 8890 power/clock code |
| Display | 1440×2560 (16:9) AMOLED, S-Boot framebuffer @ `0xD8000000`, DECON_F + DSIM | DPP scaling path | Use the bootloader framebuffer first. The DECON/VPP scaler is new work |
| Touch | **S6SY661** (`sec_ts`, I²C `0x48`, HSI2C @ `0x14E10000`) | S6SY761 (`sec_ts`, `0x48`) | Likely close to drop-in, with new bus/GPIO/rail values |
| Buttons | gpio_keys (power, volume, physical Home) + capacitive keys (`sec_touchkey`) | exynos_keys | New GPIO banks. Home is a real key here |
| Storage | UFS 2.0 @ `0x155A0000` | UFS @ `0x11120000` | The Exynos UFS hooks are a starting point. 8890-specific PHY/vendor registers needed |
| Wi-Fi | **BCM4359 over PCIe** (`14e4:4415`), pcie0 @ `0x157A0000` | BCM4361 over PCIe | brcmfmac supports 4359. `hakios.exynos_pcie.c` needs 8890 controller/PHY |
| Power | MAX77854 (charger/MUIC/FG), S2MPS16 PMIC, S2MM005 CCIC | MAX77705, S2MPS18 via ACPM | New drivers. 8890 has no ACPM |
| USB | DWC3 @ `0x15400000` | DWC3 (`usb_gadget=5`) | Core yes, PHY new. **Needed early as the log/console channel** |
| Audio, cameras, modem | Cirrus Arizona/Moon + MAX98506, IMX260/2L1, Shannon | — | Out of scope for now |
| Recovery | TWRP 3.x (CronosKernel), pstore `pmsg-ramoops` present | TWRP 3.7 | Same workflow |

## The two things that can kill the project, checked first

1. **16K pages on M1.** iOS XNU runs with a 16K granule. The A53 cluster cannot run it, so XNU has to live on the four M1
   cores. If M1 also lacks TGran16, we would need a 4K-page XNU, a research project of its own. A few-hundred-byte probe
   answers this before anything else is built.
2. **The boot handoff.** S-Boot starts us on an A53 with the MMU off. The shim/loader must PSCI-start an M1 and continue there
   before XNU turns on 16K translation. Both UEFI (4K) and the loader can stay on the A53.

## Phases (each ends at a gate you can see)

### Phase 0: safety, tooling, probe — **done 2026-11-08, gate passed**
- Backups: `private/20261107T133304Z` (BOOT, RECOVERY, EFS, CPEFS, RADIO, DT) and `private/20261107T204822Z-extra` (`sdb`/`sdc`
  bootloader LUNs, `sdd`, PARAM, UP_PARAM, BOTA*, PERSISTENT, STEADY, ...), all hash-checked against the live device.
- Test cycle (`probe/`): build → `samsung_bootimg.py` repacks our kernel into Lineage's boot.img (its dt.img and ramdisk; the
  packer reproduces Lineage's image byte for byte) → `dd` to BOOT in TWRP, verify readback → reboot → back in TWRP.
- **Log channel:** TWRP's ramoops sits at `0x92000000` (32K; Lineage's is at `0x91800000`). A `DBGC`-headed record written in the
  4K slots from `0x92004000` comes back as `/sys/fs/pstore/pmsg-ramoops-0` in TWRP. RAM survives the reboot.
- **Return to TWRP:** PMU `INFORM3 = 0x12345674`, `INFORM2 = 0x12345678` (Samsung's recovery mode) as the first thing at entry,
  then `SWRESET` (PMU `+0x400`) = 1.
- **Results:** S-Boot enters at **EL1** on A53 `0x100`. **M1 (`MIDR 0x531f0011`) has TGran16 = 1**; A53 does not. Both are ARMv8.0
  with aes/pmull/sha1/sha2/crc32. CTR differs: M1 I-line 128 B, A53 64 B.
- **Secondary cores:** Samsung's EL3 answers PSCI CPU_ON with 0 but leaves the core off. What works: the CPU part of Samsung's
  `pwrcal_syspwr_init` (CPU/L2/non-CPU options, up scheduler), then CPU_ON, then the PMU's `CPU_CONFIGURATION[3:0] = 0xF`
  (`0x105C2000 + slot*0x80`, M1 slots 0-3, A53 4-7). All 7 cores arrived at our entry at EL1. XNU's PSCI start needs this as
  its "prepare" hook (the S9 port's patch 1508 hook, at a different place).
- `heimdall` v2.2.2 (built from source by Ege): `Heimdall/build/bin/heimdall`, for Download mode if TWRP is ever lost.

### Phase 1: boot shim + loader — **done 2026-11-08, gate passed (in the log)**
- Decision: no UEFI port. `shim/` is Scion's Tensor G5 shim (hakios-soc-lga, MIT) cut down for the 8890. It offers the
  loader the UEFI services it uses (memory, GOP, stall, reset, the DTB) and nothing else, so nothing before the kernel can
  reach storage (`shim/audit.py` runs on every build). It runs at EL1 on the A53, MMU on (4K, 1:1).
- 8890 specifics in the shim: INFORM2/3 → TWRP first thing; the framebuffer from S-Boot's `s3cfb.bootloaderfb=0xd8000000`
  (1440×2560; S-Boot **ignores the boot.img cmdline**, so no `scion.fb=`); Samsung's bootargs-only log regions
  `0x91200000 + 32 MB` reserved (`memmap.c`); CNTFRQ is 0, the tree's 26 MHz is used. `make -C shim test` checks the
  memory map on the phone's own tree.
- Loader: `platform_gracerlte` in `ios-s9plus-exynos/core/xnuloader/loader.c` (by root compatible `samsung,exynos8890`;
  GIC-400; cores 0-3 A53 `0x100-0x103`, 4-7 M1 `0x0-0x3`; payload moved off modem_if to `0xE0000000`; log in TWRP's pmsg).
- Image: `tools/mkboot.sh` → `build/note-fe-boot.img` (shim + XNUP payload as the kernel of Lineage's boot.img).
  `tools/phone-write.sh IMAGE` writes and verifies BOOT; `tools/phone-run.sh` does a whole run and saves pstore in `build/runs/`.
- **TWRP's pmsg zone is `0x92004000`** (4 KB, right after the 16 KB dump record), not where `ram.c` suggested.
- Gate seen in pmsg-ramoops-0: `XNULOADER v3` banner, `platform gracerlte, GICv2`, payload moved, `no kernel in payload`,
  SWRESET → TWRP.
- **Display works** (seen on the panel 2026-11-08): DECON_F runs the panel in command mode with the TE trigger, window 0
  fetched from `0xd8000000` by VPP 1. S-Boot leaves the trigger masked with auto-mask (`HW_SW_TRIG_CONTROL` `0x12b0`):
  one frame for the logo, then none. The shim clears both bits (what it reads, less bits 5 and 12), so
  every TE sends a frame and the framebuffer behaves like a plain one for the loader and XNU.
- Found for Phase 2: **S-Boot leaves the watchdog (`0x10020000`) counting** (WTCON `0x8021`), probably harmless because the
  PMU masks its reset, as on the S9. The loader's `exynos_wdt` assumes two cluster watchdogs and writes the "other" one:
  needs a single-watchdog case and the 8890's PMU bit before it is turned on. 4 KB of pmsg is tight for the loader's log.

### Phase 2: first XNU boot, RAM root, `hello` as PID 1 — **done 2026-11-08, gate passed**
- Kernel: the S9 port's XNU (`ios-s9plus-exynos/core`, `make kernel` natively on macOS with its `xnu-deps/setup.sh`),
  unchanged except two stale patches removed from the clone (1203 touch timestamps: nothing uses it; 1501 S9 ABOX audio:
  its hook is gone from the repo's own drivers). Both failed to apply at the repo's HEAD.
- Run: `tools/mkboot.sh kernel=ios-s9plus-exynos/core/hakios-kernel ramdisk=ios-s9plus-exynos/core/userland/hello
  --cmdline "loader_return_after=60 cpus=1 bootcpu=4 hello_wait=90"`. The loader runs on the A53 and watches; XNU runs on M1
  core `0x000` in the high 2 GB bank. PAN turns itself off (the M1 has none, S9 patch 1509).
- Loader fixes this took (`loader.c`): the PMU core power-up after CPU_ON (`pmu_cpu_power`, the Phase 0 recipe);
  **Samsung's EL3 does not pass CPU_ON's context in x0** (the trampoline now loads its record by address); the return stubs'
  `msr PAN/UAO` are **UNDEFINED on ARMv8.0** and turned every return into an endless trap loop (now only where the core has them).
- Seen: `Hello from user space on Darwin!` at 2.8 s, GICv2 timer on PPI 27, XNU's console on `0xd8000000`, the 60 s budget
  ends in an orderly return and SWRESET → TWRP. Checkpoints (`--checkpoint N` to mkpayload) bisect early hangs.

### Phase 3: debug channel + SMP — **done 2026-11-08, both gates passed**
- Device layer `ios-s9plus-exynos/core/devices/gracerlte/` (`make gracerlte_defconfig`, made with `tools/hakios-device.py new`):
  only this phone's drivers are built; its defaults (`cpus=4 bootcpu=4`) reach the command line through `tools/mkboot.sh`
  (`ARGS="..."` for a run's own). The SoC driver `soc/drivers/core/hakios.exynos8890_cpu.c` powers a core after CPU_ON.
- **SMP: all four M1s run the kernel** (3 of 3 runs, each core in the scheduler ~1.2 ms after CPU_ON, clean stop on return).
  What it took: (1) PSCI answers are 32-bit: Samsung's EL3 returns -1 as `0xFFFFFFFF`, which the loader and kernel read as a
  state and refused every core (both now sign-extend; patch 1514); (2) a hook after a granted CPU_ON for the PMU power-up
  (patch 1513); (3) **Samsung's EL3 keeps only the low 32 bits of CPU_ON's entry**: the kernel's reset vector in the high bank
  (`0x8_8340_2000`) became garbage. The loader now gives the kernel a trampoline below 4 GB (`/chosen/psci-cpu-on-entry`,
  `kernel_cpu_entry`, which goes on to the reset vector found through `loader_stub_slot[4]`; patch 1515).
- **USB log: the kernel's whole log on the Mac's `/dev/cu.usbmodem*`** (CDC-ACM, the S9 port's DWC3 gadget, a new 8890
  glue `soc/drivers/core/hakios.exynos8890_usb.c`; on by default). `tools/phone-run.sh` keeps it in `build/runs/<t>/usb.txt`
  (~38 KB a run, from the kernel's first line to its last, vs 4 KB of pmsg). What it took: S-Boot leaves every USB clock
  running and the PHY isolated (PMU `+0x704`); Samsung's USB3 CAL sequence (version 0x0101, 26 MHz) brings the PHY up.
  **S-Boot leaves the MAX77854 MUIC's data switch open** (CTRL1 `0x00`), so the host saw nothing: `max77854_usbsw=1`
  writes CTRL1 = `0x09` (D+/D- to the AP), the one MAX77854 write, approved by Ege; S-Boot resets it each boot.
  Reaching the MAX77854 (HSI2C1 `0x13650000`, the common HSI2C driver now without the S9's USI): S-Boot bit-bangs that
  bus, its pins `gpd1-2/3` are GPIO and are handed to the controller (function 2).
- Host side: macOS drops serial data until the port is open, so the capture asks the console for `log` (a replay from the
  start); TWRP has a serial port of its own, so the capture waits for it to go before taking the kernel's.

### Phase 4: storage + iOS userspace — **gate passed 2026-11-08: SpringBoard's home screen on the panel**
- **Storage: done 2026-11-08, read only.** The common UFS driver's own mode (`ufs_native=4`) with a new SoC row,
  `exynos8890` (`drivers/storage/hakios.ufshc.c`; the Exynos look's addresses are per SoC now), memory below 4 GB from
  `sys/hakios.exynos8890_mem.h` (`0xDEC00000`, 4 MB). S-Boot leaves the controller enabled, the link up (HS-G3, one lane)
  and its list running, with the device in POWERDOWN (its last command); the driver wakes it, proves its descriptor layout
  (PRDT entries of 16 bytes) with one two-block read against LUN 0's MBR and GPT header (the S9 compares with the loader's
  reads, which need UEFI), reads both GPTs and makes `/dev/ufs_NAME` for 24 partitions. Taken in five runs, one step each.
- iOS userland: `sysimage/unpack.sh` (7-Zip's `7zz` as `7z`), `ALONE=1 RC=rc.launchd SYSTEM=1 userland/build.sh` (since Phase 6
  also `GL=1 REST=1 NO_AIR=1`: the T880 draws, and the launch angels such as PosterBoard are in),
  `xnuloader/mkidentity.sh`; `system.apfs` (2 504 355 840 bytes) to SYSTEM by `tools/phone-write.sh IMAGE SYSTEM`
  (64 MB pieces through TWRP's RAM `/tmp`); `system_apfs=ufs_SYSTEM` is a default.
- **First iOS boot (run 20261108T132637):** init's 38 checks clean, `/dev/ufs_SYSTEM` mounted read only as the root,
  `/private/var` in RAM; launchd, backboardd (22 s), SpringBoard; the home screen from ~125 s, then 10 frames a second in
  software (7.5 ms a frame in the framebuffer driver). Only widget extensions crashed (Mail, Wallet), as on the S9+.
  **Stripes: fixed 2026-11-08** (`soc/drivers/core/hakios.exynos8890_decon.c`, the S9's `hakios_scanout_*` for DECON_F,
  `fb_flip=1` a default). Flipping buffers alone did not help: S-Boot's DECON takes 24 ms a frame (its VCLK at 26 MHz, see
  the 60 Hz bullet below) against the panel's 16.7 ms refresh, so a TE always came mid-frame and the DECON started the next
  frame the moment one ended, out of step with the panel's scan. Now the trigger auto-masks at each frame's start and is
  unmasked at its end: frames start at a TE, never overtaken by the scan. Measured with the DECON's own frame-start/done bits.
- **Aspect: native 16:9, 2026-11-08** (run 20261108T150117): `fb_nativeui=2 fbwidth=1440 fbheight=2560` (gracerlte.config).
  UIKit lays out 450 x 800 points at 3.2 pixels a point on the panel's own size, nothing scaled; MobileGestalt still sees
  the iPhone X (kernel `MiniIOMobileFramebuffer.cpp`, `userland/src/display_native.c`, now one table of canvases; opened by
  backboardd's and SpringBoard's libraries). Ege: fills the screen, but the home screen's icons sit far apart and some UI
  is a little off -- accepted for now; to revisit (layout tuning), or fall back to a letterbox (VGR1 needs the CFW SMC).
- Native UFS read-only for 8890 (the RAM is too small to hold a 2.5 GB system the way the RAM path does).
- Build the iOS SYSTEM image from your IPSW with Scion's `sysimage/` tooling, and write it to the chosen partition (decision below).
- Framebuffer-only display: CPU blit to 1440×2560, software rendering.
- **Gate:** SpringBoard on screen (slow, no GPU, no input).

### Phase 5: input (about 1 week) — **done 2026-11-08: touch and keys**
- Touch: the S9's `hakios.s6sy761.c` with a board table (`s6sy761_board=gracerlte`: HSI2C7, IRQ gpa1-0) and the S6SY661's
  older command set (sense on 0x40, one event 0x71 until an empty one, the older event layout). S-Boot leaves the touch
  supplies off: `devices/gracerlte/drivers/power/hakios.s2mps16.c` switches on LDO32/LDO33 of the S2MPS16 (only their on
  bits, stock voltages checked first, read back; `s2mps16_tsp=1`, approved by Ege). Seen on the phone: taps land where pressed.
- Keys (run 20261108T152809): the S9's `hakios.exynos_keys.c` with a board table (`exynos_keys_board=gracerlte`: ALIVE
  0x10580000, power gpa2-4, volume up gpa0-3, down gpa0-4, Home gpa0-5), only read: S-Boot leaves them inputs, Home with
  its pull-up. Seen on the phone: power locks and wakes, Home goes home; volume presses are read (counted in the log) but
  iOS ignores them while there is no audio device -- with audio, later. Capacitive back/recents: not done.
- **Gate:** unlock, open Settings, navigate.

### Phase 6: GPU (2-4 weeks, the biggest unknown)
- Bring up the Mali-T880 in the kernel step by step, as the reference did: power domain and clocks, reset, GPU_ID, a write-value
  job, then the Midgard (LPAE-format) MMU.
- Mesa Panfrost Midgard (GLES 3.1) through the hakios DRM shim. Check the reference's Bifrost-specific patches before reusing them.
- **Gate:** GLES compositor, smooth-ish scrolling.
- **First look, 2026-11-08 (run 20261108T155235, read only, `exynos8890_g3d=1`):** S-Boot leaves the G3D block on: the domain
  is on, G3D_PLL is at 260 MHz (the stock table's lowest level), the GPU is on that PLL with its gates open, DVS is off, BUCK6
  is at 600 mV (B6CTRL3 0x30), and BUCK11 vdd_extra is at 750 mV. Samsung's ECT in the stock sboot (offset 0x38c00) gives
  600-643 mV for 260 MHz, depending on the chip's ASV group (fused at 0x101E9000, not read yet). GPU_ID 0x08802000: a
  T880 r2p0 with 12 shader cores (0xff0f), one L2, one tiler, 8 address spaces, 3 job slots and MMU_FEATURES 0x2830 (48-bit
  VA, 40-bit PA). Its cores are off and idle after the power-on reset (INT_RAWSTAT 0x100). The first bring-up at 260 MHz
  needs no PMU, clock or voltage write, only the GPU's own registers. Faster clocks need G3D_PLL changes and per-chip
  voltages from the ECT plus the fuse.
- **Step 1, first job: done 2026-11-08 (run 20261108T163932, `mali=8 mali_soc=exynos8890`).** The Mali driver is now one driver
  with a glue per SoC (`drivers/gpu/hakios.mali_glue.h`). The S9's power, clock and supply code moved unchanged to
  `drivers/gpu/hakios.exynos9810_g3d.c` (it compiles, but hasn't been run). The Note FE's is `soc/drivers/core/hakios.exynos8890_g3d.c`,
  which writes nothing to the G3D block: it goes on only at S-Boot's 260 MHz and a voltage in Samsung's table. The chip's
  fused ASV (read through Samsung's SMC_CMD_REG) is group 10, table version 10, which gives 600 mV at 260 MHz, exactly
  S-Boot's setting (and 675 mV at 546 MHz, 712 mV at 650 MHz). The T880 path, keyed off GPU_ID, uses LPAE page tables,
  no COHERENCY_ENABLE, and kbase's tFRx r2p0 settings after a reset. Seen on the phone: the reset, all 12 shader cores,
  the L2 and the tiler powered, then a write-value job through the GPU's own tables with no fault. Two Midgard
  differences found on the way: AS_TRANSTAB's identity mode still walks a table (a bus fault at level 0, so there is no
  identity step on the T880), and write-value jobs have no immediate types (the job writes zero over a pattern instead).
  Next: step 9 (HakiosMali serving programs) and Mesa's panfrost for Midgard (v5), starting with gl_test.
- **Step 2, Mesa draws: done 2026-11-08 (run 20261108T190222, `mali=9`, root `GL=test`).** `mali_test` (HakiosMali
  without Mesa) passes with a Midgard write job. Mesa 25.0.7 is built on this Mac by `DRIVERS=softpipe,panfrost
  tools/mk-mesa.sh` (needs meson, Homebrew's llvm-ar/strip on PATH, and GNU-style `sha256sum`/`stat`: shims were used,
  the script is unchanged). It names the GPU "Mali-T880 (Panfrost), OpenGL ES 3.1", and gl_test's triangle is right to
  the pixel at 256x256 and 1080x2400. The one bug: every job finished and nothing faulted, yet triangles were missing or
  cut along 128-pixel bins. Found by peeking at GPU memory after each chain (new boot-arg `mali_peek=VA[,VA...]`) and a
  scratch test drawing each shape twice: the vertex job's positions were right, but the tiler binned each draw with the
  positions the previous draw had left at the same address. The GPU's units were not coherent with each other because
  Midgard page entries had no shareability bits; they now carry Linux's `ARM_LPAE_PTE_SH_OS` (bits 9:8 = 2). Ruled out
  on the way: AFBC, hierarchy masks, depth range, CRC, core affinity, MEMATTR 0x88, CPU cache cleaning, polygon-list
  clearing, job barrier/prefetch/invalidate bits. `gl_test` now takes `NAME=VALUE` environment arguments (a job list
  cannot set them) and prints a coarse map when the triangle is wrong.
  Next: GL=1 (the window server drawing with the T880), then faster clocks (needs approval: G3D_PLL and BUCK6 writes).
- **Step 3, the window server on the T880: done 2026-11-08 (run 20261108T191238, `mali=9`, root `GL=1 REST=1 NO_AIR=1`).**
  CoreAnimation draws every frame through Mesa's panfrost: 0 GL errors, no GPU faults, about 34 ms a frame (the panel's
  30 Hz), of which about 11 ms is waiting for the GPU even on frames of a few draws, and under 1 ms handing commands on.
  Ege: everything renders as before, smoother, usable, not yet consistent. 772 small non-BGRA IOSurfaces are copied by
  the CPU rather than shared. A horizontal line flashes at the top of the screen during animations (not looked at yet).
  **The wallpaper.** Every Note FE root so far lacked `REST=1`, so PosterBoard (the wallpaper and its Settings picker)
  was never in the image. With it, PosterBoard aborts: it makes its snapshot renderer with `MTLCreateSystemDefaultDevice()`
  and puts `[device newCommandQueue]` in a dictionary, and there is no Metal device (traced through the shared cache:
  PosterBoard +0x71aa8). Ege chose a still picture for now: `userland/src/inject_sb.c`
  (`poster_variant_view_did_load`) puts the stock Clownfish background on the lock poster's view where there is no Metal
  device; the home screen shows it through its portal (run 20261108T194813 and after; USB screenshot checked). The picker
  and the live wallpapers still need Metal. Root recipe from now on: `GL=1 REST=1 NO_AIR=1 ALONE=1 RC=rc.launchd`.
- **Where a frame's time goes, 2026-11-08** (`userland/rc.jobs.fps` with the Settings tap at 0.63 0.775; `touch_lat=1`,
  `fps_log=1`, `glprof=1000`; `tools/frames.py`). The GPU is busy 15-55% of each second even in the heaviest gestures: its
  clock is not the limit yet. The window server's frame was about 9 ms + 0.3-0.5 ms a draw, a quarter of its drawing
  thread inside Mesa's kernel calls: the submit cleaning the caches of every surface it names, and no-purge MADVISE
  round trips. `mcs=1` (clean only a surface whose seed moved) and `caenv=HMD=1` (MADVISE answered in the shim) took its
  median frame from 14.3 to 7.3 ms, p90 45 to 24.5 ms (runs 20261108T201319, 202435); Ege saw nothing drawn wrong, so
  both are gracerlte.config defaults. Settings' zoom went from 15 to 26 frames a second; home-screen swipes stayed at about
  25 and Ege felt no difference, because there the panel is the limit: frames are handed over every 33 ms and each
  waits about 80 ms for the panel behind the two queued before it (`fb_queue=2` at S-Boot's 30 Hz link), while the
  start of a swipe has 55-150 ms gaps between frames from the CPU side. The display driver's copy is 7.2 ms a frame.
  `fb_queue=1` (one frame waiting, not two) cut the hand-over-to-panel wait of animating frames from a median 57 ms to
  6 ms (p90 94 to 61; run 20261108T204337), but Ege felt no difference: left at 2.
- **60 Hz, 2026-11-08** (approved by Ege: CMU writes for the DECON's VCLK only). The registers, read on the phone: S-Boot's
  link is already Samsung's (DSC on, two encoders, 2 slices of 720 x 32 at 8 bits a pixel, DSIM 480 x 2560, PLL 1/69/1 =
  897 Mbit/s on four lanes, about 8 ms a frame); the 24 ms were VCLK0 on the 26 MHz oscillator (CMU_DISP0's user mux 0,
  the DECON's mux on DISP_PLL's mux, itself on the oscillator), two compressed pixels a cycle. Samsung's table for 1440 x
  2560 with two DSC encoders wants 42 MHz; `decon_vclk42=1` (from `tools/mkboot.sh`) takes CMU_TOP's VCLK0 (BUS2 672 MHz)
  divided by 16 at a frame's end: 14.9 ms a frame, a frame every refresh (306-333 in 5 s), 0 underruns in 4000 frames
  (run 20261108T210643). No PLL, voltage, DSI or panel write; the driver puts the muxes back on a stall or underrun.
  Ege: small animations smooth, scrolling/swiping/app switching about the same: those are the window server's CPU.
- **Heavy frames are the drawing thread's processor time, 2026-11-08.** `glprof_wait=1` (eagl.c: the drawing thread's
  stacks while it waits inside a frame) found it blocked only ~0.5 s in a 5-minute script: not the GPU. A heavy frame is
  ~240 draws and ~20 ms of CPU. `mali_calls=1` (per-request counts and kernel time a second): ~1000 submits, ~1500
  WAIT_BO, ~600 MMAP_BO a second; `mali_cache_profile=1`: the submit cleaned ~47 full-screen surfaces a second (700 MB,
  250 ms of the drawing thread). Those were the window server's own targets, whose seed CoreAnimation and eagl bump twice
  a frame with write locks that write nothing. Fix (Ege chose it over zero-copy for now): eagl says a target is drawn by
  its GPU alone (HakiosMali request 0x7b, `hakios_drm_gpu_drawn`); MiniIOSurface counts write locks by other processes,
  and the submit cleans such a surface only after one of those. Submit cleaning 250 -> 8-40 ms a second, drawing 21.4 ->
  16.0 ms a frame, home swipes 37-40 -> 43 fps (run 20261108T221705). Left: Mesa's per-batch buffer churn (CREATE/MMAP/
  WAIT_BO round trips), ~16 batches a frame; the display's invalidate + 7 ms copy (zero-copy, fb_zc on the DECON).
- **The big cores sit at Samsung's floor, 2026-11-08 (read only, run 20261108T225347).** S-Boot leaves MNGS_PLL at
  728 MHz (measured on an M1: 727), BUCK2 vdd_mngs at 675 mV. Samsung's tree (`/cpufreq`): 728 MHz is the big cluster's
  `cl1_min_qos_freq` and `cl1_reboot_limit_freq`; Android holds 2288 MHz through boot (`cl1_boot_min_qos`), boosts to it on
  touch, and its governor scales between; we have no 8890 frequency driver, so the M1s never leave the floor. Everything
  else is already at its top: MIF0-3 PLLs 1794 MHz (BUCK1 850 mV), INT 800 mV, the A53s (the loader's) 1066 MHz at 787 mV,
  BUS0-3 1056/800/672/1872 MHz. This chip's voltages (from the stock sboot's ECT as S5E8890-asv.c computes them: ASV table version
  10, big group 9, + the MARGIN block's 25 mV for dvfs_big, SSA1 0): big 728 675 mV, 1144 725, 1456 794, 1768 875,
  2080 956, 2288 1012.5 mV. The same computation gives exactly what S-Boot set on BUCK2 (728: 675), BUCK3 (little 1066:
  787.5) and BUCK1 (MIF 1794: 850). The ECT's dividers (dvfs_big) that change above 728: PCLK_MNGS and CNTCLK_MNGS 5 -> 7,
  SCLK_PROMISE_MNGS 2 -> 4 (1456) and 6 (2288).
- **The M1s at 1456 MHz, 2026-11-08 (run 20261108T230829; approved by Ege).** `cpufreq_big=1456000` (default in
  tools/mkboot.sh; hakios.exynos8890_cpu.c, from hakios.input.c): BUCK2 675 -> 793.75 mV first (hakios.s2mps16.c, read
  back), then Samsung's transition through the 1056 MHz switch clock (BUS0_PLL), MNGS_PLL S 2 -> 1. Measured 727 MHz
  before, 1055 on the switch, 1455 after; 0 underruns; Ege: "noticeably smoother", still not a clean 30 or 60. Only on
  this chip's ASV (big group 9, table 10) and from S-Boot's state; S-Boot's MNGS_EMA_CON (0x1091B9) is kept. Right after
  boot the GPU, still at S-Boot's 260 MHz, was 73% busy at ~19 ms a frame: it may be the next limit. Next, each needing
  Ege's approval: a governor (the S9's drivers/power/hakios.exynos_cpufreq.c sets rates through ACPM, which the 8890 has
  not: here the AP writes MNGS_PLL and BUCK2 itself) with touch boost, a TMU for the 8890 before rates above 1456, and
  faster G3D clocks.
- **The GPU at 650 MHz, 2026-11-08 (run 20261108T232049; approved by Ege).** `mali_mhz=650` (default in tools/mkboot.sh;
  hakios.exynos8890_g3d.c's step 2, before the GPU's first job): BUCK11 vdd_extra 750 -> 800 mV, BUCK6 600 -> 706.25 mV
  (this chip's dvs_g3dm and dvfs_g3d at 650: ASV table 10, group 10), then Samsung's transition through the 528 MHz
  switch clock (SCLK_BUS0_PLL), G3D_PLL M 100 P 4 S 0, SCLK_HPM 1 -> 2. 650 is Samsung's GPU_MAX_CLOCK for this phone.
  No faults; jobs 720 -> ~440 us each (not 2.5x: part of each job is not the GPU's clock). The glue now offers 260, 546
  and 650 to `mali_gov=1` (clock-only switches, the supplies kept at 650's), not run yet; the 8890 has no G3D
  temperature read, so mali_gov's thermal caps would not act.
- **The 8890's temperatures, 2026-11-09 (run 20261109T222318, read only; then `tmu=3`, a default).** The S9's TMU driver
  with a per-SoC glue (`hakios.exynos_tmu.h`: `hakios.exynos9810_tmu.c`, `soc/drivers/core/hakios.exynos8890_tmu.c`). The
  four blocks (MNGS 0x10060000, APOLLO 0x10064000, GPU 0x10068000, ISP 0x1006C000) run as S-Boot leaves them, two-point
  fuses, the hardware trip not armed; ~33 C at start, ~52 C idle, 70 to 76 C with all four M1s busy. `tmu=3` ends the run
  at 110 C. Nothing of the TMU is written.
- **A CPU governor, 2026-11-09 (runs 20261109T223627 and T224140; approved by Ege).** `cpufreq_gov=1` (a default,
  `exynos8890.config`): the S9's governor (load windows, touch and frame boosts, thermal ceilings) with a per-SoC glue
  (`hakios.cpufreq_glue.h`: `hakios.exynos9810_cpufreq.c` through the APM; the 8890's in `hakios.exynos8890_cpu.c`, which
  moves MNGS_PLL and BUCK2 itself). The M1s between 728 MHz (675 mV) and 2288 MHz (1012.5 mV) in Samsung's steps, each
  change ~20 to 40 us with interrupts off, the measured MHz right at every level, idle at 728. Samsung's limits kept:
  2288 with four M1s online; **1560 while the GPU runs above 600 MHz** (its gpu_dvfs_table), so with `mali_mhz=650` the
  M1s top out at 1560 and with `mali_mhz=546` at 2288; 1456 without temperatures. Cooling from 76 C: under full load at
  2288 the big zone reaches 76 C in seconds and the ceiling settles around 1456 to 1768. `cpufreq_big=1456000` is still
  the fixed rate when `cpufreq_gov=` is taken away. The S9's own configuration (hakios-starlte) now needs
  `CONFIG_HAKIOS_EXYNOS9810_CPUFREQ=y` beside `CONFIG_HAKIOS_EXYNOS_CPUFREQ=y`; the 9810 glues are built here but not
  run on an S9.
- **The GPU's clock after its load, 2026-11-09 (run 20261109T225105; approved by Ege).** `mali_gov=1 mali_gov_frame=546`
  (defaults in tools/mkboot.sh): 260, 546 or 650 MHz, clock only, the supplies kept at 650's; 546 while frames come and
  650 only above 50% load, so the M1s' 1560 cap applies only then (the CPU governor reads G3D_PLL each 40 ms window, so
  it follows a GPU rise by up to one window). The 8890 glue now gives the G3D zone's temperature for the caps (546 from
  85 C, 260 from 95). 106 switches in the first three minutes, none failed. Seen idle: something presents 10 frames a
  second, which holds the frame boosts on (GPU 546, M1s 1456) instead of 260 and 728; not yet known what draws.
  `tools/ect.py` and `tools/ectvolt.py` print Samsung's tables from the stock sboot.bin (ECT at 0x38c00).
- **Cut strokes in Freeform fixed, 2026-11-08 (run 20261108T232917).** The S6SY661 reader stopped at 32 events and lifted
  every finger: 49 times in a few minutes of drawing, each a "finger up" mid-stroke. It now reads as Samsung's
  sec_ts_read_event: until an empty or a status event, at most 128, and on overflow clears the chip's queue with the
  fingers kept. Ege: the cuts are gone. The long reads were all coordinates (17 to 128 at once, no status): the polling
  thread (priority 92, every 4 ms, I2C reads spinning, well under 1 ms each) sometimes does not run for what must be
  hundreds of ms, so touch moves arrive late and in bursts. Not yet known why; a likely part of the remaining stutter.

- **Touch arriving late: the bus ran at 11 kHz, 2026-11-09 (runs 20261109T225912, then T230647 fixed).** Timing in the
  polling thread showed every event read taking 9.15 ms (all of it in the two I2C transfers), slower than the chip makes
  events while a finger moves: rounds of 0.1 to 1.4 s, the UI that far behind the finger. `hakios.exynos_hsi2c.c` wrote
  the 9810 USI's bit masks into TIMING registers that on the 8890 hold counts; with Samsung's own calculation
  (`timing_counts`, the touch bus only; ACLK_PERIC1_66 is BUS0_PLL 528 / 8 = 66 MHz) a read is ~0.2 ms, the worst round
  of a minute 1 ms, none slow. About 0.5% of transfers get no ack from the chip and are retried. The PMIC's and MUIC's
  buses (hakios.s2mps16.c, hakios.max77854.c) still have the slow timing: a BUCK2 change costs milliseconds.

- **Where heavy frames go, 2026-11-09 (runs 20261109T232806, T233?).** App switcher, notification pull: 27 to 36
  frames a second, the render thread 25 to 31 ms a frame, the GPU 13 to 31 ms a frame shown, ~650 passes a second.
  Fragment jobs are 88% of the GPU's time (560 to 700 us each; vertex and tiler 80 to 100 us), with Reduce Transparency
  the same: pixels, not blur and not per-job overhead. Running a fragment job beside the next pass's tiler job is not
  safe on Midgard as panfrost uses it (one tiler heap for all passes; to be checked in xnu-deps/build/mesa). The M1s
  were at 100% under the governor's own thermal ceiling (1352 MHz at 72 C): the 8890 glue now gives Samsung's table
  (`clips`: 1872000 from 76 C, 1352000 from 81, 728000 from 86, each kept until 5 C under), approved by Ege; under a
  long full load the big zone then sits at 74 to 82 C between 1352 and 1872 MHz. Next: rendering at 1080 x 1920
  (450 x 800 points at 2.4), scaled to the panel by VGR1 (the driver's fb_dpp mode; needs the CFW SMC, not approved)
  or by one GPU pass.

- **Drawn at 1125 x 2000, 2026-11-09 (run 20261109T235154; not yet a default).** `fbwidth=1125 fbheight=2000` (in
  ARGS): 450 x 800 points at 2.5 pixels a point, 39% fewer pixels, enlarged to the panel by the framebuffer driver's
  per-frame copy (fb_flip already copies every frame; no GPU pass, no display write). A new canvas in
  `userland/src/display_native.c` and in the kernel's `nativeArtworkIdentity()`. Samsung's FHD itself, 1080 x 1920, is
  the iPhone Plus's panel: CoreAnimation then draws 1242 x 2208 and the screen stays black (run 20261109T234614).
  `tools/usb-shot.py` shows the panel's picture from the host. The root before it: build/root.apfs.20261108.

- **1125 x 2000 a default; idle heat, 2026-11-10 (run after 20261109T235154).** In gracerlte.config. Heavy gestures: the
  GPU 5 to 12 ms a frame shown (13 to 31 at the panel's size), mostly at 546 MHz; what is left is the M1s at Samsung's
  81 C clip (1352 MHz) with a core at 100%. Scaled, the driver drew every frame twice (unfiltered, then filtered 40 ms
  later): with the home screen's Clock icon redrawing 10 times a second the picture pulsed, and those frames held the
  frame boosts on. Now a frame that does not follow another within 50 ms (MOVING_MS, MiniIOMobileFramebuffer.cpp, in
  xnu-src: export before a commit) is drawn filtered at once and raises no clock; Ege: the pulsing is gone. A filtered
  frame costs 12 to 14 ms of one core. Page one with the Clock icon: ~44% of a core, ~61 C; page two: no frames, 728 MHz
  at 1 to 2%, 55 C after three minutes, all four sensors alike (the G3D's the warmest). The console's `touch X Y X2 Y2 MS`
  swipes from the host. Still to decide: the Clock icon's second hand; the loader's A53 (core 0) spinning in its watch
  loop (run_other_cpu's pause is a counted loop, no WFI); the GPU's supplies kept at 650 MHz's while it runs at 260.

### Phase 7: the rest, in order of value
Battery/charger readout (MAX77854) → Wi-Fi (8890 PCIe + BCM4359 brcmfmac + firmware from `/vendor/etc/wifi`) → brightness →
vibration → install-on-flash release flow (BOOT + SYSTEM, return timer kept). Audio, cameras and cellular are out of scope.

- **Wi-Fi, 2026-11-10: the radio scans; joining not yet tried.** Stage 1: `EXYNOS_PCIE` with a per-SoC glue
  (`hakios.exynos_pcie_glue.h`; the 8890's in `devices/gracerlte/soc/drivers/core/hakios.exynos8890_pcie.c`): the link
  to the BCM4359 (14e4:4415) comes up, Gen1 x1. Stage 2: Linux's brcmfmac (`tools/mk-brcmfmac.sh`; on macOS it needs a
  GNU `sha256sum -c`, a wrapper is in build/scratch/gnubin) with Samsung's firmware from the stock AP image's
  /vendor/etc/wifi in `core/fw-wifi/` (bcmdhd_sta.bin_c0_plus_blob, bcmdhd_clm.blob without which scans are refused,
  nvram.txt_murata_c0_plus: the chip's own address has Murata's prefix). The address must be known at boot
  (`fw-wifi/mac.info`, the chip's own) or wifid misses en0. Run with `wifi_pcie=6 brcmfmac=8
  wifi_nvram=nvram.txt_murata_c0_plus` (not defaults yet). Reading CMU_FSYS1 +0x854 stops the bus.
- **Layout 414 x 736 points, 2026-11-11.** At 450 points wide the keyboard's `.?123` did nothing in any app (UIKit
  resized its 414-wide keyboard on a keyplane change and rebuilt it). Now drawn at 1035x1840 (default) or 828x1472.
  `kbtrace=1` traces the keyboard, in an app too when `INJECT` gives it libhakios.display.dylib.

- **Data kept between boots, 2026-11-11 (runs 20261111T002856, 20261111T003237): works, opt-in.** Ege lifted the
  read-only rule for USERDATA only (sda23; Lineage's data erased). The UFS own mode makes `/dev/ufs_USERDATA_data`, the
  one partition it writes (anything outside its blocks is refused), and `/private/var` is a journaled HFSX volume on it:
  `ufs_native_ro=0 ufs_native_write=USERDATA var_hfs=ufs_USERDATA_data`. The first run wrote 21801 times with no error
  and was stopped with `return`; the second mounted what it left. Without the arguments `/private/var` is in RAM as
  before. `tools/phone-wipe-data.sh` (from TWRP) makes the volume empty again (not run yet). Not a default until it has
  survived more runs; bootloader, PARAM and EFS stay never written.
- **Data volume the default, 2026-11-11 (run 20261111T111546).** `tools/mkboot.sh` gives the three arguments; the end
  of a run syncs the volume ("ufs: synced (0)"). In RAM instead: `ARGS="ufs_native_ro=1 ufs_native_write= var_hfs="`.
- **Brightness, 2026-11-11 (run 20261111T112747): Control Center's slider dims the panel (Ege saw it).** Panel writes
  approved by Ege. `devices/gracerlte/drivers/gpu/hakios.s6e3ha5.c` (`panel=1`, with `brightness_bridge=1`, both from
  mkboot): the S6E3HA5 (ID 41 60 80) on DSIM0 `0x13900000`, the S9 driver's DSIM packets now shared
  (`sys/hakios.exynos_dsim.h`; the S9's side not compiled or run). Only the AOR (B1) is sent, from S-Boot's own
  (read as `64 60`, taken as 0x660) down to Samsung's dimmest 0x9E2: the slider's top is S-Boot's brightness, not the
  panel's. **Open:** brighter than S-Boot needs Samsung's gamma rows from the panel's MTP (dimming_core_hero.c;
  TWRP's `cat /sys/class/lcd/panel/aid_log` prints this panel's rows to dmesg, to check a port against; kept in
  build/scratch/panel-twrp.txt); B1's first byte had a low half of 4 that Samsung's table never has, sent as 0;
  ELVSS and VINT (supply voltages) not touched. Samsung's source: build/scratch/samsung/k.
- **Small cores: not with this kernel; cheap wins first (Ege, 2026-11-11).** The A53s have no 16 KB granule (Phase 0),
  so they need a 4 KB-page XNU. **Parked by Ege the same day**: doubtful gain (the scheduler does not tell the cores
  apart, 4 KB pages cost the M1s, the loader keeps one A53). Done instead, runs 20261111T113642 to T115912:
  - `vmd` left out (`userland/mkjobs.py` DENY): the Phone app starts it, and without CommCenter it spun on the lookup,
    a whole core and 28% of another in launchd, until the run's end.
  - The display leaves a row alone when it was scaled from the same pixels (`fScanSig`, MiniIOMobileFramebuffer.cpp):
    page one with the Clock, drawing 14 -> 4 ms a frame, kernel_task 50% -> 16% of a core, 1144 -> 728 MHz, 55 -> 50 C.
    What is left is hashing the frame and backboardd's 10% (the render itself).
  - The GPU's supplies follow its clock (`set_rate`, hakios.exynos8890_g3d.c): 600/750 mV at 260 MHz, 668.75/800 at
    546, 706.25/800 at 650, Samsung's order; 52 switches, none failed.
  - Open: a WFE pause for the loader's A53 (timer event stream, CNTKCTL_EL1). It ran and returned, but its gain was
    not measured: the run beside it showed another screen. Not in the tree. Page one against anything else is 5 C by
    itself: compare like with like.
- **Done 2026-11-11: the panel's own scaler instead of scaling on the processor (Ege: "Panel's FHD mode").** The
  S6E3HA5 enlarges 1080x1920 itself (Samsung's CONFIG_FB_DSU, `S6E3HA5_SEQ_DDI_SCALER_FHD*`, BA 02); the display's
  1035x1840 picture sits unscaled in its middle, G1 reading the drawing buffers (fb_dpp, now the SoC config's default).
  Ege: "ios is visible, thin black border. Touch is accurate. Sharp and clear." 24 swipes (run 20261111T151723
  against T120820): kernel_task 52% -> 32% of a core, backboardd 60% -> 53%, SpringBoard 30% -> 27%, big cores
  75 -> 59 C, 56-59 fps; a frame takes 8.4 ms on the link (14.6 before). Commits: device 14307f5 and 332ccd4, core 49f7d83.
  - What it took: the blender's pixel count (+0x114) with its size, or the first frame never ends; G1's sizes go
    with the ping-pong update, not SFR_UPDATE_FORCE; `WIN_PIXEL_COUNT` (+0x144) = the window's pixels; the panel's
    reads kept off the frames (below).
  - Panel reads must not meet a frame: the boot-time ID/B1 reads went out while frames ran back to back, a third of
    them timed out, and after a failed read the panel took no command (the switch sent, the loader's picture kept).
    The reads now come from the DECON's probe with its trigger masked (`hakios_s6e3ha5_boot_reads`, device commit
    after 14307f5); four boots in a row clean. The short writes (core 49f7d83) match Samsung but were not the cause.
- **Done 2026-11-11: services this phone can never use left out** (Ege: keep phone, messages and Siri, "remove the
  rest"). `userland/mkjobs.py` DENY: App Store/media/ads, iCloud/Apple ID, AirDrop/Continuity/Find My, analytics,
  online search, Maps, Wallet IDs. 146 s after boot: 138 -> 102 processes, 159 -> 134 s CPU, 505 -> 405 MB. At idle
  they were only 0.3% of a core: the idle cost is kernel_task 16%, backboardd 9%, SpringBoard 3% (5 min on the home
  screen), the next target. `nanoregistryd` must stay (SpringBoard stops after one frame without it). Not jobs, so
  not removed: the News widget (NewsTag, NewsToday2: take it off the home screen), CAReportingService,
  ACCHWComponentAuthService, WiFiCloudAssets. Console `top all` lists every process with its memory.
- **Done 2026-11-11: idle load (Ege: "Go ahead with the idle polling in the drivers").** Console `top threads` showed
  the drivers' polling was small (the DECON watcher 0.8%, Mali 0.6%, Wi-Fi ~1.5% of a core): 12% of kernel_task's 16%
  was the framebuffer copying each frame (10 a second at idle) into a scan-out buffer. Fix: fb_zc on the 8890 (device
  7607482): G1 reads the window server's surface itself, from a pool above the buffers. kernel_task 16% -> 4%, all
  processes 29.5% -> 18.4% of a core. Pool surfaces need rows on 256 bytes (core acc0539): CoreAnimation's 4160 gave
  pulsing bands. **Undone 2026-11-12 (fb_zc off, device 68d5e69):** Ege saw tearing and small glitches while
  swiping. CoreAnimation draws in turn into only two surfaces, the next while the other is still on the panel;
  with G1 reading them, the GPU wrote into the picture being scanned out (fps_log "handed over drawn into the one
  on the panel": 30-52 a second, run 20261112T125616). Fix (core dd8626e): eagl's blit into the display surface
  waits while it is in use (IOSurfaceIsInUse, raised by the display's driver while shown): 0 such frames. But
  zero-copy then swipes 51 fps and uneven against 55 copied (same 24-stroke swipe, runs T130213, T130551), and
  eagl's attach reads the surface back from write-combined memory (1.3 vs 0.3 ms a frame). So frames are copied
  again; idle pays the copy (~12% of a core). Both at once would need a third surface, or command-mode updates
  sent only on a new frame (the surface free after its own transfer), and the readback done by the GPU.
  The kernel command line holds 606 of 607 (CONFIG_CMDLINE_RUN=256): a new boot-arg needs room made first, e.g. by
  moving mkboot.sh's --with words into the device config. mali=9 is in the SoC config now; runs need no ARGS.
- **Done 2026-11-12: battery drain while plugged in (runs 20261112T150418 to T154948).** Ege: iOS drained the battery
  even on the laptop's USB. Three causes. (1) The charger stopped ~100 s into every run: S-Boot leaves the MAX77854's
  watchdog on (CNFG_00 WDTEN) and nothing fed it (DETAILS_01 0xB); hakios.max77854.c now restarts it each poll (CNFG_06
  WDTCLR, Samsung's own; approved by Ege, the charger's only write). Input stays at S-Boot's 500 mA: idle with the
  screen on it charges ~+110 mA, under load the battery still makes up the rest (boot draws ~1.4 A). (2) iOS's power
  and activity loggers (aggregated, coreduetd, biomed, dasd) held ~2.3 cores at 1872-2288 MHz and 70-76 C, idle or
  locked: their stores in the kept /private/var had times from 1970 and from 2026 (Wi-Fi's time). The calendar now
  starts at the S2MPS16's RTC (CONFIG_HAKIOS_PMIC_RTC, rtc_pmic=1: drivers/power/hakios.pmic_rtc.c with a chip glue,
  read only), right to the second, and /private/var was emptied once (Ege approved): ~6% of one core at idle, 728 MHz,
  48 C. (3) The loader's A53 spun for the whole run; it now waits with WFE on the timer's event stream (~1 ms) once
  it only watches. ufs_native_ro=1 left the device config (mkboot.sh no longer overrides it: room on the command line).
  Still open: no system sleep.
- **Done 2026-11-12: the display off while locked (run 20261112T192556; Ege: phantom touches gone).** iOS's
  kFBMethodRequestPowerChange (backboardd: 0 at "Set Blanked: YES", 1 at unblank, once each at start-up) now reaches
  hakios_scanout_power: the frame in flight finished, the DECON's trigger masked (no frames, no vsync from the
  framebuffer), the panel's display off (28), the touch chip's sense off and not polled; back on: frames, display on
  (29) after the first, sense on. Locked: big cores 728 MHz, ~1% load. Not sleep-in (10): asleep, the S6E3HA5 stops
  the timing the S6SY661 runs on, and the touch screen saw fingers nobody put down (T184419, T184949; none with the
  panel awake, T190010); Samsung's settings after sleep-out (B9 H-sync, CE FFC) took the panel off its scaler (it
  showed the loader's screen, T185335). The DECON's clocks and the DSIM stay on while dark.
- **Done 2026-11-12: the GPU no higher than 546 MHz (device 87c67f9, core 1e536cf).** Ege: the frame rate had
  dropped, even swiping the home screen. Samsung's table locks the big cores to 1560 MHz while the G3D runs above
  600; the poster wallpaper's heavier frames (GPU 5.1 -> 6.3 ms) made mali_gov take 650 most of the time, so the
  render thread (the bottleneck) went 8.5 -> 13.3 ms a frame. 650 is out of the 8890 glue's rates, mkboot.sh
  passes mali_mhz=546, mali_gov runs with two rates: 57 fps steady, big cores at 2288 (run 20261112T135243).
- **Done 2026-11-12: wallpapers without Metal (core 3e938be; Ege: "Wallpapers work now").** Stock wallpapers ship
  only 375w-812h, 393w-852h and 430w-932h; this display is 414w-736h@3x, so WallpaperKit synthesized one with Metal
  (black). `+[WKWallpaperBundle shouldLoadWallpaperBundleAtURL:]` also takes 375w-812h@3x (inject_sb.c). PosterBoard's
  home preview (`PBFHomeScreenPreviewImageBuilder _imageForView:`) renders on the CPU without a Metal device. The
  gallery needs duetexpertd (FaceSuggestions) and mobileassetd (its data vault): no longer DENY'd. The display
  library loads in PosterBoard/CollectionsPoster via dispatch_async (from the constructor: launch watchdog
  0x8badf00d); the editor had been laid out 345 points wide. Diagnostics: `applog=1 applog_name=X`, `systrace=1
  systrace_name=X`. Open: some gallery previews are PosterBoard's magenta placeholder (snapshots that need Metal:
  PBFPosterSnapshotter, PaperBoardUI's PBUICALocalRenderer); the lock screen's poster switcher still withheld.
- **Load and heat looked at, 2026-11-11 (run 20261111T110534): no fault.** Boot is one minute at full load (two with
  `/private/var` in RAM), then 2-11% and 45 C at 728 MHz. Under use the big cores reach 77-82 C and are capped to
  1352-1872 MHz; the four small cores are not online.
- **Battery readout, 2026-11-10 (run 20261110T215024): done, read only.** The MAX77854's gauge (0x36) and charger state
  (0x69: INT_OK, DETAILS_01) read every 5 s in `hakios.max77854.c` and handed to HakiosBattery (`CONFIG_HAKIOS_BATTERY=y`);
  the status bar shows the level and the bolt (23%, 3793 mV, +133 mA, 649 of 2801 mAh learnt, design 3103). The
  temperature is the battery's thermistor (channel 1 of adc@13620000, the tree's table): 31.9 C, the gauge's own register
  32 C (run 20261110T215531). The bus now has its TIMING registers in counts (`timing_counts`, as the touch's): one poll
  158 ms -> 5 ms. Render size 900x1600 tried the same day: "alright", default still 1125x2000.

## Decisions I need from you

**Settled 2026-11-08** (Ege: the phone is spare, no data to keep, "whichever is easiest"): iOS SYSTEM goes into Lineage's
`SYSTEM` partition (4.97 GB; the image is ~2.5 GB), written from TWRP, the kernel itself reading only; `/private/var` later
in `USERDATA`, as the S9 port does. IPSW: the reference's own, iPhone X 16.5 20F66
(`iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw`, 6 165 698 198 bytes), which `core/sysimage/unpack.sh` is made for.
Lineage stops booting once SYSTEM is written; TWRP (RECOVERY) stays the base of every run.

The questions as they were asked:

1. **Where iOS SYSTEM lives.** About 2.5 GB is needed. Options: overwrite Lineage's `SYSTEM` (4.97 GB, which kills Lineage), or
   carve a partition out of `USERDATA` (54 GB, a repartition that is hard to undo). Only needed by Phase 4.
2. **Which IPSW.** The reference uses iPhone X 20F66 (notch, gesture UI). The Note FE is 16:9 with a **physical Home button**, a
   natural fit for the **iPhone 8 Plus** 20F66 userspace (1242×2208 is exactly 16:9; Home-button UI). It may need some tooling
   changes. You supply the IPSW either way.
3. **Build host.** XNU + Mesa + the userspace build need the Scion toolchain/SDK (macOS or Linux) and roughly 50-100 GB free.

## What I need from you, concretely
- The IPSW (decision 2), and an OK on decision 1 before Phase 4.
- Someone at the phone when a boot doesn't come back: holding Vol-Down+Home+Power gets Download mode, Vol-Up+Home+Power gets TWRP.
