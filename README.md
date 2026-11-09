# exfatrim

## Safety

`exfatrim` sends TRIM for the free space of an exFAT volume on macOS. It
treats two things differently.

- Files and folders are never touched. `exfatrim` works only on space the file
  system already counts as free.
- Leftovers, the old contents of files you already deleted, are discarded.
  After a trim, undelete and recovery tools cannot bring those files back. A
  trim cannot be undone.

No byte outside a cluster the allocation bitmap marks free, read on the
unmounted volume, is ever unmapped. Before it unmaps anything, `exfatrim`
refuses the volume when any of these checks fails:

1. macOS reports `DK_FEATURE_UNMAP` for the device. This check runs before
   anything unmounts.
2. After the unmount, `fsck_exfat -n` exits 0.
3. The raw device opens with an exclusive lock, held through the last unmap and
   the verification reads, so nothing mounts the volume mid-run.
4. The boot sector has the exFAT signature, a zeroed must-be-zero region and a
   matching checksum.
5. The volume has a single FAT, revision 1.x, no media-failure flag and a
   clean volume flag.
6. The FAT starts at sector 24 or later, the cluster heap follows the FAT and
   the geometry fits the device.
7. The root directory holds exactly one allocation bitmap, exactly
   `(clusters + 7) / 8` bytes long, in a FAT chain that stays inside the
   volume, never repeats a cluster and fills the whole bitmap.
8. The root directory, the bitmap and the up-case table are marked allocated.
9. The bitmap's free space matches what macOS reported while the volume was
   mounted, within 1% of the volume.
10. Every free run shrinks inward to the device block size, so an extent never
    reaches into an allocated neighbour, and no extent lies outside the cluster
    heap.

`Ctrl-C` stops between unmap calls and mounts the volume again.

Keep a backup of any drive you run `exfatrim` on.

Before the first real trim per user, `exfatrim` also asks for confirmation; see
[First-run confirmation](#first-run-confirmation).

## What it does

`exfatrim` is for anyone who keeps an SSD formatted exFAT, the format macOS,
Windows and iPadOS all read and write, and wants the drive to learn which
blocks hold leftovers. It unmounts the volume, checks it, reads the exFAT
allocation bitmap, unmaps the free clusters only and mounts the volume again.
With `-v` it also samples the free space to check that the drive dropped the
leftovers.

## The macOS situation

macOS never sends TRIM to an exFAT volume.

- APFS trims free space at mount, and HFS+ during `fsck_hfs`.
- `trimforce enable` turns on TRIM only for third-party SATA drives on an AHCI
  controller. It does nothing for USB or Thunderbolt drives.
- macOS's exFAT driver (`com.apple.fskit.exfat` on macOS 26 and later) and its
  tools (`fsck_exfat`, `newfs_exfat`, `mount_exfat`) contain no TRIM path.

## Why that hurts an SSD

The controller keeps leftovers as live data, because nothing tells it
otherwise. Background garbage collection can reclaim only blocks the host has
overwritten. Under heavy write-and-delete churn the controller runs out of
erased blocks, sustained writes slow down and the drive can stall.

## Why not reformat as APFS

Windows cannot read APFS without third-party software. exFAT stays the format
for a drive shared with Windows machines, and `exfatrim` supplies the missing
TRIM.

## Does macOS pass TRIM to my drive?

Run `sudo exfatrim -n`. It refuses with a clear message when the drive's
`DKIOCGETFEATURES` lacks `DK_FEATURE_UNMAP`, which means macOS cannot send
TRIM to the drive over its current connection. A drive that reports
`DK_FEATURE_UNMAP` may still ignore the command; `-v` measures that.

## Build and install

`exfatrim` is written in C17, has no dependencies and builds with `clang` from
the Xcode command line tools and a POSIX `make`. The compiler runs with
`-std=c17 -pedantic -Wall -Wextra -Werror`.

```sh
make
sudo make install        # copies exfatrim to /usr/local/bin; PREFIX=... overrides
```

`make format` rewrites every source file with `clang-format` 22.1.8, and
`make format-check` fails on any difference and on any other `clang-format`
version.

### Source layout

Each module in `src/` has a header of the same name.

| Module | Holds |
|---|---|
| `exfat` | boot sector, checksum, geometry checks, FAT chains, root directory, allocation bitmap and the metadata cross-checks; reads only through a callback |
| `walk` | the bitmap's free runs, shrunk inward to the alignment unit, as extents; no I/O |
| `trim` | batches extents into `DKIOCUNMAP` calls and counts bytes after each successful call |
| `verify` | the sampling population, draws, the before and after reads and the 95% bound |
| `device` | raw aligned reads, the unmap ioctl, the exclusive lock, block size and feature queries |
| `volume` | resolving the argument, unmount, `fsck_exfat`, remount on every exit path and signals |
| `report` | human-readable sizes, the output lines and the first-run notice |
| `main` | option parsing and the order of the phases |

`exfat` and `walk` never exit, die or print. They return a status code and
write the refusal reason into a buffer the caller supplies, so the unit tests
can feed them corrupt volumes.

## Usage

```
sudo exfatrim [-n] [-v] [-y] [volume]
```

`volume` is a name (`SSD`), a mount point (`/Volumes/SSD`) or a disk id
(`disk4s2`, `/dev/disk4s2`, `/dev/rdisk4s2`). A name wins over a disk id. Omit
`volume` when exactly one exFAT volume is mounted. A volume that is already
unmounted can be named by disk id; it stays unmounted.

| Flag | Effect |
|---|---|
| `-n` | Dry run. Reads the volume and reports what a trim would cover. Changes nothing and never asks for confirmation. |
| `-v` | Verify. Samples free clusters that hold leftovers, trims, then reports how many the drive now reads as zeros. |
| `-y` | Skip the first-run confirmation for this run and write no marker. |

```
$ sudo exfatrim -n -v SSD
SSD (dry run): would trim 412 GB in 1873 range(s). Nothing changed.
  Free to use: 412 GB of 2.0 TB.
  Leftovers: 299 of 411 sampled free clusters (73%).

$ sudo exfatrim -v SSD
✓ SSD trimmed: 412 GB of free space handed back to the SSD (38s).
  Free to use: 412 GB of 2.0 TB. Ready to use. Next trim: ~Dec 8.
  Verified: 299 of 299 sampled leftovers now read as zeros (≥99.0% cleared, 95% confidence).
```

An error prints one `✗` line. Indented lines follow only when they give a
command to run or a choice to make.

### First-run confirmation

Before the first real trim per user, after the volume resolves and the
permission and UNMAP checks pass and before the unmount, `exfatrim` prints this
notice with the volume's mount point (its `/dev/diskNsM` path when the volume is
unmounted) and waits for an answer:

```
exfatrim clears the deleted files on /Volumes/SSD.
Your files and folders will not be changed.  After a trim undelete or recovery
tools will no longer bring back any deleted file.

If you are trying to recover deleted files from this volume STOP HERE.

Continue? [y/N]
```

- `y` or `Y` continues. Any other answer, or end of input, exits 1 with the
  volume untouched and still mounted.
- A `y` writes an empty marker file, `/var/db/exfatrim/lectured/<user>`, where
  `<user>` is `SUDO_USER` when set and otherwise the name of the real uid. A
  marker suppresses the notice. `exfatrim` creates the directory and its
  parents (mode 0755) when they are absent. Delete the marker to see the notice
  again.
- If writing the marker fails, `exfatrim` prints one warning line on stderr and
  continues the trim you confirmed. The notice shows again on the next run.
- `-y` skips the notice for that run and writes no marker.
- With no terminal on stdin, no marker and no `-y`, `exfatrim` exits 1 and says
  to run it in a terminal or pass `-y`.
- `-n` never asks.

### Monthly routine

Run `sudo exfatrim -v SSD` once a month. `sudo exfatrim -n -v SSD` tells you
whether a trim is due: the share of sampled free space that still holds
leftovers.

### Exit codes

| Code | Meaning |
|---|---|
| 0 | Success. With `-v`, every dirty sample read as zeros, or no dirty sample existed. |
| 1 | Refused or error, including a first-run confirmation that was declined. |
| 2 | Usage error. |
| 3 | `-v` only. At least one dirty sample still holds leftovers; the trim is not confirmed. |
| 4 | `exfatrim` unmounted the volume and could not mount it again. Run `diskutil mount <disk id>` or replug the drive. Code 4 takes precedence over 1 and 3. |

## How `-v` works

A free cluster can show a trim only if it held leftovers first. `-v` therefore
samples free clusters and keeps the ones with a nonzero byte, which this section
calls dirty samples. A dirty sample is the statistical unit; the leftovers are
what it holds. `-v` trims, then reads the dirty samples again.

- Population. Free clusters that lie wholly inside an extent `exfatrim` sends.
  A free cluster at the edge of a run may be shrunk out of its extent and never
  unmapped, so it is not eligible.
- Draws. Distinct clusters chosen uniformly at random with
  `arc4random_uniform`, stopping at 299 dirty samples or at
  min(3,000, population) draws.
- Read size. The first `min(cluster size, 1 MiB)` of each cluster. A sampled
  cluster is unmapped whole, so its first MiB stands for it, and a run on 32
  MiB clusters reads about 3 GB at most.
- Timing. The first read happens on the unmounted volume after every safety
  check has passed and before the first unmap. The second read happens after
  the last unmap and before the volume mounts again, because mounting writes
  and frees clusters of its own.
- Bound. If all N dirty samples read as zeros, the share of leftovers the drive
  cleared is at least `0.05^(1/N)` with 95% confidence. N = 299 gives 99.0%,
  N = 59 gives 95.0% and N = 30 gives 90.4%.

If no dirty sample exists, the sampled free space already reads as zeros and the
run exits 0.

Exit 3 means at least one dirty sample still holds leftovers. A drive that
ignored TRIM and a drive that does not return zeros for trimmed blocks look the
same, so exit 3 says the trim could not be confirmed and does not say which
cause applies.

With `-n -v`, `exfatrim` draws the same sample on the mounted volume,
read-only, and reports the share that still holds leftovers.

## Testing

`make test` needs macOS 26 or later (`diskutil image`), `python3` and no
`sudo`. It builds and runs the unit tests, then the disk-image tests.

Unit tests (`test/unit.c`) link `src/exfat.c` and `src/walk.c` to an in-memory
read callback and need no disk image. They build a valid volume in memory, then
break it one structure at a time. Each refusal case asserts the specific
status code and the user-facing text: a bad signature or checksum, revision 2,
a FAT offset below 24 sectors, a heap overlapping the FAT, the media-failure
and dirty flags, a bitmap length one too long or one too short, a self-loop and
a two-cluster loop in the bitmap chain, a chain leaving the heap, two bitmaps
and a root directory marked free. The walk cases cover a run shorter than the
alignment unit, a run touching the heap's end, clusters smaller than the
alignment unit, no free run and every cluster free.

The image tests attach disk images they create, run `exfatrim` only against
them, address each by the device id they recorded or the unique volume name
they gave it, and detach them on exit.

- RAW images (`diskutil image create blank --format RAW`) read unmapped blocks
  as zeros, like a drive that honours TRIM. A `-v` run on one must exit 0 with
  at least one dirty sample, all cleared.
- SPARSE images (`hdiutil create -type SPARSE`) accept `DKIOCUNMAP` and keep
  the data, like a drive that ignores TRIM. A `-v` run on one must exit 3.
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
- The first-run confirmation runs through a pseudo-terminal: `n` and end of
  input exit 1 with the volume still mounted and its files unchanged; `y`
  trims and writes the marker; a second run with the marker asks nothing; `-n`
  and `-y` never ask; with no terminal, no marker and no `-y` the run exits 1.

Three environment variables exist for the tests only:

- `EXFATRIM_TEST_BLOCK=<bytes>` raises the alignment unit to a power of two at
  or above the device block size and at most `UINT32_MAX`. Disk images report
  512-byte blocks, so this exercises the inward rounding that a drive with
  4096-byte blocks needs.
- `EXFATRIM_TEST_SKIP_FSCK=1` skips the `fsck_exfat` gate, so a test can show
  that `exfatrim`'s own checks refuse a corrupted volume.
- `EXFATRIM_TEST_LECTURE_DIR=<path>` replaces `/var/db/exfatrim/lectured`, so a
  test can write the first-run marker without privileges.

### Checks on every push

CI builds the tool and runs `make test`. It also enforces these checks:

- `shellcheck` accepts `test/test.sh`, and `python3 -I -m py_compile` accepts
  every Python file under `test/`.
- `make format-check` finds every `.c` and `.h` file formatted by
  `clang-format` 22.1.8.
- commitlint requires every commit message in the pushed range to follow the
  Conventional Commits format (`@commitlint/config-conventional`): a `type:` or
  `type(scope):` prefix, such as `feat:` or `fix:`, then a subject.
- yamlfmt 0.21.0 requires every file under `.github/workflows/` to match its
  formatting (`yamlfmt -lint`).

The commitlint and yamlfmt workflows live in `.github/workflows/` once the
lint branch lands.

## License and warranty

`exfatrim` is MIT licensed (see `LICENSE`) and comes with no warranty. A TRIM
cannot be undone. Keep a backup of any drive you run it on.
