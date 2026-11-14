"""Shared executable-contract scenarios for emulator and protected hardware."""

import time
from typing import Protocol

from pico import SerialPort


class Session(Protocol):
    console: SerialPort

    def load(self, name: str, *, reset: bool = True) -> None: ...
    def command(self, text: str) -> bytes: ...
    def until(self, marker: bytes, timeout: float = 8) -> bytes: ...


def check_exec(session: Session) -> None:
    session.load("rwx.elf")
    output = session.command("run")
    assert b"RWX sandbox OK" in output and b"fault" not in output and b"exit:" not in output, output
    session.load("assembly.elf")
    assert b"20000000" in session.command("map")
    assert b"hello from assembly" in session.command("run")
    session.load("exec.elf")
    output = session.command('run one "two words" ""')
    assert b"exec data=7 bss=0 argc=4" in output, output
    assert b"arg: two words\r\narg: \r\n" in output, output
    assert b"env:" not in output, output
    assert b"exec heap OK" in output and b"exit:" not in output, output
    output = session.command('run again "two words" ""')
    assert b"exec data=7 bss=0 argc=4" in output, output
    assert b"arg: again\r\narg: two words\r\narg: \r\n" in output, output

    session.load("exec.elf", reset=False)
    assert b"reset unavailable" in session.command("reset")
    assert b"exec data=7 bss=0" in session.command("run")
    assert b"reset unavailable" in session.command("reset")
    assert b"run requires a ready" in session.command("run")
    session.load("exec.elf")
    assert b"exec data=7 bss=0" in session.command("run")
    session.command("reset")
    assert b"exec data=7 bss=0" in session.command("run")


def check_threads(session: Session) -> None:
    session.load("threads.elf")
    for _ in range(2):
        output = session.command("run")
        assert b"thread replacement OK" in output and b"exit:" not in output, output
        info = session.command("info")
        assert b"state: exited" in info, info
        assert b"thread 0: unused" in info and b"thread 1: unused" in info, info

    session.load("threads_busy.elf")
    session.console.write(b"run\r")
    session.until(b"two threads running\r\n")
    for _ in range(2):
        time.sleep(0.02)
        session.console.write(b"\x1b")
        session.until(b"pico> ")
        info = session.command("info")
        assert b"state: stopped" in info, info
        assert b"thread 0: runnable" in info and b"thread 1: runnable" in info, info
        assert b"run requires a ready" in session.command("run")
        assert b"state: stopped" in session.command("info")
        session.console.write(b"resume\r")
    time.sleep(0.02)
    session.console.write(b"\x1b")
    session.until(b"pico> ")
    session.command("reset")
    assert b"state: ready" in session.command("info")

    session.load("threads_fault.elf")
    output = session.command("run")
    assert b"guest fault: cause=3 " in output and b"thread=1" in output, output
    assert b"state: faulted" in session.command("info")
    assert b"resume requires" in session.command("resume")
