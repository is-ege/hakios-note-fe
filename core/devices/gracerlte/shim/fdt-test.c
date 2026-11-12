/*
 * Host test of the shim's device-tree reader and memory map (fdt.c, memmap.c): `make test`.
 *
 *   fdt-test FILE.dtb      prints what the shim would see, and checks it:
 *                          - every free range is in whole 2 MB blocks, in RAM, and meets no reservation;
 *                          - a 2 MB block with a no-map region in it is not mapped cacheable;
 *                          - 0xF0000000 + 64 MB, where the loader takes its payload, is RAM.
 *
 * With the Note FE's tree (tests/gracerlte-rev0b.dtb, entry 0x0b of Lineage's dt.img) it also checks the Exynos 8890's
 * rules: Samsung's log regions kept, the CP's region the only claim on the payload's window, where the loader puts
 * itself and the payload free, the high bank whole.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fdt.h"
#include "memmap.h"

static int
free_has(const struct memmap *m, uint64_t base, uint64_t size)
{
	for (int i = 0; i < m->nfree; i++) {
		if (base >= m->free[i].base && base + size <= m->free[i].base + m->free[i].size) {
			return 1;
		}
	}
	return 0;
}

static int
free_meets(const struct memmap *m, uint64_t base, uint64_t size)
{
	for (int i = 0; i < m->nfree; i++) {
		if (base < m->free[i].base + m->free[i].size && m->free[i].base < base + size) {
			return 1;
		}
	}
	return 0;
}

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

int
main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: fdt-test FILE.dtb\n");
		return 2;
	}
	FILE *f = fopen(argv[1], "rb");
	if (!f) {
		perror(argv[1]);
		return 2;
	}
	static unsigned char blob[1 << 20];
	size_t n = fread(blob, 1, sizeof(blob), f);
	fclose(f);

	struct fdt_info fi;
	int r = fdt_parse(blob, &fi);
	printf("fdt_parse: %d, %zu bytes (header says %u)\n", r, n, fi.total_size);
	if (r != 0) {
		return 1;
	}
	printf("model %s; compatible %s; bootargs %s\n", fi.model ? fi.model : "-", fi.compatible ? fi.compatible : "-",
	    fi.bootargs ? fi.bootargs : "-");
	printf("uart: kind %d at %#llx shift %u; fb: %d at %#llx %ux%u stride %u\n", fi.uart.kind,
	    (unsigned long long)fi.uart.base, fi.uart.shift, fi.fb.ok, (unsigned long long)fi.fb.base, fi.fb.width,
	    fi.fb.height, fi.fb.stride);
	for (int i = 0; i < fi.nmem; i++) {
		printf("memory %#llx + %#llx\n", (unsigned long long)fi.mem[i].base, (unsigned long long)fi.mem[i].size);
	}
	for (int i = 0; i < fi.nrsv; i++) {
		printf("reserved %#llx + %#llx%s%s\n", (unsigned long long)fi.rsv[i].base, (unsigned long long)fi.rsv[i].size,
		    fi.rsv[i].flags & RSV_NOMAP ? " no-map" : "", fi.rsv[i].flags & RSV_MEMRESERVE ? " (memreserve)" : "");
	}

	/* the shim's own reservations, as S-Boot places them: the image at 0x80080000, the tree at 0x8a000000 */
	struct fdt_range extra[3] = {
		{ 0x80080000, 0x02000000, 0 },
		{ 0x8a000000, fi.total_size, 0 },
		{ 0xF0000000, 0x04000000, 0 },
	};
	static struct memmap mm;
	memmap_build(&mm, &fi, extra, 3);
	unsigned long long total = 0, biggest = 0;
	for (int i = 0; i < mm.nfree; i++) {
		struct fdt_range *fr = &mm.free[i];
		printf("free %#llx + %#llx (%llu MB)\n", (unsigned long long)fr->base, (unsigned long long)fr->size,
		    (unsigned long long)(fr->size >> 20));
		total += fr->size;
		if (fr->size > biggest) {
			biggest = fr->size;
		}
		CHECK((fr->base & (BLOCK - 1)) == 0 && (fr->size & (BLOCK - 1)) == 0 && fr->size != 0, "free %#llx + %#llx not whole 2 MB blocks",
		    (unsigned long long)fr->base, (unsigned long long)fr->size);
		CHECK(memmap_in_ram(&mm, fr->base, fr->size), "free %#llx not in RAM", (unsigned long long)fr->base);
		CHECK(!memmap_overlaps_reserved(&mm, fr->base, fr->size), "free %#llx + %#llx meets a reservation",
		    (unsigned long long)fr->base, (unsigned long long)fr->size);
		for (uint64_t b = fr->base; b < fr->base + fr->size; b += BLOCK) {
			CHECK(memmap_block(&mm, b) == BLOCK_NORMAL, "free block %#llx not mapped cacheable", (unsigned long long)b);
		}
		if (i > 0) {
			CHECK(fr->base >= mm.free[i - 1].base + mm.free[i - 1].size, "free ranges out of order or overlapping");
		}
	}
	printf("free: %llu MB in %d ranges, the largest %llu MB\n", total >> 20, mm.nfree, biggest >> 20);
	for (int i = 0; i < mm.nrsv; i++) {
		if (!(mm.rsv[i].flags & RSV_NOMAP)) {
			continue;
		}
		for (uint64_t b = mm.rsv[i].base & ~(BLOCK - 1); b < mm.rsv[i].base + mm.rsv[i].size; b += BLOCK) {
			CHECK(memmap_block(&mm, b) != BLOCK_NORMAL, "no-map %#llx's block %#llx mapped cacheable",
			    (unsigned long long)mm.rsv[i].base, (unsigned long long)b);
		}
	}
	CHECK(memmap_in_ram(&mm, 0xF0000000, 0x04000000), "0xF0000000 + 64 MB is not RAM");

	if (fi.compatible && fdt_compat_has(fi.compatible, fi.compatible_len, "samsung,exynos8890")) {
		CHECK(fi.nmem == 2 && mm.nram == 2, "two RAM banks, got %d", mm.nram);
		CHECK(fi.timer_hz == 26000000, "the timer's 26 MHz, got %u", fi.timer_hz);
		CHECK(fi.psci_smc, "PSCI by SMC");
		CHECK(fi.uart.kind == UART_NONE, "a UART taken (the shim has no use for one)");
		/* the payload's window: the CP's modem_if (0xF0000000 + 136 MB) and nothing else; the loader moves the payload out */
		for (int i = 0; i < fi.nrsv; i++) {
			int in_window = fi.rsv[i].base < 0xF4000000 && 0xF0000000 < fi.rsv[i].base + fi.rsv[i].size;
			CHECK(!in_window || (fi.rsv[i].base == 0xF0000000 && fi.rsv[i].size == 0x08800000),
			    "the tree reserves %#llx + %#llx in the payload's window", (unsigned long long)fi.rsv[i].base,
			    (unsigned long long)fi.rsv[i].size);
		}
		CHECK(!free_meets(&mm, 0x91200000, 0x02000000), "Samsung's log regions and TWRP's ramoops handed out");
		CHECK(!free_meets(&mm, 0xD8000000, 1440 * 4 * 2560), "S-Boot's framebuffer handed out");
		CHECK(free_has(&mm, 0xA1000000, 0x01000000), "loader.efi's base (xnuloader/build.sh /base) not free");
		CHECK(free_has(&mm, 0xE0000000, 0x04000000), "platform_gracerlte.payload_home's 64 MB not free");
		CHECK(biggest == 0x80000000ULL && free_has(&mm, 0x880000000ULL, 0x80000000ULL),
		    "the whole high bank (2 GB) free for XNU, the largest is %#llx", biggest);
	}
	printf("%s\n", failures ? "FAILED" : "all checks passed");
	return failures != 0;
}
