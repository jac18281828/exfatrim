#!/bin/bash
# Safety invariant: exfatrim only ever runs against disk images this script
# attached and recorded, always with an explicit device or volume name.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
BIN=$ROOT/exfatrim
make -s -C "$ROOT" exfatrim

WORK=$(mktemp -d)
ATTACHED=""
IMAGES=0
DISK=""
RAW=""
VOL=""
MP=""
OUT=""
RC=0
CLEARED=0
NDIRTY=0
POLLER=""

cleanup() {
    local d
    if [ -n "$POLLER" ]; then kill "$POLLER" 2>/dev/null || true; fi
    for d in $ATTACHED; do
        hdiutil detach -quiet -force "$d" 2>/dev/null || true
    done
    rm -rf "$WORK"
}
trap cleanup EXIT

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

pass() {
    echo "PASS $*"
}

run() {
    OUT=$("$@" 2>&1) && RC=0 || RC=$?
}

new_volume() {
    local format=$1 size=$2 cluster=$3 image
    IMAGES=$((IMAGES + 1))
    VOL=EXFT$$N$IMAGES
    if [ "$format" = RAW ]; then
        image=$WORK/v$IMAGES.dmg
        diskutil image create blank --format RAW --size "$size" "$image" >/dev/null
    else
        hdiutil create -quiet -type SPARSE -size "$size" -layout NONE "$WORK/v$IMAGES" >/dev/null 2>&1
        image=$WORK/v$IMAGES.sparseimage
    fi
    DISK=$(diskutil image attach --noMount "$image" | awk '/^\/dev\/disk/{print $1; exit}')
    [[ $DISK =~ ^/dev/disk[0-9]+$ ]] || fail "attach returned '$DISK'"
    ATTACHED="$ATTACHED $DISK"
    diskutil info "$DISK" | grep -q 'Protocol: *Disk Image' || fail "$DISK is not a disk image"
    RAW=${DISK/disk/rdisk}
    newfs_exfat -b "$cluster" -v "$VOL" "$RAW" >/dev/null
}

detach_volume() {
    hdiutil detach -quiet "$DISK"
    ATTACHED=$(printf '%s' "$ATTACHED" | tr ' ' '\n' | grep -vx -e '' -e "$DISK" | tr '\n' ' ' || true)
}

mount_volume() {
    diskutil mount "$DISK" >/dev/null
    MP=$(diskutil info "$DISK" | sed -n 's/^ *Mount Point: *//p')
    [ -n "$MP" ] || fail "$DISK did not mount"
}

unmount_volume() {
    diskutil unmount "$DISK" >/dev/null
}

populate() {
    local files=$1 maxbytes=$2 junk_mb=$3 i
    for i in $(seq 1 "$files"); do
        head -c $(( (RANDOM * 97) % maxbytes + 1 )) /dev/urandom > "$MP/f$i"
    done
    for i in $(seq 1 2 "$files"); do rm "$MP/f$i"; done
    mkdir -p "$MP/dir/sub"
    head -c 777777 /dev/urandom > "$MP/dir/sub/nested"
    head -c $(( junk_mb * 1024 * 1024 )) /dev/urandom > "$MP/junk"
    sync
    rm "$MP/junk"
    sync
    (cd "$MP" && find . -type f ! -name '._*' ! -path './.fseventsd/*' -print0 | xargs -0 shasum) > "$WORK/sums"
}

check_sums() {
    (cd "$MP" && shasum -c --quiet "$WORK/sums") || fail "file contents changed ($1)"
}

raw_sum() {
    dd if="$RAW" bs=1m 2>/dev/null | shasum | cut -d' ' -f1
}

verified_counts() {
    local re='Verified: ([0-9]+) of ([0-9]+) sampled leftovers'
    [[ $OUT =~ $re ]] || fail "no verification line: $OUT"
    CLEARED=${BASH_REMATCH[1]}
    NDIRTY=${BASH_REMATCH[2]}
}

expect_trim_confirmed() {
    [ "$RC" -eq 0 ] || fail "$1: exit $RC: $OUT"
    verified_counts
    [ "$NDIRTY" -ge 1 ] || fail "$1: no sampled leftovers: $OUT"
    [ "$CLEARED" -eq "$NDIRTY" ] || fail "$1: $CLEARED of $NDIRTY cleared"
}

expect_clean_fs() {
    fsck_exfat -n "$RAW" >/dev/null || fail "fsck after trim ($1)"
}

trim_whole_volumes() {
    local cluster before re
    for cluster in 4096 32768 131072; do
        new_volume RAW 384m "$cluster"
        mount_volume
        populate 120 3000000 200
        unmount_volume

        before=$(raw_sum)
        run "$BIN" -n -v "$DISK"
        [ "$RC" -eq 0 ] || fail "dry run -v (cluster $cluster): exit $RC: $OUT"
        re='Leftovers: ([0-9]+) of ([0-9]+)'
        [[ $OUT =~ $re ]] || fail "no dry-run sample line: $OUT"
        [ "${BASH_REMATCH[1]}" -ge 1 ] || fail "dry run found no leftovers: $OUT"
        [ "$(raw_sum)" = "$before" ] || fail "dry run changed the device (cluster $cluster)"

        mount_volume
        run "$BIN" -n "$VOL"
        [ "$RC" -eq 0 ] || fail "dry run by name (cluster $cluster): exit $RC: $OUT"
        run "$BIN" -y -v "$VOL"
        expect_trim_confirmed "trim by name (cluster $cluster)"
        check_sums "trim by name (cluster $cluster)"

        unmount_volume
        run "$BIN" -y -v "$DISK"
        [ "$RC" -eq 0 ] || fail "trim by id (cluster $cluster): exit $RC: $OUT"
        expect_clean_fs "cluster $cluster"
        python3 -I "$HERE/verify_zero.py" "$RAW" || fail "free space not zeroed (cluster $cluster)"
        run "$BIN" -y -v "$DISK"
        [ "$RC" -eq 0 ] || fail "second trim (cluster $cluster): exit $RC: $OUT"
        [[ $OUT == *"Verified: all "*"sampled free clusters already read as zeros"* ]] \
            || fail "second trim reported leftovers (cluster $cluster): $OUT"

        mount_volume
        check_sums "after trim by id (cluster $cluster)"
        detach_volume
        pass "cluster=$cluster verified, dry run, files intact, free space zeroed"
    done
}

trim_ignored() {
    new_volume SPARSE 256m 4096
    mount_volume
    populate 40 1000000 100
    run "$BIN" -y -v "$VOL"
    [ "$RC" -eq 3 ] || fail "ignored TRIM: expected exit 3, got $RC: $OUT"
    [[ $OUT == *"Trim not confirmed"*"ignored TRIM"*"does not return zeros"* ]] || fail "exit-3 message: $OUT"
    check_sums "ignored TRIM"
    detach_volume
    pass "sparse image that ignores TRIM exits 3"
}

negative() {
    local label=$1 skip=$2 pattern=$3 undo=$WORK/undo before
    shift 3
    python3 -I "$HERE/exfat_patch.py" "$RAW" "$undo" "$@"
    before=$(raw_sum)
    if [ "$skip" = 1 ]; then
        run env EXFATRIM_TEST_SKIP_FSCK=1 "$BIN" -y "$DISK"
    else
        run "$BIN" -y "$DISK"
    fi
    [ "$RC" -eq 1 ] || fail "$label: expected exit 1, got $RC: $OUT"
    [[ $OUT == *"$pattern"* ]] || fail "$label: refused for another reason: $OUT"
    [ "$(raw_sum)" = "$before" ] || fail "$label: the device changed"
    python3 -I "$HERE/exfat_patch.py" "$RAW" "$undo" restore
    mount_volume
    check_sums "$label"
    unmount_volume
    pass "refuses: $label"
}

expect_lock_held_during_run() {
    local busy
    for _ in 1 2 3 4 5; do
        rm -f "$WORK/done"
        python3 -I "$HERE/poll_busy.py" "$RAW" "$WORK/done" > "$WORK/busy" &
        POLLER=$!
        sleep 0.2
        run env EXFATRIM_TEST_SKIP_FSCK=1 "$BIN" -y -v "$DISK"
        touch "$WORK/done"
        wait "$POLLER"
        POLLER=""
        busy=$(cat "$WORK/busy")
        if [ "$RC" -eq 0 ] && [ "$busy" -ge 1 ]; then return 0; fi
    done
    fail "the device was never locked during a run (exit $RC, $busy refused opens): $OUT"
}

small_volume_cases() {
    local re arg before
    new_volume RAW 64m 512
    mount_volume
    populate 300 6000 1

    for arg in "$VOL" "$MP" "${DISK#/dev/}" "$DISK" "$RAW"; do
        run "$BIN" -n "$arg"
        [ "$RC" -eq 0 ] || fail "resolve '$arg': exit $RC: $OUT"
        [[ $OUT == "$VOL (dry run)"* ]] || fail "resolve '$arg': $OUT"
    done
    run "$BIN" -n "$VOL-missing"
    [ "$RC" -eq 1 ] || fail "unknown volume name: exit $RC: $OUT"
    run "$BIN" -n "disk4s2x"
    [ "$RC" -eq 1 ] || fail "malformed disk id: exit $RC: $OUT"
    pass "name resolution: name, mount point, disk id, /dev and /dev/r forms"

    run env EXFATRIM_TEST_BLOCK=3000 "$BIN" -n "$VOL"
    [ "$RC" -eq 1 ] || fail "bad EXFATRIM_TEST_BLOCK accepted: exit $RC: $OUT"
    run env EXFATRIM_TEST_BLOCK=4294967296 "$BIN" -n "$VOL"
    [ "$RC" -eq 1 ] || fail "EXFATRIM_TEST_BLOCK above UINT32_MAX accepted: exit $RC: $OUT"
    [[ $OUT == *"EXFATRIM_TEST_BLOCK must be a power of two"* ]] || fail "EXFATRIM_TEST_BLOCK above UINT32_MAX: $OUT"
    run env EXFATRIM_TEST_BLOCK=4096 "$BIN" -y -v "$VOL"
    expect_trim_confirmed "4096-byte alignment on 512-byte clusters"
    check_sums "4096-byte alignment"
    unmount_volume
    expect_clean_fs "4096-byte alignment"
    run python3 -I "$HERE/verify_zero.py" "$RAW"
    re='([0-9]+) still hold data'
    [[ $OUT =~ $re ]] || fail "verify_zero output: $OUT"
    { [ "$RC" -eq 1 ] && [ "${BASH_REMATCH[1]}" -ge 1 ]; } || fail "alignment left no unaligned fringe untrimmed: $OUT"
    pass "alignment unit 4096 on 512-byte clusters keeps live files and fsck clean"

    negative "bit cleared under a live file stops at fsck" 0 "file-system errors" clear-bit live
    negative "bitmap DataLength corrupted" 1 "allocation bitmap's length" bitmap-len 17592186044417
    negative "self-loop in the bitmap's FAT chain" 1 "cluster chain loops" fat-loop-bitmap
    negative "root directory marked free" 1 "root directory sits in space marked free" clear-bit root
    negative "exFAT revision 2" 1 "exFAT revision" boot-revision 2
    negative "FAT offset below 24 sectors" 1 "geometry" boot-fat-offset 8
    negative "media failure flag" 1 "media failure" boot-media-failure

    before=$(raw_sum)
    run python3 -I "$HERE/hold_lock.py" "$RAW" env EXFATRIM_TEST_SKIP_FSCK=1 "$BIN" -y "$DISK"
    [ "$RC" -eq 1 ] || fail "locked device: expected exit 1, got $RC: $OUT"
    [[ $OUT == *"can't lock"* ]] || fail "locked device: $OUT"
    [ "$(raw_sum)" = "$before" ] || fail "locked device: the device changed"
    mount_volume
    check_sums "locked device"
    pass "refuses a device another process holds"

    expect_lock_held_during_run
    pass "holds an exclusive lock on the device during a run"

    detach_volume
}

expect_mounted() {
    local mp
    mp=$(diskutil info "$DISK" | sed -n 's/^ *Mount Point: *//p')
    [ "$mp" = "$MP" ] || fail "$1: the volume is no longer mounted at $MP"
}

pty_run() {
    local answer=$1 lecture=$2 user=$3
    shift 3
    run python3 -I "$HERE/pty_run.py" "$answer" env EXFATRIM_TEST_LECTURE_DIR="$lecture" SUDO_USER="$user" "$@"
}

confirmation_cases() {
    local lecture=$WORK/lecture/nested/deeper user=sudoer$$ me
    me=$(id -un)
    new_volume RAW 128m 4096
    mount_volume
    populate 40 500000 40

    pty_run n "$lecture" "$user" "$BIN" -v "$VOL"
    [ "$RC" -eq 1 ] || fail "answer n: expected exit 1, got $RC: $OUT"
    [[ $OUT == *"exfatrim clears the deleted files on $MP."* ]] || fail "notice names the mount point: $OUT"
    [[ $OUT == *"Continue? [y/N]"* ]] || fail "no prompt: $OUT"
    expect_mounted "answer n"
    check_sums "answer n"
    [ ! -e "$lecture" ] || fail "answer n wrote a marker"
    pass "confirmation: n exits 1, the volume stays mounted, no marker"

    pty_run EOF "$lecture" "$user" "$BIN" -v "$VOL"
    [ "$RC" -eq 1 ] || fail "end of input: expected exit 1, got $RC: $OUT"
    expect_mounted "end of input"
    check_sums "end of input"
    [ ! -e "$lecture" ] || fail "end of input wrote a marker"
    pass "confirmation: end of input exits 1, the volume stays mounted"

    pty_run y "$lecture" "$user" "$BIN" -v "$VOL"
    expect_trim_confirmed "answer y"
    { [ -f "$lecture/$user" ] && [ ! -s "$lecture/$user" ]; } || fail "answer y left no empty marker at $lecture/$user"
    expect_mounted "answer y"
    check_sums "answer y"
    pass "confirmation: y trims and writes the marker in a new nested directory"

    pty_run NONE "$lecture" "$user" "$BIN" -v "$VOL"
    [ "$RC" -eq 0 ] || fail "second run asked again or failed (exit $RC): $OUT"
    pass "confirmation: a second run with the marker asks nothing"

    pty_run NONE "$WORK/lecture-dry" "$user" "$BIN" -n "$VOL"
    [ "$RC" -eq 0 ] || fail "-n asked or failed (exit $RC): $OUT"
    [[ $OUT == *"$VOL (dry run)"* ]] || fail "-n output: $OUT"
    pass "confirmation: -n never asks"

    pty_run NONE "$WORK/lecture-yes" "$user" "$BIN" -y "$VOL"
    [ "$RC" -eq 0 ] || fail "-y asked or failed (exit $RC): $OUT"
    [ ! -e "$WORK/lecture-yes" ] || fail "-y wrote a marker"
    pass "confirmation: -y skips the notice and writes no marker"

    run env EXFATRIM_TEST_LECTURE_DIR="$WORK/lecture-notty" SUDO_USER="$user" "$BIN" "$VOL" </dev/null
    [ "$RC" -eq 1 ] || fail "no terminal: expected exit 1, got $RC: $OUT"
    [[ $OUT == *"terminal"*"-y"* ]] || fail "no terminal: $OUT"
    expect_mounted "no terminal"
    check_sums "no terminal"
    [ ! -e "$WORK/lecture-notty" ] || fail "no terminal wrote a marker"
    pass "confirmation: no terminal, no marker and no -y exits 1"

    pty_run y "$WORK/lecture-uid" "" "$BIN" "$VOL"
    [ "$RC" -eq 0 ] || fail "real-uid marker run: exit $RC: $OUT"
    [ -f "$WORK/lecture-uid/$me" ] || fail "no marker named for the real uid ($me)"
    check_sums "real-uid marker run"
    pass "confirmation: without SUDO_USER the marker is named for the real uid"

    detach_volume
}

trim_whole_volumes
trim_ignored
confirmation_cases
small_volume_cases
echo "ALL PASS"
