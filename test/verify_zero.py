"""Independent check for test.sh: parse an exFAT volume and report free clusters
that still hold data. Exit 0 if every free cluster reads as zeros."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from exfat_image import Volume  # noqa: E402

vol = Volume(sys.argv[1])
bm = vol.bitmap()
free = dirty = 0
for i in range(vol.nclus):
    if not bm[i >> 3] >> (i & 7) & 1:
        free += 1
        if any(vol.read(vol.cluster_off(i + 2), vol.clus)):
            dirty += 1
print(f"  verify: {free} free clusters, {dirty} still hold data")
sys.exit(1 if dirty else 0)
