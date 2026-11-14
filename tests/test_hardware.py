#!/usr/bin/env python3
"""Run guest execution and PMP probes against an already flashed monitor."""

from __future__ import annotations

import argparse
import os
import select
import sys
import time
from pathlib import Path

from test_exec import check_exec, check_threads

from pico import SerialPort, UploadError, send_file


class Hardware:
    def __init__(self, console: str, upload: str):
        self.console = SerialPort(console)
        self.upload = upload
        self.buffer = bytearray()

    def until(self, marker: bytes, timeout: float = 5) -> bytes:
        deadline = time.monotonic() + timeout
        while marker not in self.buffer:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([self.console.fd], [], [], remaining)[0]:
                raise UploadError(
                    f"console timeout waiting for {marker!r}: {bytes(self.buffer[-1000:])!r}"
                )
            data = os.read(self.console.fd, 4096)
            if not data:
                raise UploadError("console disconnected")
            self.buffer.extend(data)
        end = self.buffer.index(marker) + len(marker)
        output = bytes(self.buffer[:end])
        del self.buffer[:end]
        return output

    def command(self, text: str) -> bytes:
        self.console.write(text.encode() + b"\r")
        return self.until(b"pico> ")

    def load(self, name: str, *, reset: bool = True) -> None:
        self.console.write(b"load\r" if reset else b"load --no-reset\r")
        self.until(b"waiting for ELF")
        send_file(Path("build") / name, self.upload)
        self.until(b"pico> ")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise UploadError(message)


def run(board: Hardware) -> None:
    banner = board.until(b"pico> ")
    require(b"U-mode threads" in banner and b"EMULATOR" not in banner, "not the protected monitor")
    print(banner.decode(errors="replace"), end="", flush=True)
    check_exec(board)
    check_threads(board)
    print("thread creation, replacement, suspension, reset, and faults: passed", flush=True)
    print("default-linker ELF, arguments, brk, and load-time reset policy: passed", flush=True)
    board.load("hello.elf")
    for attempt in range(2):
        if attempt:
            board.command("reset")
        output = board.command("run")
        require(
            b"data=7 bss=0" in output and b"syscalls and reset state OK" in output,
            f"guest/reset failed: {output!r}",
        )
        require(
            b"guest fault" not in output and b"exit:" not in output, f"guest failed: {output!r}"
        )
    print("U-mode execution, syscalls, and reset: passed", flush=True)
    board.load("registers.elf")
    output = board.command("run")
    require(
        b"register frame OK" in output and b"exit:" not in output,
        f"register preservation failed: {output!r}",
    )
    print("full register preservation: passed", flush=True)

    board.load("echo.elf")
    board.console.write(b"run\r")
    board.until(b"returns to the monitor\r\n")
    board.console.write(b"alpha")
    board.until(b"alpha")
    board.console.write(b"\x03")
    board.until(b"pico> ")
    board.console.write(b"resume\r")
    time.sleep(0.02)
    board.console.write(b"beta")
    board.until(b"beta")
    board.console.write(b"\x1b")
    board.until(b"pico> ")
    print("blocking console read, suspend, and resume: passed", flush=True)

    board.load("loop.elf")
    board.console.write(b"run\r")
    board.until(b"Ctrl-C must still work\r\n")
    time.sleep(0.05)
    start = time.monotonic()
    board.console.write(b"\x03")
    board.until(b"pico> ")
    require(b"state: stopped" in board.command("info"), "busy guest did not stop")
    print(
        f"uncooperative guest suspension: passed ({1000 * (time.monotonic() - start):.1f} ms host round trip)",
        flush=True,
    )
    board.console.write(b"resume\r")
    time.sleep(0.05)
    board.console.write(b"\x1b")
    board.until(b"pico> ")

    # Each probe must fail in U-mode and leave the machine-mode shell alive.
    causes = {1: 7, 2: 5, 3: 5, 4: 2, 5: 1, 6: 2, 7: 2, 8: 5, 9: 5, 10: 5, 11: 7, 12: 7}
    for probe, cause in causes.items():
        board.load(f"probe{probe}.elf")
        output = board.command("run")
        require(
            f"guest fault: cause={cause} ".encode() in output,
            f"PMP/trap probe {probe} failed: {output!r}",
        )
        require(b"state: faulted" in board.command("info"), "faulted guest state lost")
        require(b"resume requires" in board.command("resume"), "faulted guest was resumable")
        print(f"protection probe {probe}: cause {cause}, shell survived", flush=True)
    board.load("hello.elf")
    print(board.command("map").decode(errors="replace"), end="", flush=True)
    print(board.command("info").decode(errors="replace"), end="", flush=True)
    print("hardware checks passed; hello.elf remains loaded and ready", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--console", required=True)
    parser.add_argument("--upload", required=True)
    arguments = parser.parse_args()
    board = None
    try:
        board = Hardware(arguments.console, arguments.upload)
        run(board)
        return 0
    except (OSError, UploadError, TimeoutError, AssertionError) as error:
        print(f"hardware check: {error}", file=sys.stderr)
        return 1
    finally:
        if board:
            board.console.close()


if __name__ == "__main__":
    raise SystemExit(main())
