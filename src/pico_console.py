"""Open the monitor console; Ctrl-D or Ctrl-] detaches."""

import argparse
import os
import select
import signal
import sys
import termios
import tty
from pathlib import Path

from pico_send import SerialPort, UploadError

DETACH = b"\x1b]pico-console;detach\x07"


def split_output(data: bytes) -> tuple[bytes, bytes, bool]:
    """Keep a possible partial detach sequence until the next serial read."""
    position = data.find(DETACH)
    if position >= 0:
        return data[:position], b"", True
    for size in range(min(len(data), len(DETACH) - 1), 0, -1):
        if data.endswith(DETACH[:size]):
            return data[:-size], data[-size:], False
    return data, b"", False


def interrupted(number: int, frame: object) -> None:
    del number, frame
    raise KeyboardInterrupt


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="console CDC device (first serial interface)")
    arguments = parser.parse_args()
    candidates = sorted(Path("/dev/serial/by-id").glob("*RV32_SRAM_monitor*if00*"))
    path = arguments.port
    if path is None:
        if len(candidates) != 1:
            parser.error("specify --port with the monitor's console CDC device")
        path = str(candidates[0])
    if not sys.stdin.isatty():
        parser.error("run pico-console in an interactive terminal")
    port = None
    attributes = termios.tcgetattr(sys.stdin.fileno())
    signal.signal(signal.SIGTERM, interrupted)
    print("Connected; Ctrl-D or Ctrl-] detaches. Ctrl-C/Escape suspends the guest.", flush=True)
    try:
        port = SerialPort(path)
        tty.setraw(sys.stdin.fileno())
        pending = b""
        while True:
            ready = select.select([sys.stdin.fileno(), port.fd], [], [])[0]
            if port.fd in ready:
                data = os.read(port.fd, 4096)
                if not data:
                    raise UploadError("console disconnected")
                data, pending, detach = split_output(pending + data)
                while data:
                    data = data[os.write(sys.stdout.fileno(), data) :]
                if detach:
                    return 0
            if sys.stdin.fileno() in ready:
                data = os.read(sys.stdin.fileno(), 256)
                if not data or b"\x1d" in data:
                    return 0
                port.write(data)
    except (OSError, UploadError, TimeoutError) as error:
        print(f"\r\npico-console: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    finally:
        termios.tcsetattr(sys.stdin.fileno(), termios.TCSADRAIN, attributes)
        if port:
            port.close()


if __name__ == "__main__":
    raise SystemExit(main())
