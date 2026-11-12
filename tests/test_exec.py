"""Shared executable-contract scenarios for emulator and protected hardware."""

from typing import Protocol


class Session(Protocol):
    def load(self, name: str, *, reset: bool = True) -> None: ...
    def command(self, text: str) -> bytes: ...


def check_exec(session: Session) -> None:
    session.load("rwx.elf")
    output = session.command("run")
    assert b"RWX sandbox OK" in output and b"fault" not in output and b"exit:" not in output, output
    session.load("assembly.elf")
    assert b"hello from assembly" in session.command("run")
    session.load("exec.elf")
    output = session.command('run one "two words" ""')
    assert b"exec data=7 bss=0 argc=4" in output, output
    assert b"arg: two words\r\narg: \r\n" in output, output
    assert b"env:" not in output, output
    assert b"exec heap OK" in output and b"exit:" not in output, output
    session.command("reset")
    assert b"exec data=7 bss=0" in session.command("run")

    session.load("exec.elf", reset=False)
    assert b"reset unavailable" in session.command("reset")
    assert b"exec data=7 bss=0" in session.command("run")
    assert b"reset unavailable" in session.command("reset")
    assert b"run requires a ready" in session.command("run")
    session.load("exec.elf")
    assert b"exec data=7 bss=0" in session.command("run")
    session.command("reset")
    assert b"exec data=7 bss=0" in session.command("run")
