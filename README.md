# exfatrim

`exfatrim` sends TRIM for the free space of an exFAT volume on macOS. It is for
anyone who keeps an SSD formatted exFAT, the format macOS, Windows and iPadOS
all read and write, and wants the drive to learn which blocks deleted files
left behind. It unmounts the volume, checks it, reads the exFAT allocation
bitmap, unmaps the free clusters only and mounts the volume again. With `-v`
it also samples the free space to check that the drive dropped the data.

## The macOS situation

macOS never sends TRIM to an exFAT volume.

- APFS trims free space at mount, and HFS+ during `fsck_hfs`.
- `trimforce enable` turns on TRIM only for third-party SATA drives on an AHCI
  controller. It does nothing for USB or Thunderbolt drives.
- macOS's exFAT driver (`com.apple.fskit.exfat` on macOS 26 and later) and its
  tools (`fsck_exfat`, `newfs_exfat`, `mount_exfat`) contain no TRIM path.
  Nothing on macOS ever trims an exFAT volume.

## Why that hurts an SSD

The controller keeps deleted data as live data, because nothing tells it
otherwise. Background garbage collection can reclaim only blocks the host has
overwritten. Under heavy write-and-delete churn the controller runs out of
erased blocks, sustained writes slow down and the drive can stall.

## Why not reformat as APFS

Windows cannot read APFS without third-party software. exFAT stays the format
for a drive shared with Windows machines, and `exfatrim` supplies the missing
TRIM.

## Does macOS pass TRIM to my drive?

Run `exfatrim -n`. It refuses with a clear message when the drive's
`DKIOCGETFEATURES` lacks `DK_FEATURE_UNMAP`, which means macOS cannot send
TRIM to the drive over its current connection. A drive that reports
`DK_FEATURE_UNMAP` may still ignore the command; `-v` measures that.

## Build and install

`exfatrim` is one C file with no dependencies. It builds with `clang` from the
Xcode command line tools.

```sh
make
sudo make install        # copies exfatrim to /usr/local/bin; PREFIX=... overrides
```

## Usage

```
sudo exfatrim [-n] [-v] [volume]
```

`volume` is a name (`"My SSD"`), a mount point or a disk id (`disk4s2`,
`/dev/disk4s2`, `/dev/rdisk4s2`). A name wins over a disk id. Omit `volume`
when exactly one exFAT volume is mounted. A volume that is already unmounted
can be named by disk id; it stays unmounted.

| Flag | Effect |
|---|---|
| `-n` | Dry run. Reads the volume and reports what a trim would cover. Changes nothing. |
| `-v` | Verify. Samples free clusters that hold data, trims, then reports how many the drive now reads as zeros. |

```
$ sudo exfatrim -n -v "My SSD"
My SSD (dry run): would trim 412 GB in 1873 range(s). Nothing changed.
  Free to use: 412 GB of 2.0 TB.
  Deleted data: 299 of 411 sampled free clusters (73%) still hold data.

$ sudo exfatrim -v "My SSD"
✓ My SSD trimmed: 412 GB of free space handed back to the SSD (38s).
  Free to use: 412 GB of 2.0 TB. Ready to use. Next trim: ~Dec 8.
  Verified: 299 of 299 dirty free clusters read as zeros; the drive cleared at least 99.0% of dirty free space (95% confidence).
```

An error prints one `✗` line. Indented lines follow only when they give a
command to run or a choice to make.

### Monthly routine

Run `sudo exfatrim -v "My SSD"` once a month. `sudo exfatrim -n -v "My SSD"`
tells you whether a trim is due: the share of sampled free space that still
holds deleted data.

### Exit codes

| Code | Meaning |
|---|---|
| 0 | Success. With `-v`, every dirty sample read as zeros, or no dirty sample existed. |
| 1 | Refused or error. |
| 2 | Usage error. |
| 3 | `-v` only. At least one dirty sample still holds data; the trim is not confirmed. |
| 4 | `exfatrim` unmounted the volume and could not mount it again. Run `diskutil mount <disk id>` or replug the drive. Code 4 takes precedence over 1 and 3. |

## Safety

The invariant: **no byte outside a cluster the bitmap marks free, read on the
unmounted volume, is ever unmapped.** Before it unmaps anything, `exfatrim`:

1. checks that macOS reports `DK_FEATURE_UNMAP` for the device, before it
   unmounts anything;
2. unmounts the volume and requires `fsck_exfat -n` to exit 0;
3. opens the raw device with an exclusive lock and holds it through the last
   unmap and the verification reads, so nothing mounts the volume mid-run;
4. validates the boot sector: signature, zeroed must-be-zero region, checksum,
   a single FAT, revision 1.x, no media-failure flag, a clean volume flag,
   FAT offset of at least 24 sectors, a heap that follows the FAT and a
   geometry that fits the device;
5. requires exactly one allocation bitmap, exactly `(clusters + 7) / 8` bytes
   long, in a FAT chain that stays inside the volume, never repeats a cluster
   and fills the whole bitmap;
6. requires the root directory, the bitmap and the up-case table to be marked
   allocated;
7. compares the bitmap's free space with what macOS reported while the volume
   was mounted, and refuses a difference above 1% of the volume;
8. shrinks every free run inward to the device block size, so an extent never
   reaches into an allocated neighbour, and refuses any extent outside the
   cluster heap.

`Ctrl-C` stops between unmap calls and mounts the volume again.

## How `-v` works

A free cluster can show a trim only if it held data first. `-v` therefore
samples free clusters, keeps the ones with a nonzero byte (the dirty samples),
trims, and reads the dirty samples again.

- **Population.** Free clusters that lie wholly inside an extent `exfatrim`
  sends. A free cluster at the edge of a run may be shrunk out of its extent
  and never unmapped, so it is not eligible.
- **Draws.** Distinct clusters chosen uniformly at random with
  `arc4random_uniform`, stopping at 299 dirty samples or at
  min(3,000, population) draws.
- **Read size.** The first `min(cluster size, 1 MiB)` of each cluster. A
  sampled cluster is unmapped whole, so its first MiB stands for it, and a run
  on 32 MiB clusters reads about 3 GB at most.
- **Timing.** The first read happens on the unmounted volume after every safety
  check has passed and before the first unmap. The second read happens after
  the last unmap and before the volume mounts again, because mounting writes
  and frees clusters of its own.
- **Bound.** If all N dirty samples read as zeros, the share of dirty free
  space the drive cleared is at least `0.05^(1/N)` with 95% confidence. N = 299
  gives 99.0%, N = 59 gives 95.0% and N = 30 gives 90.5%.

If no dirty sample exists, free space already reads as zeros, an earlier trim
worked and the run exits 0.

Exit 3 means at least one dirty sample still holds data. A drive that ignored
TRIM and a drive that does not return zeros for trimmed blocks look the same,
so exit 3 says the trim could not be confirmed and does not say which cause
applies.

With `-n -v`, `exfatrim` draws the same sample on the mounted volume,
read-only, and reports the share that still holds deleted data.

## Testing

`make test` needs macOS 26 or later (`diskutil image`) and no `sudo`. It
attaches disk images it creates, runs `exfatrim` only against them by the
device id it recorded, and detaches them on exit.

- **RAW images** (`diskutil image create blank --format RAW`) read unmapped
  blocks as zeros, like a drive that honours TRIM. A `-v` run on one must exit
  0 with at least one dirty sample, all cleared.
- **SPARSE images** (`hdiutil create -type SPARSE`) accept `DKIOCUNMAP` and
  keep the data, like a drive that ignores TRIM. A `-v` run on one must exit 3.
- Per cluster size (4 KB, 32 KB and 128 KB): live files stay byte-identical,
  `fsck_exfat -n` is clean and every free cluster is zero after a trim of the
  unmounted volume.
- A second `-v` run right after a trim of the unmounted volume reports no
  dirty sample. A dry run changes no byte of the device.
- Volume name, mount point and disk id (with and without `/dev/` and
  `/dev/r`) all resolve.
- Corrupted volumes are refused with the volume bytes unchanged: a bitmap bit
  cleared under a live file, a corrupt bitmap length, a self-loop in the
  bitmap's FAT chain, a root directory marked free, an unsupported revision,
  a FAT offset below 24 sectors, a media-failure flag, and a device another
  process holds.

Two environment variables exist for the tests only:

- `EXFATRIM_TEST_BLOCK=<bytes>` raises the alignment unit to a power of two at
  or above the device block size. Disk images report 512-byte blocks, so this
  exercises the inward rounding that a drive with 4096-byte blocks needs.
- `EXFATRIM_TEST_SKIP_FSCK=1` skips the `fsck_exfat` gate, so a test can show
  that `exfatrim`'s own checks refuse a corrupted volume.

CI builds the tool and runs the tests on every push.

## License and warranty

`exfatrim` is MIT licensed (see `LICENSE`) and comes with no warranty. A TRIM
cannot be undone. Keep a backup of any drive you run it on.
