"""Shared executable-contract scenarios for emulator and protected hardware."""

from typing import Protocol


class Session(Protocol):
    def load(self, name: str) -> None: ...
    def command(self, text: str) -> bytes: ...


def check_exec(session: Session) -> None:
    session.load("rwx.elf")
    output = session.command("run")
    assert b"RWX sandbox OK" in output and b"fault" not in output and b"exit:" not in output, output
    session.load("assembly.elf")
    assert b"hello from assembly" in session.command("run")
    session.command("setenv COURSE 'assembly lab'")
    session.load("exec.elf")
    output = session.command('run one "two words" ""')
    assert b"exec data=7 bss=0 argc=4" in output, output
    assert b"arg: two words\r\narg: \r\n" in output, output
    assert b"env: COURSE=assembly lab" in output, output
    assert b"exec heap OK" in output and b"exit:" not in output, output
    session.command("reset")
    assert b"exec data=7 bss=0" in session.command("run")

    # Pending release can be cancelled before reset, but cannot be reversed once
    # the pristine bytes have actually been reclaimed for the guest.
    session.command("reset-data off")
    session.command("reset-data on")
    session.command("reset")
    assert b"exec data=7 bss=0" in session.command("run")
    session.command("reset-data off")
    assert b"effective=off" in session.command("reset")
    output = session.command("run")
    assert b"exec data=7 bss=0" in output and b"exec heap OK" in output, output
    session.command("reset")
    assert b"exec data=99 bss=0" in session.command("run")
    assert b"reload" in session.command("reset-data on")
    session.command("unsetenv COURSE")
    assert b"COURSE=" not in session.command("env")
    session.command("reset-data auto")
