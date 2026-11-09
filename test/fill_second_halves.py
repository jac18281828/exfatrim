"""Write leftovers into the second half of every free cluster of a test image.

usage: fill_second_halves.py DEVICE
Prints the number of free clusters written. Allocated clusters stay untouched.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from exfat_image import BLK, Volume  # noqa: E402

vol = Volume(sys.argv[1], writable=True)
bm = vol.bitmap()
half = vol.clus // 2
assert half % BLK == 0, "cluster halves must be block aligned"
free = 0
for i in range(vol.nclus):
    if not bm[i >> 3] >> (i & 7) & 1:
        vol.f.seek(vol.cluster_off(i + 2) + half)
        assert vol.f.write(b"\xa5" * half) == half
        free += 1
print(free)
