<!-- Made by tools/docs-gen.py from drivers/*/conf and the SoC and device repositories' drivers. Do not edit: change that, then run tools/docs-gen.py. -->


# Drivers

Every driver the kernel can be built with. `CONFIG_HAKIOS_<NAME>=y` in a defconfig builds it in; [Drivers in the configuration](../drivers-config.md) says how, and [Writing a driver](../porting/drivers.md) how to add one.

## Always built

| File | What |
|---|---|
| `hakios.audio.c` | /dev/hakios-audio, the sound Darwin plays, to whatever plays it; and where the camera, flashlight, motor and speaker drivers are started from |
| `hakios.scsnd.c` | SCSND: what iOS plays, to the Scion app's audio ring in the shared framebuffer (patch 1401) |
| `hakios.sccam_torch.c` | the phone's flashlight under AVF, through the Scion app's camera block (patch 1402) |
| `hakios.version.c` | kern.hakios.version, and the version in the boot log |
| `hakios.platform.c` | what the drivers share (the locks of the PMIC arbiter, the always-on processor, RPMh and the SMMU), and kern.hakios.drivers, the list of the drivers built in |
| `hakios.stubs.c` | what the rest of the kernel calls in a driver that is left out: nothing, and "not there" |
| `hakios.stubs.cpp` | the same, for the calls that are C++ (the display asking whether the GPU is done with a surface) (with XNU's `iokitcpp`) |
| `hakios.physmap.cpp` | physical memory XNU does not manage, mapped write-combined through IOKit (sys/hakios.physmap.h): the S9's DMA buffers below 4 GB (the camera's, UFS's own mode; sys/hakios.exynos9810_mem.h has where each is) (with XNU's `iokitcpp`) |
| `hakios.pci.c` | the AVF guest's PCI bus: crosvm's virtio devices are found on it (patch 1100) |
| `hakios.virtio.c` | the virtio transport (PCI and MMIO) every virtio driver goes through: queues, interrupts, configuration |
| `hakios.virtio_console.c` | the virtio console, the AVF guest's boot log to the host |
| `hakios.vgpu.c` | the virtio GPU (AVF): the 2D display path, the cursor queue and the device's interrupts (patches 1202, 1300-1305) |
| `hakios.vgpu3d.c` | SCGPU: /dev/vgpu3d, OpenGL ES through the Scion app's own gfxstream (its stream ring and doorbell) |
| `hakios.vgpu3d_xnu.c` | SCGPU's glue to XNU: memory, mapping and waiting |
| `hakios.backlight.cpp` | the panel's brightness, whichever phone: panel drivers register (hakios_panel_register), the display service's "brightness" reaches them; boot-arg backlight=1 publishes AppleARMBacklight and says what iOS sets on it, backlight=2 also takes its brightness (with XNU's `iokitcpp`) |
| `hakios.input.c` | /dev/hakios-touch, the queue of touches and buttons, and where the phone's drivers are started from |
| `hakios.net.c` | en0, an interface that is Ethernet to the rest of the kernel, for whatever link carries its frames |
| `hakios.thermal_provider.c` | the SoC's temperature, whichever phone: the driver that reads the sensors registers (hakios_thermal_register), and kern.hakios.thermal_* and hakios_thermal_max_mc() read it; THERMAL and EXYNOS_TMU both register |
| `hakios.scfb_battery.c` | the phone's battery under AVF, as the Scion app writes it into the shared framebuffer (scfb.addr=) |
| `hakios.image.c` | a file of one of the RAM disks by path, for the drivers that load firmware before there is a root |
| `hakios.sparse.c` | /dev/rsp0, a block device over an image that holds only some of its blocks (apfs_sparse=) |
| `hakios.apfs_check.c` | a walk of the APFS volume on /dev/rvblk0 through VFS, to compare with tools/apfs/apfs.py (with XNU's `apfs`) |
| `hakios.lx.c` | the kernel's side of a Linux Wi-Fi driver's object (sys/hakios.lx.h): memory, time, threads, the station. What it asks of the radio (hakios_wifi_*() of sys/hakios.input.h) is the radio driver's: WIFI's (the WCN6750) or BRCMFMAC's (the BCM4361); hakios.stubs.c answers for a kernel with neither, and for the station without WIFI_STA. The object itself is linked in only with one of those two |

## audio

| Driver | Files | Requires | Boot arguments | gracerlte | qemu | |
|---|---|---|---|---|---|
| **`HAKIOS_ADSP`**<br>the audio DSP started and linked (adsp=1, adsp_step=N); the battery's state comes through it too. Needs the DSP's firmware from the phone (fw-audio/) | `hakios.adsp.c` |  | `adsp=`, `adsp_again=`, `adsp_apr_wait=`, `adsp_keep=`, `adsp_listen=`, `adsp_new=`, `adsp_pause=`, `adsp_return=` and 3 more |  |  | |
| **`HAKIOS_SPEAKER`**<br>the two speaker amplifiers on I2C, and their watch of temperature and battery | `hakios.speaker.c` |  | `speaker_probe=` |  |  | |
| **`HAKIOS_GUARD`**<br>the speakers' guard: what the samples go through on their way to the amplifiers where no DSP protects the speakers (a high-pass, three power averages, a peak limiter); for SOUND and EXYNOS_ABOX, nothing of its own | `hakios.guard.c` |  |  |  |  | |
| **`HAKIOS_SOUND`**<br>the speakers playing (sound=N): what the DSP's services are told, and the feeder, through the speakers' guard that limits what the amplifiers get | `hakios.sound.c` | `HAKIOS_ADSP`, `HAKIOS_SPEAKER`, `HAKIOS_GUARD` | `sound=`, `sound_below=`, `sound_gain=`, `sound_guard_as=`, `sound_pause=`, `sound_secs=` |  |  | |
| **`HAKIOS_MIC`**<br>the microphones, through the audio DSP (mic=N) | `hakios.mic.c` | `HAKIOS_ADSP`, `HAKIOS_SOUND` | `mic=`, `mic_adc=`, `mic_input=`, `mic_main=`, `mic_secs=`, `mic_tap=`, `mic_test=` |  |  | |
| **`HAKIOS_VIRTIO_SOUND`**<br>QEMU's virtio sound device as the player of /dev/hakios-audio (virtio_sound=1). QEMU only | `hakios.virtio_sound.c` |  | `virtio_sound=` |  | built | |
| **`HAKIOS_MAX98512`**<br>the Galaxy S9's two speaker amplifiers (MAX98512s on PERIC1's HSI2C), handed to ABOX's UAIF1 (max98512=N: 1 a look, 2 set up and muted, 3 turned on and off by UAIF1's start and stop) | `hakios.max98512.c` | `HAKIOS_EXYNOS_HSI2C` | `max98512=`, `max98512_gain=` |  |  | |
| **`HAKIOS_EXYNOS_ABOX`**<br>the Galaxy S9's speaker path through ABOX, Samsung's audio block, with its CPU off (abox=N, the first N of 8 steps: power, clocks, pins, UAIF1, the route, RDMA0 with a tone, then /dev/hakios-audio's sink); the MAX98512s through their driver's hook (sys/hakios.exynos_abox.h); with abox_mic=1 the codec's microphone into /dev/hakios-mic through UAIF0 and WDMA1 while iOS listens | `hakios.exynos_abox.c` | `HAKIOS_GUARD`, `HAKIOS_EXYNOS_ACPM` | `abox=`, `abox_bclk_on=`, `abox_burst=`, `abox_dvfs=`, `abox_gain=`, `abox_mic=`, `abox_mmu_off=`, `abox_rdma_ctrl0=` and 8 more |  |  | |
| **`HAKIOS_CS47L93`**<br>the Galaxy S9's audio codec, a Cirrus CS47L93 on PERIC0's SPI (cs47l93=N of 6 steps: 1 a look, 2 powered and its ID read, 3 the jack, 4 its clocks, 5 the main microphone, 6 AIF1 to ABOX's UAIF0, recorded by "abox rec"); the headphones (OUT2, the jack watched) only with cs47l93_hp=1 | `hakios.cs47l93.c` | `HAKIOS_EXYNOS_SPI`, `HAKIOS_EXYNOS_ACPM` | `cs47l93=`, `cs47l93_div=`, `cs47l93_hp=`, `cs47l93_hp_vol=`, `cs47l93_mic_vol=` |  |  | |

## bluetooth

| Driver | Files | Requires | Boot arguments | gracerlte | qemu | |
|---|---|---|---|---|---|
| **`HAKIOS_BT`**<br>the Bluetooth half of the WCN6750: step one, a read-only look at its three pins (bt=1) | `hakios.bt.c` |  | `bt=`, `bt_again=` |  |  | |
| **`HAKIOS_BCM4361_BT`**<br>the Galaxy S9's Bluetooth: the BCM4361's Bluetooth half on UART_BT (out of its USI reset), powered by BT_EN on the PCAL6524 expander, HCI over H4, Broadcom's patch from the phone, then 4 Mbaud with RTS/CTS (bt=N there, 7 steps: 1 a look, 2 the expander, 3 the UART in loopback, 4 the chip powered, 5 HCI_Reset and its version, 6 the patch and the version again, 7 the rate raised and the chip kept: the USB console's "bt" and "bt scan", and /dev/cu.bluetooth, the kernel control com.apple.uart.bluetooth that iOS's BlueTool and bluetoothd open, and a "bluetooth" registry entry for bluetoothd; userland's BT=1 has iOS use it). No host stack in the kernel. Needs the phone's bcm4361B2 .hcd (its VENDOR, with ufs=2, or fw-bt/; BLUETOOTH.md of starlte) | `hakios.bcm4361_bt.c`<br>`hakios.bt_node.cpp` | `HAKIOS_EXYNOS_HSI2C` | `brcmfmac=`, `bt=`, `bt_baud=`, `bt_expander_reset=`, `bt_hcd=`, `bt_keep=`, `bt_module=`, `bt_scan=` and 4 more |  |  | |

## camera

| Driver | Files | Requires | Boot arguments | gracerlte | qemu | |
|---|---|---|---|---|---|
| **`HAKIOS_CAMERA`**<br>the cameras (cam=N): the sensors on the CCI bus and the ISP's interface. The sensors, their tables (fw-camera/) and the pins are the POCO X5 Pro's | `hakios.camera.c` |  | `cam_after=`, `cam_fake=`, `cam_fake_fps=`, `cam_frames=`, `cam_gain=`, `cam_keep_mb=`, `cam_mode=`, `cam_pattern=` and 9 more |  |  | |
| **`HAKIOS_TORCH`**<br>the flashlight: the flash LED module of the PM8350C (torch=1); it stays off while the battery is too warm | `hakios.torch.c` |  | `torch_max_s=`, `torch_secs=` |  |  | |
| **`HAKIOS_S2MPB02_TORCH`**<br>the Galaxy S9's flashlight: the S2MPB02's flash block on PERIC1's HSI2C, lit by a pin (torch=N there, the same steps as the POCO's: 1 a look, 2 once, 3 for iOS through /dev/hakios-torch) | `hakios.s2mpb02_torch.c` | `HAKIOS_EXYNOS_HSI2C` | `cam_s9=`, `torch_max_s=`, `torch_secs=` |  |  | |
| **`HAKIOS_EXYNOS_CAMERA`**<br>the Galaxy S9's front camera, stages 1 to 3 (cam_s9=N): the S5K3H1's supplies on the S2MPB02, its reset and MCLK, its model ID over I2C; the ISPPRE domain, CSIS1, its D-PHY and VC0's WDMA; one frame of the CSIS's test pattern, then one of the sensor's, 1632 x 1224 RAW10, into memory (1 a look, 2 the ID, 3 the domain, 4 the pattern, 5 the sensor's frame, auto-exposed, 6 that and then the USB console's camshot: a frame demosaiced and sent as a picture, tools/usb-shot.py --cam; 7 the sensor streaming into a ring of four frames for the run, /dev/hakios-camera for user space; devices/starlte/CAMERA.md has the plan). Its memory is 32 MB of physical memory at 0xD8000000, below 4 GB and outside XNU's RAM, mapped through IOKit | `hakios.exynos_camera.c` | `HAKIOS_EXYNOS_HSI2C` | `cam_s9=`, `cam_s9_addr=`, `cam_s9_ae=`, `cam_s9_ae_frames=`, `cam_s9_bayer=`, `cam_s9_dgain=`, `cam_s9_exp=`, `cam_s9_gain=` and 7 more |  |  | |

## core

| Driver | Files | Requires | Boot arguments | gracerlte | qemu | |
|---|---|---|---|---|---|
| **`HAKIOS_SOCINFO`**<br>the chip's identity from the firmware's shared memory: kern.hakios.soc_* (socinfo=1). Any Qualcomm SoC that keeps SMEM item 137; where the shared memory is comes from soc_smem= | `hakios.socinfo.c` |  | `soc_smem=`, `soc_smem_size=`, `socinfo=` |  |  | |
| **`HAKIOS_EXYNOS_HSI2C`**<br>an Exynos 9810 HSI2C (the I2C master of a USI block), for the Galaxy S9's touch screen and battery (sys/hakios.exynos_hsi2c.h); nothing of its own on the command line | `hakios.exynos_hsi2c.c` |  |  | built |  | |
| **`HAKIOS_EXYNOS_SPI`**<br>an Exynos 9810 SPI master (the SPI of a USI block), polled, the chip select by hand, for the Galaxy S9's audio codec (sys/hakios.exynos_spi.h); nothing of its own on the command line | `hakios.exynos_spi.c` |  |  |  | built | |
| **`HAKIOS_EXYNOS_ACPM`**<br>the Exynos 9810's power processor asked through its ACPM firmware: the S2MPS18 PMIC's registers and the clocks' DVFS rates (sys/hakios.exynos_acpm.h), for the Galaxy S9's GPU; nothing of its own on the command line | `hakios.exynos_acpm.c` |  |  |  |  | |
| **`HAKIOS_EXYNOS_M3`**<br>the Galaxy S9's big cores (four Exynos M3s): the six CLUSTER1 fields of the PMU that Samsung's kernel writes before it starts any other core, and the cluster's state logged before CPU_ON (m3_prep=N, the first N of 3 steps: read, write, start the M3s; patch 1508 asks it before every CPU_ON) and cohtest=N, a check that the big cluster is coherent with the A55s once started (cached ping-pong, cache maintenance by VA across the clusters, read back through an uncached alias), off by default | `hakios.exynos_m3.c` | `HAKIOS_EXYNOS_ACPM` | `cohtest=`, `cohtest_delay=`, `m3_prep=` |  |  | |
| **`HAKIOS_USB_GADGET`**<br>a USB device link for phones with a DesignWare DWC3: the kernel's log streamed to a host over a CDC-ACM serial port (/dev/ttyACM0) and a line console back, in steps (usb_gadget=N, 5 for all); the SoC's half is a glue driver (EXYNOS_USB). Device-neutral; descriptors and requests in hakios.usb_acm.h | `hakios.dwc3.c` |  | `usb_gadget=`, `usb_gadget_clk=`, `usb_gadget_crlf=`, `usb_gadget_drain=`, `usb_gadget_fs=`, `usb_gadget_ldo=`, `usb_gadget_poll=`, `usb_gadget_soc=` and 3 more | built |  | |
| **`HAKIOS_EXYNOS_USB`**<br>the Galaxy S9's USB port, the SoC's half for USB_GADGET: the Exynos 9810's PMU isolation, clocks and USB 2.0 PHY (the BSP's USB 3.1 CAL sequence), the SuperSpeed side held idle; usb_gadget_clk=1, usb_gadget_ldo=1 | `hakios.exynos_usb.c` | `HAKIOS_USB_GADGET`, `HAKIOS_EXYNOS_ACPM` |  |  |  | |

## gpu

| Driver | Files | Requires | Boot arguments | gracerlte | qemu | |
|---|---|---|---|---|---|
| **`HAKIOS_ADRENO`**<br>the Adreno 642L of the SC7280 woken step by step (adreno=N), and HakiosGPU, the IOKit service Mesa and the window server draw through (gpu=1, adreno_keep=1). Needs the GPU's firmware from the phone (fw-gpu/) | `hakios.adreno.c`<br>`hakios.gpu.cpp` |  | `adreno=`, `adreno_blind=`, `adreno_dirty=`, `adreno_fine=`, `adreno_keep=`, `adreno_only=`, `adreno_pause=`, `adreno_return=` and 12 more |  |  | |
| **`HAKIOS_MALI`**<br>a Mali GPU brought up step by step (mali=N, 9 for all), the Galaxy S9's G72 or the Note FE's T880, its SoC's side a glue (mali_soc=: exynos9810, exynos8890); and HakiosMali, the IOKit service Mesa's panfrost draws through; mali_gov=1 moves its clock after its load where the glue can. HakiOS's Mali-G52 driver of the Infinix Hot 20, ported | `hakios.mali.c`<br>`hakios.mali.cpp` |  | `mali=`, `mali_cache_profile=`, `mali_calls=`, `mali_fastmap=`, `mali_flush=`, `mali_gov=`, `mali_gov_frame=`, `mali_nap_us=` and 5 more | built |  | |
| **`HAKIOS_EXYNOS9810_G3D`**<br>the Exynos 9810's side of the Galaxy S9's Mali-G72 (mali_soc=exynos9810, the default): the G3D's supply through ACPM, PLL_G3D (260, 455, 572 MHz for mali_gov) and its power domain | `hakios.exynos9810_g3d.c` | `HAKIOS_MALI`, `HAKIOS_EXYNOS_ACPM` | `mali_acpm=`, `mali_mhz=`, `mali_pll=`, `mali_uv=` |  |  | |
| **`HAKIOS_EXYNOS_DPU`**<br>the Galaxy S9's display controller as the display's scan-out: frames flipped between buffers at the DECON's refresh, no tearing (fb_flip=1), and the iPhone's picture enlarged to the panel by the DPP's VGF0 scaler instead of the processor (fb_dpp=1); with fb_zc=1 the window server's display surfaces are made of its pool and scanned out as they are, nothing copied; fps_log=1 reads its refresh count. Off unless asked for | `hakios.exynos_dpu.c` |  | `fb_aspect=` |  |  | |
| **`HAKIOS_S6E3HA8`**<br>the Galaxy S9's panel brightness through DSIM0's command FIFO: the gamma worked out from the panel's MTP as Samsung's kernel does (hakios.s6e3ha8_gamma.h) and the AOR, Samsung's tables for each level (panel=1; kern.hakios.brightness 0-255, panel_brightness=N at start; panel_gamma=0 the AOR alone) | `hakios.s6e3ha8.c` |  | `panel=`, `panel_brightness=`, `panel_gamma=`, `panel_gamma_log=` |  |  | |

## haptic

| Driver | Files | Requires | Boot arguments | gracerlte | qemu | |
|---|---|---|---|---|---|
| **`HAKIOS_HAPTIC`**<br>the vibration motor: an Awinic AW86927 driving a linear resonant actuator (haptic=N), its waveforms from fw-haptic/ | `hakios.haptic.c` |  | `haptic=`, `haptic_effect=` |  |  | |

## input

| Driver | Files | Requires | Boot arguments | gracerlte | qemu | |
|---|---|---|---|---|---|
| **`HAKIOS_VIRTIO_INPUT`**<br>QEMU's virtio tablet and keyboard as the touch screen and the buttons (virtio_input=1). QEMU only | `hakios.virtio_input.c` |  | `virtio_input=` |  | built | |
| **`HAKIOS_GOODIX`**<br>the touch screen: a Goodix GT9916 ("Berlin-D") or a FocalTech FT3680 on SPI (touchscreen=1). The bus, pins and supplies are the POCO X5 Pro's; another phone with one of these controllers changes those | `hakios.goodix.c` |  | `touch_flip=`, `touch_poll_us=`, `touch_rate=`, `touch_spi_hz=`, `touchscreen=` |  |  | |
| **`HAKIOS_BUTTONS`**<br>the power and volume keys, read from the PMIC through the SPMI arbiter (buttons=1) | `hakios.buttons.c` |  | `buttons=`, `buttons_steps=`, `press_power=`, `press_power_ms=`, `touchscreen=` |  |  | |
| **`HAKIOS_EXYNOS_KEYS`**<br>a Samsung phone's power, volume and Bixby/Home keys, pins of the SoC's ALIVE GPIO block (exynos_keys=1; exynos_keys_board=: starlte, gracerlte). Bixby or Home is the home button | `hakios.exynos_keys.c` |  | `exynos_keys=`, `exynos_keys_board=` | built |  | |
| **`HAKIOS_S6SY761`**<br>the Galaxy S9's touch screen: a Samsung S6SY761 on the Exynos 9810's HSI2C of USI03, the bus driven by this file (s6sy761=1; s6sy761_steps=N, s6sy761_ipclk=MHz); the Note FE's S6SY661 with s6sy761_board=gracerlte. The S9's supply cycle needs EXYNOS_ACPM | `hakios.s6sy761.c` | `HAKIOS_EXYNOS_HSI2C` | `s6sy761=`, `s6sy761_board=`, `s6sy761_ipclk=`, `s6sy761_ldo=`, `s6sy761_pri=`, `s6sy761_steps=`, `s6sy761_trace=` | built |  | |
| **`HAKIOS_SSP`**<br>the Galaxy S9's sensors behind its sensor hub, the MCU of the BCM4775 GNSS chip on USI_CHUB00's SPI: powered, its firmware loaded through the chip's ROM, Samsung's sensor protocol (SSP), the accelerometer, gyroscope, magnetometer, barometer, light and proximity sensors read into /dev/hakios-sensors (shub=N, 6 steps: 1 a look, 2 powered, 3 the ROM's version, 4 the firmware, 5 SSP, 6 the sensors on). Needs fw/bcm_sensorhub_a1.bin (devices/starlte/firmware/sensorhub/) for step 4 | `hakios.ssp.c` | `HAKIOS_EXYNOS_SPI`, `HAKIOS_EXYNOS_ACPM` | `shub=`, `shub_chub_on=`, `shub_div=`, `shub_hello=`, `shub_keep=`, `shub_log=`, `shub_ms=`, `shub_req_len=` and 2 more |  |  | |

## modem

| Driver | Files | Requires | Boot arguments | gracerlte | qemu | |
|---|---|---|---|---|---|
| **`HAKIOS_QMI`**<br>QMI messages over the IPC router: how the Wi-Fi firmware's service and the modem's are spoken to | `hakios.qmi.c` |  |  |  |  | |
| **`HAKIOS_MODEM`**<br>the modem (modem=1): started, its file systems served from the flash, the IPA's data path as en1, the SIM and the network registration. Research; needs fw-modem/ from the phone | `hakios.mpss.c`<br>`hakios.cell.c`<br>`hakios.nas.c`<br>`hakios.ipa.c` | `HAKIOS_QMI`, `HAKIOS_UFS` | `modem=`, `modem_again=`, `modem_apn=`, `modem_buffer=`, `modem_data_nobind=`, `modem_data_off=`, `modem_data_wait=`, `modem_dpm_off=` and 45 more |  |  | |

## net

| Driver | Files | Requires | Boot arguments | gracerlte | qemu | |
|---|---|---|---|---|---|
| **`HAKIOS_VIRTIO_NET`**<br>QEMU's virtio network device as the link behind en0 (virtio_net=1). QEMU only | `hakios.virtio_net.c` | `HAKIOS_WIFI_STA` | `virtio_net=`, `virtio_net_ssid=`, `virtio_net_wifi=` |  | built | |

## power

| Driver | Files | Requires | Boot arguments | gracerlte | qemu | |
|---|---|---|---|---|---|
| **`HAKIOS_BATTERY`**<br>the battery: kern.hakios.battery_*, and HakiosBattery, the IOPMPowerSource Darwin reads the charge from (the state comes from the audio DSP's battery manager, with adsp=1) | `hakios.battery.c`<br>`hakios.battery.cpp` |  | `battery_shared_pa=` | built |  | |
| **`HAKIOS_CPUFREQ`**<br>the clock of the cores the kernel runs on (cpufreq=1), and the memory bus's votes (ddr=) | `hakios.cpufreq.c` |  | `cpufreq=`, `cpuload=`, `ddr=`, `ddr_rest=`, `ddr_vote=`, `membench=` |  |  | |
| **`HAKIOS_THERMAL`**<br>the SoC's temperature sensors (thermal=1): kern.hakios.thermal_*, and the run handed back to the loader when the phone gets too hot (thermal_stop=) | `hakios.thermal.c` |  | `thermal=`, `thermal_log=`, `thermal_poll=`, `thermal_stop=`, `thermal_tm0=`, `thermal_tm1=` |  |  | |
| **`HAKIOS_PMIC`**<br>the blocks of the power-management chips the SPMI arbiter shows, and the PMIC's clock (pmic=1); read only | `hakios.pmic.c` |  | `pmic=`, `pmic_list=` |  |  | |
| **`HAKIOS_PMIC_RTC`**<br>the phone's real-time clock read at start-up (rtc_pmic=1): the calendar starts at its time when it is valid and not behind epoch=; the chip's side a glue (hakios.pmic_rtc.h: S2MPS18_RTC, the Note FE's S2MPS16); read only | `hakios.pmic_rtc.c` |  | `rtc_pmic=` | built |  | |
| **`HAKIOS_S2MPS18_RTC`**<br>the Galaxy S9's real-time clock, the S2MPS18 PMIC's RTC read through ACPM: the chip's side of PMIC_RTC; read only, nothing written to the PMIC | `hakios.s2mps18_rtc.c` | `HAKIOS_EXYNOS_ACPM`, `HAKIOS_PMIC_RTC` |  |  |  | |
| **`HAKIOS_BATTGUARD`**<br>the run handed back to the loader before the battery is flat (battery_stop=<percent>) | `hakios.battguard.c` |  | `battery_stop=`, `battery_stop_poll=` |  |  | |
| **`HAKIOS_EXYNOS_CPUFREQ`**<br>an Exynos's CPU frequency governor (cpufreq_gov=1): the clusters' rates moved with the load, the touch screen and the display's frames, within Samsung's limits, and lowered when the TMU reads them hot; the SoC's side a glue (cpufreq_soc=: exynos9810, exynos8890) | `hakios.exynos_cpufreq.c` |  | `cpufreq_gov=`, `cpufreq_gov_big_max=`, `cpufreq_gov_big_min=`, `cpufreq_gov_lit_max=`, `cpufreq_gov_lit_min=`, `cpufreq_gov_log=`, `cpufreq_gov_ms=`, `cpufreq_soc=` and 7 more | built |  | |
| **`HAKIOS_EXYNOS9810_CPUFREQ`**<br>the Exynos 9810's side of the governor (cpufreq_soc=exynos9810, the default): the Galaxy S9's two clusters, a rate set through ACPM's DVFS, the device tree's limits for the M3s online | `hakios.exynos9810_cpufreq.c` | `HAKIOS_EXYNOS_CPUFREQ`, `HAKIOS_EXYNOS_ACPM` |  |  |  | |
| **`HAKIOS_MAX77705`**<br>the Galaxy S9's battery: the fuel gauge and charger of its MAX77705, read (never written) over CMGP's HSI2C and handed to HakiosBattery, the power source iOS reads (max77705=1; max77705_steps=N, max77705_ipclk=MHz); and its haptic block, the vibration motor for iOS through /dev/hakios-haptic (max77705_haptic=1) | `hakios.max77705.c` | `HAKIOS_EXYNOS_HSI2C`, `HAKIOS_BATTERY` | `max77705=`, `max77705_haptic=`, `max77705_ipclk=`, `max77705_steps=`, `max77705_usbsw=` |  |  | |
| **`HAKIOS_EXYNOS_TMU`**<br>an Exynos's temperatures: its TMU (BIG, LITTLE, G3D, ISP), the SoC's side a glue (tmu_soc=: exynos9810, exynos8890), read with each sensor's fuses (tmu=1), published as kern.hakios.temp.* and the POCO's kern.hakios.thermal_* (tmu=2), and the run ended or the phone powered off when a zone or the battery is too hot (tmu=3; tmu_stop=, tmu_batt_stop=, tmu_off=1) | `hakios.exynos_tmu.c` |  | `tmu=`, `tmu_batt_stop=`, `tmu_enable=`, `tmu_log=`, `tmu_off=`, `tmu_poll=`, `tmu_soc=`, `tmu_stop=` and 1 more | built |  | |
| **`HAKIOS_EXYNOS9810_TMU`**<br>the Exynos 9810's side of the TMU driver (tmu_soc=exynos9810, the default): the Galaxy S9's two blocks, the TMU's clock gate, tmu_enable's start and tmu_off's PS_HOLD | `hakios.exynos9810_tmu.c` | `HAKIOS_EXYNOS_TMU`, `HAKIOS_EXYNOS_POWEROFF` |  |  |  | |
| **`HAKIOS_EXYNOS_POWEROFF`**<br>the Galaxy S9 powered off: PS_HOLD let go in the Exynos 9810's PMU, for the TMU's tmu_off=1 and for a halt (halt_off=1: the panel blanked first, DCS display-off and sleep-in through DSIM0; halt_off=2: the APM stopped too) | `hakios.exynos_poweroff.c` |  | `halt_off=` |  | built | |

## storage

| Driver | Files | Requires | Boot arguments | gracerlte | qemu | |
|---|---|---|---|---|---|
| **`HAKIOS_UFS`**<br>the phone's UFS flash: a block device for each GPT partition (ufs=1, ufs=2), and the UFS host controller driven from the kernel (ufs_native=): beside UEFI's (1, the POCO), or its own for the run (4; QEMU's -device ufs, the S9's with ufs_native_soc=exynos9810 as UEFI left it, never reset: on the phone through ufs=3), in steps (ufs_native_step=1 look, 2 the link, 3 NOP OUT, 4 READ CAPACITY, 5 the GPTs; the S9's default 1). THE ONLY CODE OF THE KERNEL THAT MAY WRITE TO THE PHONE (ufs_write=1, the partition `darwin` only; ufs_native_write=, one partition, never EFS, BOOT, PARAM ...): see tools/provenance.allow. ufs=3 (the S9's default, seen working on the phone): the loader serves as with ufs=2, and the kernel's own driver takes the controller after a self-test against the loader's reads (LBA 0, the GPTs, three blocks of every partition), falling back to ufs=2 on any difference; every write fenced in the whitelist itself (USERDATA only, with ufs_write=1; never EFS, RADIO, BOOT, the GPTs, LBA 0-5, a boot unit); ufs_native_ro=1 all read only. And a file of one of its ext4 partitions, read only (hakios.ext4.c: the S9's VENDOR, where its Wi-Fi and Bluetooth firmware is, read at run time; tools/ext4-test.py) | `hakios.ufs.c`<br>`hakios.ufshc.c`<br>`hakios.ext4.c` |  | `fps_log=`, `ufs_bench=`, `ufs_check=`, `ufs_native_bench=`, `ufs_native_buffer=`, `ufs_native_check=`, `ufs_native_hang=`, `ufs_native_leave=` and 12 more | built | built | |
| **`HAKIOS_VIRTIO_BLK`**<br>a disk image of the host as /dev/rvblk0 (virtio_blk=1). QEMU only | `hakios.virtio_blk.c` |  | `virtio_blk=`, `virtio_blk_rw=` |  | built | |

## wifi

| Driver | Files | Requires | Boot arguments | gracerlte | qemu | |
|---|---|---|---|---|---|
| **`HAKIOS_WIFI`**<br>Wi-Fi of the WCN6750 (wifi=1, wifi_step=N): the DSP that runs the radio's firmware, Linux's ath11k (its object, made by tools/mk-ath11k.sh, is linked in only with this), a station, and the Apple80211 surface Darwin's wifid talks to. Needs fw-wifi/ from the phone | `hakios.wifi.c`<br>`hakios.wlfw.c` | `HAKIOS_QMI`, `HAKIOS_WIFI_STA` | `wifi_again=`, `wifi_ask=`, `wifi_cal=`, `wifi_dirty=`, `wifi_file=`, `wifi_fine=`, `wifi_keep=`, `wifi_listen=` and 13 more |  |  | |
| **`HAKIOS_WIFI_STA`**<br>the station and the Apple80211 surface Darwin's wifid talks to, over whichever link is below: the WCN6750's (WIFI) or QEMU's virtio network device seen as Wi-Fi (VIRTIO_NET, virtio_net_wifi=1) | `hakios.sta.c`<br>`hakios.apple80211.c`<br>`hakios.wlan.cpp` |  |  | built | built | |
| **`HAKIOS_EXYNOS_PCIE`**<br>an Exynos phone's Wi-Fi, stage 1: its PCIe root complex (DesignWare and Samsung's PHY), WLAN_REG_ON and PERST to BRCMFMAC's driver); the SoC's side a glue (EXYNOS9810_PCIE, the Note FE's EXYNOS8890_PCIE) | `hakios.exynos_pcie.c` |  | `wifi_pcie=`, `wifi_pcie_tries=`, `wifi_pcie_ungate=` | built |  | |
| **`HAKIOS_EXYNOS9810_PCIE`**<br>the Exynos 9810's side of the PCIe driver: the Galaxy S9's channel 0 to the BCM4361, its clock gates, PHY tables and SysMMU (devices/starlte/WIFI.md) | `hakios.exynos9810_pcie.c` | `HAKIOS_EXYNOS_PCIE` | `wifi_pcie_coherent=`, `wifi_pcie_mmu_bypass=`, `wifi_pcie_mmu_probe=` |  | built | |
| **`HAKIOS_BRCMFMAC`**<br>the Galaxy S9's Wi-Fi, stage 2: Linux's brcmfmac on the BCM4361 (its object, made by tools/mk-brcmfmac.sh, is linked in with this unless WIFI's is), on the device stage 1 hands over (wifi_pcie=6, brcmfmac=N, 8 steps: 4 the firmware's interface, 5 INTx, 6 the phone's address, 7 the station, 8 joining), with WIFI_STA above it (without it there is no station to give Darwin), and the radio unpowered on the way back to the loader. Needs the firmware, NVRAM and CLM of the user's own phone in fw-wifi/ (WIFI.md of starlte) | `hakios.brcmfmac.c` | `HAKIOS_EXYNOS_PCIE` | `brcmfmac=`, `wifi_fw=`, `wifi_mac=`, `wifi_nvram=`, `wifi_psk=`, `wifi_ssid=`, `wifi_vendor=`, `wifi_vendor_part=` | built |  | |

## core (devices/gracerlte/soc)

| Driver | Files | Requires | Boot arguments | gracerlte | qemu | |
|---|---|---|---|---|---|
| **`HAKIOS_EXYNOS8890_CPU`**<br>the Exynos 8890's cores: Samsung's EL3 grants CPU_ON and leaves the core off, so the PMU powers it after each granted CPU_ON (patch 1513); acts on the cores cpus= gives the kernel | `hakios.exynos8890_cpu.c` |  |  | built |  | |
| **`HAKIOS_EXYNOS8890_USB`**<br>the Exynos 8890's USB port, the SoC's half for USB_GADGET (usb_gadget_soc=exynos8890): the PMU's isolation, the USB3 DRD PHY's control (Samsung's CAL sequence), the clocks as S-Boot leaves them | `hakios.exynos8890_usb.c` | `HAKIOS_USB_GADGET` |  | built |  | |
| **`HAKIOS_EXYNOS8890_DECON`**<br>the Exynos 8890's DECON_F as the display's scan-out (MiniIOMobileFramebuffer.cpp, fb_flip=1): frames flipped between buffers at a refresh on S-Boot's window and VPP channel; the S9's EXYNOS_DPU for this SoC | `hakios.exynos8890_decon.c` |  |  | built |  | |
| **`HAKIOS_EXYNOS8890_G3D`**<br>the Exynos 8890's side of the Note FE's Mali-T880 (mali_soc=exynos8890): the G3D block as S-Boot leaves it, read (the domain, G3D_PLL, BUCK6 through the S2MPS16 driver, the ASV fuses); with mali_mhz=546 or 650 the supplies raised to 650 MHz's and G3D_PLL moved (Samsung's table, this chip's ASV), the rates offered to mali_gov | `hakios.exynos8890_g3d.c` | `HAKIOS_MALI` |  | built |  | |
| **`HAKIOS_EXYNOS8890_TMU`**<br>the Exynos 8890's side of the TMU driver (tmu_soc=exynos8890): the Note FE's four blocks (MNGS, APOLLO, GPU, ISP), one sensor each, read only | `hakios.exynos8890_tmu.c` | `HAKIOS_EXYNOS_TMU` |  | built |  | |
| **`HAKIOS_EXYNOS8890_PCIE`**<br>the Exynos 8890's side of the PCIe driver: the Note FE's channel 0 to its BCM4359 (the clock muxes of CMU_FSYS1, the PHY's tables), stage 1 | `hakios.exynos8890_pcie.c` | `HAKIOS_EXYNOS_PCIE` |  | built |  | |

## gpu (devices/gracerlte)

| Driver | Files | Requires | Boot arguments | gracerlte | qemu | |
|---|---|---|---|---|---|
| **`HAKIOS_S6E3HA5`**<br>the Galaxy Note FE's panel brightness through DSIM0's command FIFO: the AOR alone, from S-Boot's level down (panel=1; kern.hakios.brightness 0-255, panel_brightness=N at start) | `hakios.s6e3ha5.c` |  |  | built |  | |

## power (devices/gracerlte)

| Driver | Files | Requires | Boot arguments | gracerlte | qemu | |
|---|---|---|---|---|---|
| **`HAKIOS_MAX77854`**<br>the MAX77854 (charger, fuel gauge, MUIC) on HSI2C 0x13650000: the MUIC's USB switch and status and the charger's SAFEOUT control as the boot left them (max77854=1); max77854_usbsw=1 switches D+/D- to the AP's USB; the battery, its gauge and the charger's state read for HakiosBattery, the power source iOS reads; the charger's watchdog restarted each poll | `hakios.max77854.c` | `HAKIOS_EXYNOS_HSI2C`, `HAKIOS_BATTERY` |  | built |  | |
| **`HAKIOS_S2MPS16`**<br>the S2MPS16 PMIC on HSI2C 0x10550000: the touch screen's supplies (LDO32, LDO33) read, for the S6SY761 driver's gracerlte board; s2mps16_tsp=1 switches them on; its real-time clock read for PMIC_RTC (rtc_pmic=1) | `hakios.s2mps16.c` | `HAKIOS_EXYNOS_HSI2C` |  | built |  | |

The columns on the right are the devices' defconfigs: "built" means that defconfig builds the driver in.
