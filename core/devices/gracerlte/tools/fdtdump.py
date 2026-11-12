import struct, sys
d = open(sys.argv[1], 'rb').read()
magic, total, off_struct, off_str = struct.unpack('>4I', d[:16])
p, path = off_struct, []
def s(o):
    return d[off_str + o:d.index(b'\0', off_str + o)].decode()
while True:
    t, = struct.unpack('>I', d[p:p + 4]); p += 4
    if t == 1:
        e = d.index(b'\0', p); path.append(d[p:e].decode()); p = (e + 4) & ~3
        print('/'.join(path) or '/')
    elif t == 2: path.pop()
    elif t == 3:
        ln, no = struct.unpack('>2I', d[p:p + 8]); v = d[p + 8:p + 8 + ln]; p = (p + 8 + ln + 3) & ~3
        if ln and v[-1] == 0 and all(32 <= c < 127 or c == 0 for c in v) and v[0] != 0: r = repr(v[:-1].decode().split('\0'))
        elif ln % 4 == 0: r = ' '.join('%x' % x for x in struct.unpack('>%dI' % (ln // 4), v))
        else: r = v.hex()
        print('   ', s(no), '=', r)
    elif t == 9: break
