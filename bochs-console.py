#!/usr/bin/env python3
"""Run a headless Bochs and connect its serial console to this terminal.

Usage: bochs-console.py bochs -q -f dot-bochsrc-nox

dot-bochsrc-nox attaches COM1 to /dev/tty, so Bochs's controlling terminal
is the serial line. This script runs Bochs on a pseudo-terminal and copies
bytes between it and the real terminal, because Bochs on its own leaves
the terminal unusable: it turns off Ctrl-C (so there is no way to quit),
turns off newline translation, and does not restore the terminal settings
when it exits.

Type Ctrl-A x to quit, Ctrl-A Ctrl-A to send a literal Ctrl-A.
If stdin is not a terminal (for example, commands piped in for a test),
its bytes are forwarded to xv6 and Bochs keeps running after EOF.
"""

import os
import pty
import select
import signal
import sys
import termios
import tty

CTRL_A = 0x01


def stop(pid):
    for sig in (signal.SIGTERM, signal.SIGKILL):
        try:
            os.kill(pid, sig)
        except ProcessLookupError:
            return
        for _ in range(20):
            if os.waitpid(pid, os.WNOHANG) != (0, 0):
                return
            select.select([], [], [], 0.05)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    pid, master = pty.fork()
    if pid == 0:
        os.execvp(sys.argv[1], sys.argv[1:])

    # Let "timeout" or a closed terminal stop Bochs too.
    def terminate(signum, frame):
        raise SystemExit(1)
    signal.signal(signal.SIGTERM, terminate)
    signal.signal(signal.SIGHUP, terminate)

    interactive = os.isatty(0)
    saved = None
    if interactive:
        saved = termios.tcgetattr(0)
        tty.setraw(0)
        # Keep output processing so xv6's "\n" starts a new line.
        attrs = termios.tcgetattr(1)
        attrs[1] |= termios.OPOST | termios.ONLCR
        termios.tcsetattr(1, termios.TCSANOW, attrs)
        sys.stderr.write("bochs-console: type Ctrl-A x to quit\r\n")

    inputs = [master, 0]
    escape = False
    try:
        while True:
            ready, _, _ = select.select(inputs, [], [])
            if master in ready:
                try:
                    data = os.read(master, 4096)
                except OSError:
                    data = b""
                if not data:
                    break  # Bochs exited
                os.write(1, data)
            if 0 in ready:
                data = os.read(0, 1024)
                if not data:
                    inputs.remove(0)
                    continue
                if not interactive:
                    os.write(master, data)
                    continue
                out = bytearray()
                for b in data:
                    if escape:
                        escape = False
                        if b in (ord("x"), ord("X")):
                            return
                        if b == CTRL_A:
                            out.append(CTRL_A)
                    elif b == CTRL_A:
                        escape = True
                    else:
                        out.append(b)
                if out:
                    os.write(master, bytes(out))
    finally:
        stop(pid)
        if saved is not None:
            termios.tcsetattr(0, termios.TCSADRAIN, saved)
            sys.stderr.write("\n")


if __name__ == "__main__":
    main()
