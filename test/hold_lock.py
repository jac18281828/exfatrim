"""Run a command while holding an exclusive lock on a raw device.

usage: hold_lock.py DEVICE COMMAND [ARG...]
Exits with the command's status.
"""
import os
import subprocess
import sys

fd = os.open(sys.argv[1], os.O_RDWR | os.O_EXLOCK)
try:
    sys.exit(subprocess.call(sys.argv[2:]))
finally:
    os.close(fd)
