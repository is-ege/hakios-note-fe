# This chip's voltage per level, as S5E8890-asv.c's get_asv_voltage computes it: ASV table + MARGIN + SSA1, floor SSA0.
import struct, sys
blob = open(sys.argv[1], 'rb').read(); base = int(sys.argv[2], 0)
VER = 10
CHIP = {'dvfs_big': (9, 0, 0, 0, 'SSA_BIG'), 'dvfs_little': (8, 0, 0, 0, 'SSA_LITTLE'), 'dvfs_mif': (8, 0, 0, 0, 'SSA_MIF'),
        'dvfs_int': (11, 0, 0, 0, 'SSA_INT'), 'dvfs_g3d': (10, 0, 0, 0, 'SSA_G3D')}   # group, ssa10, ssa11, ssa0
def u32(o): return struct.unpack_from('<I', blob, base + o)[0]
def s32(o): return struct.unpack_from('<i', blob, base + o)[0]
def string(o):
    n = u32(o) + 1
    return blob[base+o+4:base+o+4+n].split(b'\0')[0].decode(), o + 4 + ((n + 3) & ~3)
def names(off, skip=4):
    o = off + 4 * skip if skip == 4 else off
    return o
def header(off):
    pv, ver, n = u32(off), u32(off+4), u32(off+8); o = off + 12; d = {}
    for _ in range(n):
        nm, o = string(o); d[nm] = off + u32(o); o += 4
    return pv, d
ver, total, n = u32(4), u32(8), u32(12); o = 16; hdr = {}
for _ in range(n):
    nm, o = string(o); hdr[nm] = u32(o); o += 4
_, gen = header(hdr['GEN']); mpv, margin = header(hdr['MARGIN']); apv, asv = header(hdr['ASV'])
for dom, (grp, ssa10, ssa11, ssa0, ssa) in CHIP.items():
    o = asv[dom]; ngroup, nlevel, ntable = u32(o), u32(o+4), u32(o+8); o += 12
    lvl = [s32(o + 4*i) for i in range(nlevel)]; o += 4 * nlevel
    volts = None
    for t in range(ntable):
        tv = u32(o); o += 4; o += 8; en = [s32(o + 4*i) for i in range(nlevel)]; o += 4 * nlevel
        nv = ngroup * nlevel; v = [blob[base+o+i] * 6250 for i in range(nv)]; o = (o + nv + 3) & ~3
        if tv == VER: volts, enabled = v, en
    m = None
    if dom in margin:
        mo = margin[dom]; mg, ml = u32(mo), u32(mo+4); m = [blob[base+mo+8+i] * 6250 for i in range(mg*ml)]
    g = gen.get(ssa); ssa1 = [0]*8; sub = 256; s0b = s0o = 0
    if g:
        col, row = u32(g), u32(g+4); r = min(VER, row-1); p = [u32(g + 8 + 4*(r*col + i)) for i in range(col)]
        sub, s0b, s0o, ssa1 = p[1], p[2], p[3], p[4:12]
    print('%s: group %d, table %d, margin %s, %s row: subgroup index %d, ssa0 base %d offset %d, ssa1 %s'
          % (dom, grp, VER, 'yes' if m else 'none', ssa, sub, s0b, s0o, ssa1))
    for li, l in enumerate(lvl):
        if not enabled[li]: continue
        v = volts[li*ngroup + grp] + (m[li*ngroup + grp] if m else 0)
        v += ssa1[ssa10] if li < sub else ssa1[ssa11 + 4]
        if s0b and v < s0b + ssa0 * s0o: v = s0b + ssa0 * s0o
        print('  %5d MHz  %7.2f mV  (table %4.0f, margin %3.0f)' % (l if l < 100000 else l // 1000, v / 1000, volts[li*ngroup+grp]/1000,
              (m[li*ngroup+grp] if m else 0)/1000))
