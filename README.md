# exfatrim

`exfatrim` trims the free space of an exFAT SSD on macOS. macOS trims APFS
and HFS+ volumes itself but never sends TRIM to an exFAT volume, the format
to use for a drive shared with Windows. Without TRIM, the SSD keeps the
leftovers of every deleted file as live data. Under heavy write-and-delete
use it runs out of erased blocks, writes slow down and the drive can stall.

`exfatrim` unmounts the volume, checks it with `fsck_exfat` and reads the
exFAT allocation bitmap. It then tells the SSD to discard only the clusters
the bitmap marks free, and mounts the volume again. Your files and folders
are never touched. The leftovers of deleted files are discarded, so undelete
tools can no longer recover them. `exfatrim` refuses the volume if any check
fails. Keep a backup, as with any tool that writes to a disk.

Tested on a 2 TB Crucial X9 Pro over USB-C on macOS 27.0.1, Apple silicon.

## Install

```sh
make
sudo make install        # installs to /usr/local/bin
```

## Use

Run it about once a month, with the drive plugged in:

```sh
exfatrim                  # trim the one mounted exFAT volume
exfatrim SSD              # trim a volume by name
exfatrim /Volumes/SSD     # or by mount point
exfatrim -n               # dry run: show what would be trimmed
exfatrim -v               # trim, then check the drive cleared it
exfatrim --verify 99      # check at 99% confidence instead of 95%
exfatrim -n -v            # show how much free space holds leftovers
exfatrim -y SSD           # skip the first-run question (scripts)
```

A trim with `-v` prints three lines:

```
✓ SSD trimmed: 412 GB of free space handed back to the SSD (38s).
  Free to use: 412 GB of 2.0 TB. Ready to use. Next trim: ~Dec 8.
  Trim checked: the drive cleared what was trimmed.
```

The first real trim asks you to confirm, once per user. `-n` never asks and
never changes the drive. If `-n` refuses because macOS cannot send TRIM to
the drive over its connection, `exfatrim` cannot help that drive.

`-v` picks up to 299 random free clusters that still hold leftovers, reads
them before the trim and again after: about 80 MB on a drive with 128 KiB
clusters, and up to about 430 MB when few free clusters hold leftovers. A
drive that ignores TRIM fails the check:

```
✗ Trim not confirmed: the drive still returns old data. It may ignore TRIM, or keep data until it erases.
```

## Exit status

| Code | Meaning |
|---|---|
| 0 | Done |
| 1 | Refused or failed |
| 2 | Bad arguments |
| 3 | Trimmed, but `-v` could not confirm the drive cleared it |
| 4 | The volume did not mount again; run `diskutil mount <disk>` or replug |

## Development

`make test` runs unit tests and end-to-end tests on disk images it creates.
It needs macOS 26 or later and `python3`. `make format` formats the sources
with clang-format 22.1.8. CI runs the tests and a format check on every push,
and commit messages follow Conventional Commits.

## License

MIT; see `LICENSE`. No warranty.
