"""Write leftovers into the first free cluster of a test image.

usage: fill_first_free_cluster.py DEVICE
Prints the index of the cluster written. Every other cluster stays untouched.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from exfat_image import Volume  # noqa: E402

vol = Volume(sys.argv[1], writable=True)
bm = vol.bitmap()
for i in range(vol.nclus):
    if not bm[i >> 3] >> (i & 7) & 1:
        vol.f.seek(vol.cluster_off(i + 2))
        assert vol.f.write(b"\xa5" * vol.clus) == vol.clus
        print(i)
        break
else:
    sys.exit("no free cluster")
