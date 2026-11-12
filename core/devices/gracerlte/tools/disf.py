#!/usr/bin/env python3
"""disf.py ADDR N: disassemble N instructions from an unslid cache address; name calls, objc_msgSend stubs, classes, strings."""
import sys, re
from dsc import Cache
c = Cache()
IMM = r"#(-?0x[0-9a-f]+|-?\d+)"


def page_target(ins):
    r = re.match(r"adrp\s+(x\d+), " + IMM, ins[0][1])
    m = re.match(r"(ldr|add)\s+(x\d+), \[?(x\d+), " + IMM, ins[1][1])
    if r and m:
        return m.group(1), (ins[0][0] & ~0xfff) + int(r.group(2), 0) + int(m.group(4), 0)
    return None, None


def describe_ptr(v):
    cls = c._class_name(v)
    if cls and cls.isprintable() and len(cls) < 100:
        return "class " + cls
    try:
        s = c.cstr(v)
        if s and s.isprintable() and len(s) < 160:
            return repr(s)
    except Exception:
        pass
    try:
        # a CFString: isa, flags, ptr, len
        s = c.cstr(c.ptr(v + 16))
        if s and s.isprintable() and len(s) < 160:
            return "@" + repr(s)
    except Exception:
        pass
    try:
        return c.name(v)
    except Exception:
        return hex(v)


def call_target(t):
    ins = c.dis(t, 5)
    kind, p = page_target(ins)
    if ins[0][1].startswith("adrp\tx1,") and kind == "ldr":
        return "[msgSend " + c.cstr(c.ptr(p)) + "]"
    if ins[0][1].startswith("adrp\tx16") and ins[2][1].startswith("br"):
        return "-> " + c.name(p if kind == "add" else c.ptr(p))
    return c.name(t)


regs = {}
for addr, l in c.dis(int(sys.argv[1], 16), int(sys.argv[2])):
    note = ""
    m = re.match(r"(bl|b)\s+" + IMM + "$", l)
    if m:
        try:
            note = call_target(addr + int(m.group(2), 0))
        except Exception as x:
            note = "?" + str(x)
    m = re.match(r"adrp\s+(x\d+), " + IMM, l)
    if m:
        regs[m.group(1)] = (addr & ~0xfff) + int(m.group(2), 0)
    m = re.match(r"(ldr|add)\s+(x\d+), \[?(x\d+), " + IMM, l)
    if m and m.group(3) in regs:
        p = regs.pop(m.group(3)) + int(m.group(4), 0)
        try:
            note = f"[{p:#x}] {describe_ptr(c.ptr(p))}" if m.group(1) == "ldr" else f"{p:#x} {describe_ptr(p)}"
        except Exception:
            note = f"{p:#x}"
    print(f"{addr:#x}  {l:44s} {note}")
