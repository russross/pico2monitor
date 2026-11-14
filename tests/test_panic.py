"""Exercise production panic assembly with controlled two-core scheduling."""

from __future__ import annotations

import subprocess
import unittest
from enum import IntEnum
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SYMBOLS = (
    "monitor_panic_owner",
    "monitor_panic_parked",
    "monitor_cores_started",
    "monitor_gate_lock",
    "monitor_dispatch",
    "monitor_return",
    "monitor_gate",
    "monitor_idle",
    "monitor_kernel_fault",
    "monitor_bootsel",
    "monitor_panic_halt",
    "__StackTop",
)


class Scenario(IntEnum):
    GATE_CORE0 = 0
    GATE_CORE1 = 1
    SIMULTANEOUS_CORE0 = 2
    SIMULTANEOUS_CORE1 = 3
    PEER_IN_C = 4
    PEER_IN_GUEST = 5
    BEFORE_PEER_LAUNCH = 6
    UNRESPONSIVE_PEER = 7
    NESTED_OWNER = 8
    NESTED_PEER = 9


class PanicTests(unittest.TestCase):
    def test_recovery(self) -> None:
        build = ROOT / "build"
        listing = subprocess.check_output(
            [ROOT / "deps/toolchain/bin/riscv32-pico-elf-nm", build / "monitor_emu.elf"],
            text=True,
        )
        addresses: dict[str, int] = {}
        for line in listing.splitlines():
            fields = line.split()
            if len(fields) == 3:
                addresses[fields[2]] = int(fields[0], 16)
        for scenario in Scenario:
            with self.subTest(scenario=scenario.name):
                result = subprocess.run(
                    [
                        ROOT / "build/test-panic",
                        build / "monitor_emu.hex",
                        str(scenario),
                        *(hex(addresses[name]) for name in SYMBOLS),
                    ],
                    check=False,
                    capture_output=True,
                    text=True,
                    timeout=20,
                )
                self.assertEqual(result.returncode, 0, result.stderr[-4000:])
