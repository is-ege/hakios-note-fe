#!/usr/bin/env python3
"""Repack a Samsung Exynos boot.img (Android v0 header with Samsung's dt_size field, DTBH dt.img, SEANDROIDENFORCE
trailer) with a different kernel, keeping the template's ramdisk, dt.img and header fields.

  samsung_bootimg.py --template boot.img --kernel probe.bin --output probe-boot.img
  samsung_bootimg.py --template boot.img --selftest     # repacking the template's own kernel must reproduce it
"""
import argparse
import hashlib
import struct
import sys

HEADER = struct.Struct("<8s9I")  # magic, kernel_size, kernel_addr, ramdisk_size, ramdisk_addr, second_size,
                                 # second_addr, tags_addr, page_size, dt_size
TRAILER = b"SEANDROIDENFORCE"


def parse(image):
    magic, ks, _, rs, _, ss, _, _, page, ds = HEADER.unpack_from(image)
    if magic != b"ANDROID!":
        sys.exit("template: not an Android boot image")
    pad = lambda n: (n + page - 1) // page * page
    ko = page
    ro = ko + pad(ks)
    so = ro + pad(rs)
    do = so + pad(ss)
    end = do + pad(ds)
    if image[do:do + 4] != b"DTBH":
        sys.exit("template: no DTBH dt.img where the header says")
    return {
        "page": page,
        "kernel": image[ko:ko + ks],
        "ramdisk": image[ro:ro + rs],
        "second": image[so:so + ss],
        "dt": image[do:do + ds],
        "trailer": image[end:end + len(TRAILER)] == TRAILER,
        "end": end,
    }


def build(template, parts, kernel):
    page = parts["page"]
    pad = lambda b: b + b"\0" * (-len(b) % page)
    header = bytearray(template[:page])
    struct.pack_into("<I", header, 8, len(kernel))
    sha = hashlib.sha1()
    for blob in (kernel, parts["ramdisk"], parts["second"], parts["dt"]):
        sha.update(blob)
        sha.update(struct.pack("<I", len(blob)))
    header[576:608] = sha.digest().ljust(32, b"\0")
    out = bytes(header) + pad(kernel) + pad(parts["ramdisk"]) + pad(parts["second"]) + pad(parts["dt"])
    if parts["trailer"]:
        out += TRAILER
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--template", required=True)
    ap.add_argument("--kernel")
    ap.add_argument("--output")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()

    template = open(args.template, "rb").read()
    parts = parse(template)

    if args.selftest:
        out = build(template, parts, parts["kernel"])
        expected = template[:parts["end"] + (len(TRAILER) if parts["trailer"] else 0)]
        if out != expected:
            sys.exit("selftest: repacked image differs from the template")
        print(f"selftest: {len(out)} bytes reproduced exactly")
        return

    kernel = open(args.kernel, "rb").read()
    if kernel[0x38:0x3c] != b"ARM\x64":
        sys.exit("kernel: no arm64 Image magic")
    out = build(template, parts, kernel)
    open(args.output, "wb").write(out)
    print(f"{args.output}: {len(out)} bytes, sha256 {hashlib.sha256(out).hexdigest()}")


if __name__ == "__main__":
    main()
