#!/bin/bash
# End-to-end test of exfatrim on a throwaway exFAT disk image (no sudo needed).
# For each cluster size: write fragmented files, delete half, fill the free space
# with junk and delete it, run exfatrim, then check that every surviving file is
# byte-identical, fsck_exfat is clean, and every free cluster now reads as zeros.
set -euo pipefail
cd "$(dirname "$0")"
clang -O2 -Wall -Wextra exfatrim.c -o exfatrim

WORK=$(mktemp -d)
trap 'hdiutil detach -quiet -force "$DISK" 2>/dev/null || true; rm -rf "$WORK"' EXIT
DISK=

for CLUSTER in 4096 32768 131072; do
    VOL=TRIMTEST
    diskutil image create blank --format RAW --size 384m "$WORK/t.dmg" >/dev/null
    DISK=$(diskutil image attach --noMount "$WORK/t.dmg" | awk '/\/dev\/disk/{print $1; exit}')
    PART=$DISK
    newfs_exfat -b "$CLUSTER" -v "$VOL" "${PART/disk/rdisk}" >/dev/null
    diskutil mount "$PART" >/dev/null
    M=/Volumes/$VOL

    # Fragmented live data: 120 files of random size, delete every other one.
    for i in $(seq 1 120); do
        head -c $(( (RANDOM * 97) % 3000000 + 1 )) /dev/urandom > "$M/f$i"
    done
    for i in $(seq 1 2 120); do rm "$M/f$i"; done
    mkdir -p "$M/dir/sub"; head -c 777777 /dev/urandom > "$M/dir/sub/nested"
    # Leave old junk in the free space so a trim is observable as zeros.
    head -c $(( 200 * 1024 * 1024 )) /dev/urandom > "$M/junk" ; sync; rm "$M/junk"; sync
    (cd "$M" && find . -type f ! -name '._*' ! -path './.fseventsd/*' -print0 | xargs -0 shasum) > "$WORK/sums"

    # Normal path: mounted volume by name; the tool unmounts, trims, remounts.
    ./exfatrim -n "$VOL"
    ./exfatrim "$VOL"
    (cd "$M" && shasum -c --quiet "$WORK/sums") || { echo "FAIL: file contents changed (cluster $CLUSTER)"; exit 1; }

    # Mounting frees a few clusters of its own (.fseventsd etc.), so prove full
    # coverage on an unmounted disk: trim it by id, then check nothing is left.
    diskutil unmount "$PART" >/dev/null
    ./exfatrim "$PART"
    fsck_exfat -n "${PART/disk/rdisk}" >/dev/null || { echo "FAIL: fsck after trim (cluster $CLUSTER)"; exit 1; }
    python3 -I verify_zero.py "${PART/disk/rdisk}" || { echo "FAIL: free space not zeroed (cluster $CLUSTER)"; exit 1; }
    diskutil mount "$PART" >/dev/null
    (cd "$M" && shasum -c --quiet "$WORK/sums") || { echo "FAIL: file contents changed (cluster $CLUSTER)"; exit 1; }
    hdiutil detach -quiet "$DISK"; DISK=
    rm -f "$WORK/t.dmg"
    echo "PASS cluster=$CLUSTER"
done
