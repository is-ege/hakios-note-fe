/*
 * The Exynos 8890 Galaxy Note FE's physical memory outside XNU's RAM that this port uses: one table, as
 * hakios.exynos9810_mem.h is for the S9. XNU has the high bank (0x880000000); the flash controller's and the display's DMA
 * take 32-bit addresses, so they get pieces of the lower bank.
 *
 *   0x94000000 - 0x96FFFFFF   the DECON's scan-out buffers (fb_flip's or fb_dpp's three), and above them fb_zc's pool
 *                             (hakios.exynos8890_decon.c)
 *   0xD8000000 - 0xD9067FFF   S-Boot's framebuffer (s3cfb.bootloaderfb=, 1440 x 2560 x 4), inside the tree's camera
 *   0xDEC00000 - 0xDEFFFFFF   UFS's own-mode list, descriptors and buffers (hakios.ufshc.c)
 *   0xE0000000 - 0xE3FFFFFF   the loader's moved payload and the panic log (xnuloader/loader.c, payload_home)
 *   0xF0000000 - 0xF87FFFFF   modem_if (the loader's keep_out)
 *
 * The scan-out buffers are in the free block above Samsung's logs (0x91200000 + 32 MB, shim memmap.c), far below anything
 * the shim hands out. UFS's range is the bottom of the free block between the camera's pool and modem_if: the shim hands
 * out pages from the top of that block (shim.c, efi_allocate_pages) and the payload sits between, so the loader never
 * reaches it.
 */
#ifndef _SYS_HAKIOS_EXYNOS8890_MEM_H_
#define _SYS_HAKIOS_EXYNOS8890_MEM_H_

#define EXYNOS8890_MEM_DPU_SCAN_PA      0x94000000ULL
#define EXYNOS8890_MEM_DPU_SCAN_END     0x97000000ULL
#define EXYNOS8890_MEM_FB_PA            0xD8000000ULL
#define EXYNOS8890_MEM_FB_END           0xD9068000ULL
#define EXYNOS8890_MEM_UFS_PA           0xDEC00000ULL
#define EXYNOS8890_MEM_UFS_END          0xDF000000ULL
#define EXYNOS8890_MEM_PAYLOAD_PA       0xE0000000ULL
#define EXYNOS8890_MEM_PAYLOAD_END      0xE4000000ULL
#define EXYNOS8890_MEM_MODEM_PA         0xF0000000ULL
#define EXYNOS8890_MEM_MODEM_END        0xF8800000ULL

#define EXYNOS8890_MEM_SIZE(name)       (EXYNOS8890_MEM_##name##_END - EXYNOS8890_MEM_##name##_PA)

#define EXYNOS8890_MEM_BEFORE(a, b)     (EXYNOS8890_MEM_##a##_END <= EXYNOS8890_MEM_##b##_PA)
_Static_assert(EXYNOS8890_MEM_BEFORE(DPU_SCAN, FB), "exynos8890 memory: the scan-out buffers run into the framebuffer");
_Static_assert(EXYNOS8890_MEM_DPU_SCAN_PA >= 0x93200000ULL, "exynos8890 memory: the scan-out buffers run into Samsung's logs");
_Static_assert(EXYNOS8890_MEM_BEFORE(FB, UFS), "exynos8890 memory: the framebuffer runs into UFS's");
_Static_assert(EXYNOS8890_MEM_BEFORE(UFS, PAYLOAD), "exynos8890 memory: UFS's runs into the payload");
_Static_assert(EXYNOS8890_MEM_BEFORE(PAYLOAD, MODEM), "exynos8890 memory: the payload runs into modem_if");
_Static_assert(EXYNOS8890_MEM_SIZE(UFS) == (4ULL << 20), "exynos8890 memory: UFS's is the 4 MB its driver lays out");

#endif /* _SYS_HAKIOS_EXYNOS8890_MEM_H_ */
