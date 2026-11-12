import struct, sys
blob = open(sys.argv[1], 'rb').read()
base = int(sys.argv[2], 0)
want = sys.argv[3] if len(sys.argv) > 3 else 'dvfs_g3d'
def u32(o): return struct.unpack_from('<I', blob, base + o)[0]
def s32(o): return struct.unpack_from('<i', blob, base + o)[0]
def string(o):
    n = u32(o) + 1
    s = blob[base + o + 4: base + o + 4 + n].split(b'\0')[0].decode()
    return s, o + 4 + ((n + 3) & ~3)
def table(o):
    ver, total, n = u32(4), u32(8), u32(12)
    o = 16; out = {}
    for _ in range(n):
        name, o = string(o); out[name] = u32(o); o += 4
    return out
hdr = table(0)
print('ECT', blob[base+4:base+8], hdr)
def domains(off):
    pv, ver, n = u32(off), u32(off + 4), u32(off + 8)
    o = off + 12; d = {}
    for _ in range(n):
        name, o = string(o); d[name] = off + u32(o); o += 4
    return pv, d
pv, d = domains(hdr['DVFS'])
print('DVFS parser', pv, list(d))
o = d[want]
mx, mn = u32(o), u32(o + 4); o += 8
if pv >= 2: o += 8
nclk, nlv = u32(o), u32(o + 4); o += 8
clks = []
for _ in range(nclk):
    c, o = string(o); clks.append(c)
levels = [(u32(o + 8 * i), s32(o + 8 * i + 4)) for i in range(nlv)]
o += 8 * nlv
print(want, 'max', mx, 'min', mn, 'clocks', clks)
for i, (lv, en) in enumerate(levels):
    vals = [u32(o + 4 * (i * nclk + c)) for c in range(nclk)]
    print('  level', lv, 'kHz', 'en', en, vals)
pv, d = domains(hdr['ASV'])
print('ASV parser', pv, list(d))
o = d[want]
ngroup, nlevel, ntable = u32(o), u32(o + 4), u32(o + 8); o += 12
lvl = [s32(o + 4 * i) for i in range(nlevel)]; o += 4 * nlevel
print(want, 'groups', ngroup, 'levels', lvl, 'tables', ntable)
for t in range(ntable):
    tv = u32(o); o += 4
    if pv >= 2:
        o += 8; en = [s32(o + 4 * i) for i in range(nlevel)]; o += 4 * nlevel
    n = ngroup * nlevel
    if pv >= 3:
        v = [blob[base + o + i] * 6250 for i in range(n)]; o += n
        o = (o + 3) & ~3
    else:
        v = [u32(o + 4 * i) for i in range(n)]; o += 4 * n
    print(' table', t, 'version', tv)
    for li, l in enumerate(lvl):
        print('   %7d kHz:' % l, ' '.join('%4d' % (v[li * ngroup + g] // 1000) for g in range(ngroup)), 'mV by group')
