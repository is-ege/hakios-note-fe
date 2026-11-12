# ext4get.py SPARSE_IMAGE DIR [OUTDIR]: list a directory of an Android sparse ext4 image, or copy its files out
import struct, sys, os
f = open(sys.argv[1], 'rb')
magic, maj, minr, fh, ch, blk, total, nchunks, crc = struct.unpack('<IHHHHIIII', f.read(28))
assert magic == 0xed26ff3a
f.seek(fh)
chunks, pos = [], 0                       # (first block, blocks, type, file offset)
for _ in range(nchunks):
    t, _r, n, sz = struct.unpack_from('<HHII', f.read(ch))      # (Samsung's images: 16-byte chunk headers)
    chunks.append((pos, n, t, f.tell()))
    f.seek(sz - ch, 1)
    if t != 0xCAC4: pos += n
def read(off, size):
    out = b''
    while size:
        b = off // blk
        c = next(c for c in chunks if c[0] <= b < c[0] + c[1] and c[2] != 0xCAC4)
        inner = off - c[0] * blk
        n = min(size, c[1] * blk - inner)
        if c[2] == 0xCAC1: f.seek(c[3] + inner); out += f.read(n)
        elif c[2] == 0xCAC2:
            f.seek(c[3]); fill = f.read(4); out += (fill * (n // 4 + 2))[inner % 4:inner % 4 + n]
        else: out += bytes(n)
        off += n; size -= n
    return out
sb = read(1024, 1024)
assert struct.unpack_from('<H', sb, 56)[0] == 0xEF53
bs = 1024 << struct.unpack_from('<I', sb, 24)[0]
ipg, = struct.unpack_from('<I', sb, 40); isz, = struct.unpack_from('<H', sb, 88)
feat_incompat, = struct.unpack_from('<I', sb, 96)
dsz = struct.unpack_from('<H', sb, 254)[0] if feat_incompat & 0x80 else 32
def inode(n):
    g, i = divmod(n - 1, ipg)
    gd = read((1 if bs > 1024 else 2) * bs + g * dsz, dsz)
    tbl = struct.unpack_from('<I', gd, 8)[0] | (struct.unpack_from('<I', gd, 40)[0] << 32 if dsz > 32 else 0)
    return read(tbl * bs + i * isz, isz)
def extents(node, out):
    m, n, _mx, depth = struct.unpack_from('<HHHH', node, 0)
    assert m == 0xF30A
    for k in range(n):
        e = node[12 + 12 * k:24 + 12 * k]
        if depth == 0:
            lb, ln, hi, lo = struct.unpack('<IHHI', e); out.append((lb, ln & 0x7fff, lo | hi << 32))
        else:
            lb, lo, hi, _ = struct.unpack('<IIHH', e); extents(read((lo | hi << 32) * bs, bs), out)
def data(ino):
    size = struct.unpack_from('<I', ino, 4)[0] | struct.unpack_from('<I', ino, 108)[0] << 32
    flags, = struct.unpack_from('<I', ino, 32)
    if not flags & 0x80000:
        return ino[40:40 + size] if size <= 60 else None
    ex = []; extents(ino[40:100], ex)
    buf = bytearray(size)
    for lb, ln, pb in ex:
        chunk = read(pb * bs, ln * bs)
        buf[lb * bs:lb * bs + len(chunk)] = chunk[:max(0, size - lb * bs)]
    return bytes(buf)
def listdir(ino):
    d, o, out = data(ino), 0, {}
    while o < len(d):
        i, rl, nl, ft = struct.unpack_from('<IHBB', d, o)
        if i and nl: out[d[o + 8:o + 8 + nl].decode('utf8', 'replace')] = (i, ft)
        if rl == 0: break
        o += rl
    return out
cur = inode(2)
for part in [p for p in sys.argv[2].split('/') if p]:
    ent = listdir(cur)[part]
    cur = inode(ent[0])
    if ent[1] == 7:                        # a symlink: said, not followed
        print('symlink ->', data(cur)); sys.exit(0)
for name, (i, ft) in sorted(listdir(cur).items()):
    if name in ('.', '..'): continue
    ino = inode(i)
    size = struct.unpack_from('<I', ino, 4)[0]
    print('%10d %s%s' % (size, name, '/' if ft == 2 else ' -> ' + data(ino).decode() if ft == 7 else ''))
    if len(sys.argv) > 3 and ft == 1:
        open(os.path.join(sys.argv[3], os.path.basename(name)), 'wb').write(data(ino))
