"""Run a command on a pseudo-terminal and answer its first-run prompt.

usage: pty_run.py ANSWER COMMAND [ARG...]
ANSWER is the line to send when "[y/N]" appears, EOF to send end of input, or
NONE to expect no prompt. Prints the command's output and exits with its
status, or with 99 when it prompted although ANSWER is NONE.
"""
import os
import pty
import select
import signal
import sys
import time

PROMPT = b"[y/N]"
TIMEOUT = 180

answer, command = sys.argv[1], sys.argv[2:]
pid, fd = pty.fork()
if pid == 0:
    os.execvp(command[0], command)

out = b""
prompted = False
unexpected = False
deadline = time.time() + TIMEOUT
while time.time() < deadline:
    ready, _, _ = select.select([fd], [], [], 1)
    if not ready:
        continue
    try:
        data = os.read(fd, 4096)
    except OSError:
        break
    if not data:
        break
    out += data
    if PROMPT in out and not prompted:
        prompted = True
        if answer == "NONE":
            unexpected = True
            os.kill(pid, signal.SIGTERM)
            break
        os.write(fd, b"\x04" if answer == "EOF" else answer.encode() + b"\n")
else:
    os.kill(pid, signal.SIGKILL)

_, status = os.waitpid(pid, 0)
sys.stdout.write(out.decode(errors="replace").replace("\r\n", "\n"))
sys.exit(99 if unexpected else os.waitstatus_to_exitcode(status))
