#!/usr/bin/env python3
"""Minimal reader of the iOS 16 split dyld shared cache: address -> image, nearest export / ObjC method; disassembly."""
import mmap, os, struct, subprocess, sys, bisect

DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../../sysimage/20F66__iPhone10,3_6")
MASK = 0x000000FFFFFFFFFF
SEL_BASE = 0x18229614B     # libobjc __objc_opt_ro + relativeMethodSelectorBaseAddressOffset (20F66)


class Cache:
    def __init__(self):
        self.maps = []          # (addr, size, data, fileoff)
        main = self._open("dyld_shared_cache_arm64")
        self.main = main
        off, cnt = struct.unpack_from("<II", main, 0x188)
        for i in range(cnt):
            suffix = main[off + i * 56 + 24: off + i * 56 + 56].split(b"\0")[0].decode()
            self._open("dyld_shared_cache_arm64" + suffix)
        self.maps.sort()
        self.starts = [m[0] for m in self.maps]
        ioff, icnt = struct.unpack_from("<II", main, 0x1C0)
        self.images = []
        for i in range(icnt):
            addr, _, _, poff = struct.unpack_from("<QQQI", main, ioff + i * 32)
            self.images.append((addr, main[poff:main.find(b"\0", poff)].decode()))
        self.images.sort()
        self.iaddr = [a for a, _ in self.images]
        self._syms = {}

    def _open(self, name):
        fd = os.open(os.path.join(DIR, name), os.O_RDONLY)
        data = mmap.mmap(fd, 0, prot=mmap.PROT_READ)
        moff, mcnt = struct.unpack_from("<II", data, 0x10)
        for i in range(mcnt):
            addr, size, fileoff = struct.unpack_from("<QQQ", data, moff + i * 32)
            self.maps.append((addr, size, data, fileoff))
        return data

    def read(self, addr, n):
        i = bisect.bisect_right(self.starts, addr) - 1
        a, size, data, fo = self.maps[i]
        if not (a <= addr < a + size):
            raise KeyError(hex(addr))
        return data[fo + addr - a: fo + addr - a + n]

    def u32(self, a): return struct.unpack("<I", self.read(a, 4))[0]
    def i32(self, a): return struct.unpack("<i", self.read(a, 4))[0]
    def ptr(self, a): return struct.unpack("<Q", self.read(a, 8))[0] & MASK

    def cstr(self, a):
        out = b""
        while True:
            c = self.read(a, 64)
            z = c.find(b"\0")
            if z >= 0:
                return (out + c[:z]).decode(errors="replace")
            out += c
            a += 64

    def image_of(self, addr):
        i = bisect.bisect_right(self.iaddr, addr) - 1
        return self.images[i]

    def image(self, path_end):
        for a, p in self.images:
            if p.endswith(path_end):
                return a, p
        raise KeyError(path_end)

    def segments(self, base):
        ncmds = self.u32(base + 16)
        p, segs, cmds = base + 32, {}, []
        for _ in range(ncmds):
            cmd, size = struct.unpack("<II", self.read(p, 8))
            cmds.append((cmd, p))
            if cmd == 0x19:
                name = self.read(p + 8, 16).split(b"\0")[0].decode()
                vmaddr, vmsize, fileoff = struct.unpack("<QQQ", self.read(p + 24, 24))
                nsect = self.u32(p + 64)
                sects = {}
                for s in range(nsect):
                    sp = p + 72 + s * 80
                    sn = self.read(sp, 16).split(b"\0")[0].decode()
                    sa, ss = struct.unpack("<QQ", self.read(sp + 32, 16))
                    sects[sn] = (sa, ss)
                segs[name] = (vmaddr, vmsize, fileoff, sects)
            p += size
        return segs, cmds

    def symbols(self, base):
        """{addr: name} from the export trie and ObjC method lists of one image."""
        if base in self._syms:
            return self._syms[base]
        syms = {}
        segs, cmds = self.segments(base)
        le = segs["__LINKEDIT"]
        def le_addr(off): return le[0] + off - le[2]
        for cmd, p in cmds:
            if cmd == 0x80000033:   # LC_DYLD_EXPORTS_TRIE
                off, size = struct.unpack("<II", self.read(p + 8, 8))
                self._trie(self.read(le_addr(off), size), base, syms)
        for segname, (va, vs, fo, sects) in segs.items():
            for sn, (sa, ss) in sects.items():
                if sn == "__objc_classlist":
                    for k in range(ss // 8):
                        cls = self.ptr(sa + k * 8)
                        self._class(cls, syms, "-")
                        try:
                            self._class(self.ptr(cls), syms, "+")
                        except Exception:
                            pass
                if sn == "__objc_catlist":
                    for k in range(ss // 8):
                        cat = self.ptr(sa + k * 8)
                        try:
                            name = self.cstr(self.ptr(cat))
                            cname = self._class_name(self.ptr(cat + 8)) or "?"
                            self._methods(self.ptr(cat + 16), syms, "-", f"{cname}({name})")
                            self._methods(self.ptr(cat + 24), syms, "+", f"{cname}({name})")
                        except Exception:
                            pass
        self._syms[base] = syms
        return syms

    def _class_name(self, cls):
        try:
            ro = self.ptr(cls + 32) & ~7
            return self.cstr(self.ptr(ro + 24))
        except Exception:
            return None

    def _class(self, cls, syms, kind):
        ro = self.ptr(cls + 32) & ~7
        name = self.cstr(self.ptr(ro + 24))
        self._methods(self.ptr(ro + 32), syms, kind, name)

    def _methods(self, ml, syms, kind, cname):
        if not ml:
            return
        flags, count = struct.unpack("<II", self.read(ml, 8))
        es = flags & 0xFFFC
        for j in range(count):
            e = ml + 8 + j * es
            if flags & 0x80000000:
                noff, _, ioff = struct.unpack("<iii", self.read(e, 12))
                try:
                    sel = self.cstr(SEL_BASE + noff) if flags & 0x40000000 else self.cstr(self.ptr(e + noff))
                except Exception:
                    sel = "?"
                imp = e + 8 + ioff
            else:
                sel = self.cstr(self.ptr(e))
                imp = self.ptr(e + 16)
            syms[imp] = f"{kind}[{cname} {sel}]"

    def _trie(self, t, base, syms):
        def uleb(i):
            r = s = 0
            while True:
                b = t[i]; i += 1
                r |= (b & 0x7F) << s; s += 7
                if b < 0x80:
                    return r, i
        stack = [(0, "")]
        while stack:
            node, pre = stack.pop()
            size, i = uleb(node)
            if size:
                flags, j = uleb(i)
                if not flags & 8:
                    off, _ = uleb(j)
                    syms[base + off] = pre
            i += size
            n = t[i]; i += 1
            for _ in range(n):
                z = t.index(b"\0", i)
                edge = t[i:z].decode(errors="replace"); i = z + 1
                child, i = uleb(i)
                stack.append((child, pre + edge))

    def name(self, addr):
        base, path = self.image_of(addr)
        syms = self.symbols(base)
        keys = sorted(k for k in syms if k <= addr)
        sym = f"{syms[keys[-1]]}+{addr - keys[-1]:#x}" if keys else "?"
        return f"{os.path.basename(path)}+{addr - base:#x} {sym}"

    def find(self, base, needle):
        return {a: n for a, n in self.symbols(base).items() if needle in n}

    def dis(self, addr, n):
        code = self.read(addr, n * 4)
        hexs = " ".join(f"0x{b:02x}" for b in code)
        out = subprocess.run(["/opt/homebrew/opt/llvm/bin/llvm-mc", "--disassemble", "-triple=arm64-apple-ios"],
                             input=hexs, capture_output=True, text=True).stdout
        lines = [l.split("//")[0].strip() for l in out.splitlines() if l.strip() and not l.strip().startswith(".")]
        return [(addr + 4 * k, l) for k, l in enumerate(lines)]


if __name__ == "__main__":
    c = Cache()
    slide = int(sys.argv[1], 16)
    for a in sys.argv[2:]:
        print(a, c.name(int(a, 16) - slide))
