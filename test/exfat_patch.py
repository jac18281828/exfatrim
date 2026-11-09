"""Corrupt one structure of an exFAT volume on a test disk image, and undo it.

usage: exfat_patch.py DEVICE UNDOFILE COMMAND [ARG]
       exfat_patch.py DEVICE UNDOFILE restore

Commands: bitmap-len VALUE, clear-bit live|root, fat-loop-bitmap,
boot-revision MAJOR, boot-fat-offset SECTORS, boot-media-failure.
Every command saves the bytes it overwrites to UNDOFILE; restore writes them back.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from exfat_image import BLK, Volume as ImageVolume  # noqa: E402


class Volume(ImageVolume):
    def __init__(self, path):
        super().__init__(path, writable=True)

    def write(self, off, data):
        s = off // BLK * BLK
        e = -(-(off + len(data)) // BLK) * BLK
        self.f.seek(s)
        block = bytearray(self.f.read(e - s))
        block[off - s:off - s + len(data)] = data
        self.f.seek(s)
        assert self.f.write(bytes(block)) == len(block)

    def bitmap_byte_off(self, k):
        _, c, _ = self.bitmap_entry()
        for _ in range(k // self.clus):
            c = self.fat_next(c)
        return self.cluster_off(c) + k % self.clus


def boot_checksum(vol):
    region = vol.read(0, 11 * vol.bps)
    s = 0
    for i, b in enumerate(region):
        if i in (106, 107, 112):
            continue
        s = ((0x80000000 if s & 1 else 0) + (s >> 1) + b) & 0xFFFFFFFF
    return s


class Patcher:
    def __init__(self, vol, undo):
        self.vol = vol
        self.undo = open(undo, "w")

    def patch(self, off, data):
        self.undo.write("%d %s\n" % (off, self.vol.read(off, len(data)).hex()))
        self.undo.flush()
        self.vol.write(off, data)

    def fix_boot_checksum(self):
        v = self.vol
        self.patch(11 * v.bps, struct.pack("<I", boot_checksum(v)) * (v.bps // 4))


def restore(vol, undo):
    lines = open(undo).read().splitlines()
    for line in reversed(lines):
        off, hexdata = line.split()
        vol.write(int(off), bytes.fromhex(hexdata))


def main():
    dev, undo, cmd = sys.argv[1:4]
    arg = sys.argv[4] if len(sys.argv) > 4 else None
    vol = Volume(dev)
    if cmd == "restore":
        restore(vol, undo)
        return
    p = Patcher(vol, undo)
    if cmd == "bitmap-len":
        entry, _, _ = vol.bitmap_entry()
        p.patch(entry + 24, struct.pack("<Q", int(arg)))
    elif cmd == "clear-bit":
        if arg == "root":
            idx = vol.root - 2
        else:
            bm = vol.bitmap()
            idx = max(i for i in range(vol.nclus) if bm[i >> 3] >> (i & 7) & 1)
        k = idx >> 3
        byte = vol.read(vol.bitmap_byte_off(k), 1)[0]
        p.patch(vol.bitmap_byte_off(k), bytes([byte & ~(1 << (idx & 7)) & 0xFF]))
    elif cmd == "fat-loop-bitmap":
        _, first, _ = vol.bitmap_entry()
        p.patch(vol.fat_off + 4 * first, struct.pack("<I", first))
    elif cmd == "boot-revision":
        p.patch(105, bytes([int(arg)]))
        p.fix_boot_checksum()
    elif cmd == "boot-fat-offset":
        p.patch(80, struct.pack("<I", int(arg)))
        p.fix_boot_checksum()
    elif cmd == "boot-media-failure":
        flags = struct.unpack_from("<H", vol.bs, 106)[0]
        p.patch(106, struct.pack("<H", flags | 4))
    else:
        raise SystemExit("unknown command " + cmd)


main()
