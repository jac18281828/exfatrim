"""Independent check for test.sh: parse an exFAT volume and report free clusters
that still hold data. Exit 0 if every free cluster reads as zeros."""
import struct
import sys

BLK = 4096


def read(f, off, n):
    s = off // BLK * BLK
    e = -(-(off + n) // BLK) * BLK
    f.seek(s)
    return f.read(e - s)[off - s:off - s + n]


with open(sys.argv[1], "rb", buffering=0) as f:
    bs = read(f, 0, 512)
    assert bs[3:11] == b"EXFAT   "
    bps = 1 << bs[108]
    clus = bps << bs[109]
    fat_off = struct.unpack_from("<I", bs, 80)[0] * bps
    heap = struct.unpack_from("<I", bs, 88)[0] * bps
    n = struct.unpack_from("<I", bs, 92)[0]
    root = struct.unpack_from("<I", bs, 96)[0]

    def off(c):
        return heap + (c - 2) * clus

    def nxt(c):
        return struct.unpack("<I", read(f, fat_off + 4 * c, 4))[0]

    bm_first = bm_len = None
    c = root
    while c != 0xFFFFFFFF and bm_first is None:
        d = read(f, off(c), clus)
        for i in range(0, clus, 32):
            if d[i] == 0x81:
                bm_first, bm_len = struct.unpack_from("<IQ", d, i + 20)
                break
        c = nxt(c)

    bm, c = b"", bm_first
    while len(bm) < bm_len:
        bm += read(f, off(c), clus)
        c = nxt(c)

    dirty = 0
    for i in range(n):
        if not bm[i >> 3] >> (i & 7) & 1:
            if any(read(f, off(i + 2), clus)):
                dirty += 1
    free = sum(1 for i in range(n) if not bm[i >> 3] >> (i & 7) & 1)
    print(f"  verify: {free} free clusters, {dirty} still hold data")
    sys.exit(1 if dirty else 0)
