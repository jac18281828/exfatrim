"""Count how often a raw device refuses to open (EBUSY) until DONEFILE exists.

usage: poll_busy.py DEVICE DONEFILE
Prints the count. Stops after 30 seconds if DONEFILE never appears.
"""
import errno
import os
import sys
import time

device, done_file = sys.argv[1:3]
busy = 0
deadline = time.time() + 30
while time.time() < deadline and not os.path.exists(done_file):
    try:
        os.close(os.open(device, os.O_RDONLY))
    except OSError as e:
        if e.errno == errno.EBUSY:
            busy += 1
    time.sleep(0.01)
print(busy)
