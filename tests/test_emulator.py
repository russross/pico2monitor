"""End-to-end checks through rp2350js's USB controller and two pseudo-terminals."""

from __future__ import annotations

import binascii
import json
import os
import select
import struct
import subprocess
import tempfile
import time
import unittest
from collections.abc import Generator
from contextlib import contextmanager
from pathlib import Path
from typing import TextIO

from test_exec import check_exec

from pico_send import Frame, Kind, SerialPort, UploadError, exchange, send_file

ROOT = Path(__file__).resolve().parent.parent


class Emulator:
    def __init__(self, process: subprocess.Popen[str], log: TextIO) -> None:
        self.process = process
        self.log = log
        if self.process.stdout is None:
            raise RuntimeError("emulator stdout unavailable")
        ports = json.loads(self.process.stdout.readline())
        self.console = SerialPort(ports["console"])
        self.upload: str = ports["upload"]
        self.buffer = bytearray()
        try:
            self.until(b"pico> ")
        except BaseException:
            self.console.close()
            raise

    def close(self) -> None:
        self.console.close()

    def until(self, marker: bytes, timeout: float = 8) -> bytes:
        deadline = time.monotonic() + timeout
        while marker not in self.buffer:
            if self.process.poll() is not None:
                self.log.seek(0)
                raise AssertionError(f"emulator exited: {self.log.read()[-3000:]}")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                self.log.seek(0)
                raise AssertionError(
                    f"waiting for {marker!r}: {bytes(self.buffer)!r}\n{self.log.read()[-2000:]}"
                )
            if select.select([self.console.fd], [], [], min(remaining, 0.1))[0]:
                self.buffer.extend(os.read(self.console.fd, 4096))
        end = self.buffer.index(marker) + len(marker)
        data = bytes(self.buffer[:end])
        del self.buffer[:end]
        return data

    def command(self, text: str) -> bytes:
        self.console.write(text.encode() + b"\r")
        return self.until(b"pico> ")

    def load(self, name: str, *, reset: bool = True) -> None:
        self.console.write(b"load\r" if reset else b"load --no-reset\r")
        self.until(b"waiting for ELF")
        send_file(ROOT / "build" / name, self.upload)
        output = self.until(b"pico> ")
        if name.encode() not in output:
            raise AssertionError(f"loaded image name missing: {output!r}")


@contextmanager
def running_emulator() -> Generator[Emulator]:
    with (
        tempfile.TemporaryFile(mode="w+") as log,
        subprocess.Popen(
            [str(ROOT / "build/emulator"), str(ROOT / "build/monitor_emu.hex")],
            stdout=subprocess.PIPE,
            stderr=log,
            text=True,
        ) as process,
    ):
        try:
            emulator = Emulator(process, log)
            try:
                yield emulator
            finally:
                emulator.close()
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            log.seek(0)
            (ROOT / "build/emulator-last.log").write_text(log.read())


class MonitorTests(unittest.TestCase):
    emulator: Emulator

    def setUp(self) -> None:
        self.emulator = self.enterContext(running_emulator())

    def test_exec_contract(self) -> None:
        check_exec(self.emulator)

    def test_execution_and_repeatable_reset(self) -> None:
        e = self.emulator
        e.load("hello.elf")
        for attempt in range(2):
            if attempt:
                self.assertNotIn(b"no loaded", e.command("reset"))
            output = e.command("run")
            self.assertIn(b"data=7 bss=0", output)
            self.assertIn(b"syscalls and reset state OK", output)
            self.assertNotIn(b"exit:", output)
            self.assertIn(b"state: exited", e.command("info"))
        self.assertIn(b"resume requires", e.command("resume"))
        e.load("exit23.elf")
        self.assertIn(b"exit: 23", e.command("run"))

    def test_read_suspend_and_resume(self) -> None:
        e = self.emulator
        e.load("echo.elf")
        e.console.write(b"run\r")
        e.until(b"returns to the monitor\r\n")
        e.console.write(b"alpha")
        e.until(b"alpha")
        e.console.write(b"\x03")
        self.assertIn(b"suspended: Ctrl-C", e.until(b"pico> "))
        self.assertIn(b"state: stopped", e.command("info"))
        e.console.write(b"resume\r")
        time.sleep(0.02)
        e.console.write(b"beta")
        e.until(b"beta")
        e.console.write(b"\x1b")
        self.assertIn(b"suspended: Escape", e.until(b"pico> "))

    def test_console_line_input(self) -> None:
        e = self.emulator
        e.load("echo.elf")
        e.console.write(b"run\r")
        e.until(b"returns to the monitor\r\n")
        e.console.write(b"abx\x7fy\x08c")
        e.until(b"abx\b \by\b \bc")
        e.console.write(b"\r")
        self.assertIn(b"abc\r\n", e.until(b"abc\r\n"))
        e.console.write(b"\x04")
        e.until(b"EOF\r\n")
        e.console.write(b"partial")
        e.until(b"partial")
        e.console.write(b"\x04")
        e.until(b"partial")
        e.console.write(b"\x04")
        e.until(b"EOF\r\n")
        e.console.write(b"\x03")
        e.until(b"pico> ")

    def test_help(self) -> None:
        e = self.emulator
        output = e.command("help")
        for name in (
            "help",
            "load",
            "info",
            "map",
            "run",
            "resume",
            "reset",
            "bootsel",
            "quit",
        ):
            self.assertIn(f"  {name}".encode(), output)
            self.assertIn(b"Usage:", e.command(f"help {name}"))
        self.assertIn(b"unknown command: missing", e.command("help missing"))
        self.assertIn(b"usage: help [COMMAND]", e.command("help run extra"))
        self.assertIn(b"\x1b]pico-console;detach\x07", e.command("quit"))
        e.console.write(b"\x04")
        e.until(b"\x1b]pico-console;detach\x07")

    def test_infinite_guest_is_preemptible(self) -> None:
        e = self.emulator
        e.load("loop.elf")
        e.console.write(b"run\r")
        e.until(b"Ctrl-C must still work\r\n")
        time.sleep(0.05)
        e.console.write(b"\x03")
        e.until(b"pico> ")
        self.assertIn(b"state: stopped", e.command("info"))
        e.console.write(b"resume\r")
        time.sleep(0.05)
        e.console.write(b"\x03")
        e.until(b"pico> ")

    def test_full_register_frame(self) -> None:
        e = self.emulator
        e.load("registers.elf")
        output = e.command("run")
        self.assertIn(b"register frame OK", output)
        self.assertNotIn(b"exit:", output)

    def test_header_failure_invalidates_image(self) -> None:
        e = self.emulator
        e.load("hello.elf")
        data = bytearray((ROOT / "build/hello.elf").read_bytes())
        data[4] = 2
        with tempfile.TemporaryDirectory() as temporary:
            bad = Path(temporary) / "bad.elf"
            bad.write_bytes(data)
            e.console.write(b"load\r")
            e.until(b"waiting for ELF")
            with self.assertRaisesRegex(UploadError, "no runnable image"):
                send_file(bad, e.upload, preflight=False)
        e.until(b"pico> ")
        self.assertIn(b"state: empty", e.command("info"))
        e.load("hello.elf")
        self.assertIn(b"syscalls and reset state OK", e.command("run"))

    def test_crc_failure_and_recovery(self) -> None:
        e = self.emulator
        e.load("hello.elf")
        data = (ROOT / "build/hello.elf").read_bytes()
        e.console.write(b"load\r")
        e.until(b"waiting for ELF")
        port = SerialPort(e.upload)
        try:
            ready = port.receive(5)
            self.assertEqual(ready.kind, Kind.READY)
            session = ready.session
            metadata = struct.pack("<IIB", len(data), binascii.crc32(data) ^ 1, 1) + b"x"
            exchange(port, Frame(Kind.BEGIN, session, payload=metadata), 1, 0)
            sequence, offset = 1, 0
            while offset < len(data):
                block = data[offset : offset + 1024]
                exchange(
                    port,
                    Frame(Kind.DATA, session, sequence, offset, block),
                    sequence + 1,
                    offset + len(block),
                )
                offset += len(block)
                sequence += 1
            with self.assertRaisesRegex(UploadError, "whole-file CRC mismatch"):
                exchange(port, Frame(Kind.END, session, sequence, offset), sequence, offset)
        finally:
            port.close()
        e.until(b"pico> ")
        self.assertIn(b"state: empty", e.command("info"))
        e.load("hello.elf")
        self.assertIn(b"syscalls and reset state OK", e.command("run"))


if __name__ == "__main__":
    unittest.main()
