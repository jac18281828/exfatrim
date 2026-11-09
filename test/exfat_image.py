"""Aligned reads and boot-sector parsing shared by the exFAT test helpers.

Import it from a script run with python3 -I by putting the script's own
directory on sys.path first.
"""
import struct

BLK = 4096


class Volume:
    def __init__(self, path, writable=False):
        self.f = open(path, "r+b" if writable else "rb", buffering=0)
        bs = self.read(0, 512)
        assert bs[3:11] == b"EXFAT   ", "not an exFAT volume"
        self.bs = bs
        self.bps = 1 << bs[108]
        self.clus = self.bps << bs[109]
        self.fat_off = struct.unpack_from("<I", bs, 80)[0] * self.bps
        self.heap = struct.unpack_from("<I", bs, 88)[0] * self.bps
        self.nclus = struct.unpack_from("<I", bs, 92)[0]
        self.root = struct.unpack_from("<I", bs, 96)[0]

    def read(self, off, n):
        s = off // BLK * BLK
        e = -(-(off + n) // BLK) * BLK
        self.f.seek(s)
        return self.f.read(e - s)[off - s:off - s + n]

    def cluster_off(self, c):
        return self.heap + (c - 2) * self.clus

    def fat_next(self, c):
        return struct.unpack("<I", self.read(self.fat_off + 4 * c, 4))[0]

    def bitmap_entry(self):
        c = self.root
        while c != 0xFFFFFFFF:
            d = self.read(self.cluster_off(c), self.clus)
            for i in range(0, self.clus, 32):
                if d[i] == 0x81:
                    first, length = struct.unpack_from("<IQ", d, i + 20)
                    return self.cluster_off(c) + i, first, length
            c = self.fat_next(c)
        raise SystemExit("no allocation bitmap entry")

    def bitmap(self):
        _, first, length = self.bitmap_entry()
        out, c = b"", first
        while len(out) < length:
            out += self.read(self.cluster_off(c), self.clus)
            c = self.fat_next(c)
        return out[:length]
