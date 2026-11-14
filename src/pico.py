#!/usr/bin/env python3
"""Connect to the Pico monitor and upload a RISC-V ELF executable."""

from __future__ import annotations

import argparse
import binascii
import errno
import fcntl
import os
import select
import signal
import struct
import sys
import termios
import time
import tty
from dataclasses import dataclass
from enum import IntEnum
from pathlib import Path

PAYLOAD_LIMIT = 1024
FRAME_LIMIT = 1100
HEADER = struct.Struct("<BBQIIH")
DETACH = b"\x1b]pico-console;detach\x07"


class Kind(IntEnum):
    READY = 1
    BEGIN = 2
    DATA = 3
    END = 4
    ABORT = 5
    ACK = 6
    NAK = 7
    RESULT = 8


class UploadError(Exception):
    """An expected transport, protocol, or executable error."""


@dataclass(frozen=True)
class Frame:
    kind: Kind
    session: int
    sequence: int = 0
    offset: int = 0
    payload: bytes = b""

    def encode(self) -> bytes:
        if len(self.payload) > PAYLOAD_LIMIT:
            raise UploadError("frame payload exceeds 1024 bytes")
        raw = HEADER.pack(1, self.kind, self.session, self.sequence, self.offset, len(self.payload))
        raw += self.payload
        raw += struct.pack("<I", binascii.crc32(raw))
        output = bytearray(b"\0\0")
        code_at, code = 1, 1
        for byte in raw:
            if byte == 0:
                output[code_at] = code
                code_at, code = len(output), 1
                output.append(0)
            else:
                output.append(byte)
                code += 1
                if code == 255:
                    output[code_at] = code
                    code_at, code = len(output), 1
                    output.append(0)
        output[code_at] = code
        output.append(0)
        return bytes(output)

    @classmethod
    def decode(cls, encoded: bytes) -> Frame:
        raw = bytearray()
        cursor = 0
        while cursor < len(encoded):
            code = encoded[cursor]
            cursor += 1
            if code == 0 or code - 1 > len(encoded) - cursor:
                raise UploadError("invalid COBS frame")
            raw += encoded[cursor : cursor + code - 1]
            cursor += code - 1
            if code < 255 and cursor < len(encoded):
                raw.append(0)
        if len(raw) < 24:
            raise UploadError("short frame")
        version, kind, session, sequence, offset, length = HEADER.unpack_from(raw)
        if version != 1 or length > PAYLOAD_LIMIT or len(raw) != length + 24:
            raise UploadError("invalid frame header")
        if struct.unpack_from("<I", raw, len(raw) - 4)[0] != binascii.crc32(raw[:-4]):
            raise UploadError("frame CRC mismatch")
        try:
            frame_kind = Kind(kind)
        except ValueError as error:
            raise UploadError("unknown frame type") from error
        return cls(frame_kind, session, sequence, offset, bytes(raw[20:-4]))


class SerialPort:
    def __init__(self, path: str, console: SerialPort | None = None):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        self.buffer = bytearray()
        self.console = console
        try:
            attributes = termios.tcgetattr(self.fd)
            attributes[0] = attributes[1] = attributes[3] = 0
            attributes[2] = termios.CS8 | termios.CREAD | termios.CLOCAL | termios.HUPCL
            attributes[4] = attributes[5] = termios.B115200
            attributes[6][termios.VMIN] = 0
            attributes[6][termios.VTIME] = 0
            termios.tcsetattr(self.fd, termios.TCSANOW, attributes)
            try:
                fcntl.ioctl(
                    self.fd,
                    termios.TIOCMBIS,
                    struct.pack("I", termios.TIOCM_DTR | termios.TIOCM_RTS),
                )
            except OSError as error:
                if error.errno != errno.ENOTTY:
                    raise
        except BaseException:
            os.close(self.fd)
            raise

    def close(self) -> None:
        os.close(self.fd)

    def write(self, data: bytes, timeout: float = 2) -> None:
        deadline = time.monotonic() + timeout
        while data:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([], [self.fd], [], remaining)[1]:
                raise TimeoutError("serial write timed out")
            try:
                count = os.write(self.fd, data)
            except BlockingIOError:
                continue
            if count == 0:
                raise UploadError("serial connection closed")
            data = data[count:]

    def receive(self, timeout: float) -> Frame:
        deadline = time.monotonic() + timeout
        while True:
            delimiter = self.buffer.find(0)
            if delimiter >= 0:
                encoded = bytes(self.buffer[:delimiter])
                del self.buffer[: delimiter + 1]
                if not encoded:
                    continue
                try:
                    return Frame.decode(encoded)
                except UploadError:
                    continue
            if len(self.buffer) > FRAME_LIMIT:
                self.buffer.clear()
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("serial response timed out")
            # Upload replies and console output share USB bandwidth. Drain both
            # interfaces so diagnostics cannot block completion of the upload.
            readers = [self.fd]
            if self.console is not None:
                readers.append(self.console.fd)
            ready = select.select(readers, [], [], remaining)[0]
            if not ready:
                raise TimeoutError("serial response timed out")
            if self.console is not None and self.console.fd in ready:
                write_output(read_console(self.console))
            if self.fd not in ready:
                continue
            try:
                block = os.read(self.fd, 4096)
            except BlockingIOError:
                continue
            if not block:
                raise UploadError("serial connection closed")
            self.buffer.extend(block)


def check_result(frame: Frame) -> str:
    if len(frame.payload) < 2:
        raise UploadError("invalid result frame")
    message = frame.payload[2:].decode("ascii", errors="replace")
    if frame.payload[0]:
        disposition = "previous image preserved" if frame.payload[1] else "no runnable image"
        raise UploadError(f"{message} ({disposition})")
    return message


def exchange(port: SerialPort, frame: Frame, expected_sequence: int, expected_offset: int) -> Frame:
    for _attempt in range(5):
        port.write(frame.encode())
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            try:
                reply = port.receive(max(0, deadline - time.monotonic()))
            except TimeoutError:
                break
            if reply.session != frame.session:
                continue
            if reply.kind == Kind.RESULT:
                check_result(reply)
                return reply
            if (
                frame.kind != Kind.END
                and reply.kind == Kind.ACK
                and reply.sequence == expected_sequence
                and reply.offset == expected_offset
            ):
                return reply
            if reply.kind == Kind.NAK:
                break
    raise UploadError(f"no acknowledgement for file offset {frame.offset}")


def read_image(path: Path, *, preflight: bool = True) -> bytes:
    data = path.read_bytes()
    if not 52 <= len(data) <= 1024 * 1024:
        raise UploadError("ELF must be between 52 bytes and 1 MiB")
    if preflight and (
        data[:7] != b"\x7fELF\x01\x01\x01" or struct.unpack_from("<HH", data, 16) != (2, 243)
    ):
        raise UploadError("expected a little-endian RISC-V ELF32 ET_EXEC")
    return data


def send_file(
    path: Path, port_name: str, *, preflight: bool = True, console: SerialPort | None = None
) -> str:
    data = read_image(path, preflight=preflight)
    name = path.name.encode("ascii", errors="replace")[:63]
    port = SerialPort(port_name, console)
    session: int | None = None
    try:
        try:
            ready = port.receive(5)
            while ready.kind != Kind.READY:
                ready = port.receive(5)
        except TimeoutError as error:
            raise UploadError(
                "monitor is not waiting for an upload; type 'load' in the console"
            ) from error
        session = ready.session
        metadata = struct.pack("<IIB", len(data), binascii.crc32(data), len(name)) + name
        exchange(port, Frame(Kind.BEGIN, session, payload=metadata), 1, 0)
        sequence, offset = 1, 0
        while offset < len(data):
            payload = data[offset : offset + PAYLOAD_LIMIT]
            exchange(
                port,
                Frame(Kind.DATA, session, sequence, offset, payload),
                sequence + 1,
                offset + len(payload),
            )
            offset += len(payload)
            sequence += 1
        reply = exchange(port, Frame(Kind.END, session, sequence, offset), sequence, offset)
        if reply.kind != Kind.RESULT:
            raise UploadError("monitor did not return a final result")
        return check_result(reply)
    except BaseException:
        if session is not None:
            try:
                port.write(Frame(Kind.ABORT, session).encode(), timeout=0.2)
            except (OSError, TimeoutError, UploadError):
                pass
        raise
    finally:
        port.close()


def discover_port(interface: str, option: str) -> str:
    candidates = sorted(Path("/dev/serial/by-id").glob(f"*RV32_SRAM_monitor*{interface}*"))
    if len(candidates) != 1:
        raise UploadError(f"specify {option} with the monitor's {interface} CDC device")
    return str(candidates[0])


def split_output(data: bytes) -> tuple[bytes, bytes, bool]:
    """Keep a possible partial detach sequence until the next serial read."""
    position = data.find(DETACH)
    if position >= 0:
        return data[:position], b"", True
    for size in range(min(len(data), len(DETACH) - 1), 0, -1):
        if data.endswith(DETACH[:size]):
            return data[:-size], data[-size:], False
    return data, b"", False


def read_console(port: SerialPort) -> bytes:
    data = os.read(port.fd, 4096)
    if not data:
        raise UploadError("console disconnected")
    return data


def write_output(data: bytes) -> None:
    while data:
        data = data[os.write(sys.stdout.fileno(), data) :]


def wait_console(port: SerialPort, marker: bytes) -> None:
    deadline = time.monotonic() + 8
    pending = b""
    while marker not in pending:
        remaining = deadline - time.monotonic()
        if remaining <= 0 or not select.select([port.fd], [], [], remaining)[0]:
            raise UploadError(f"console timeout waiting for {marker!r}")
        data = read_console(port)
        write_output(data)
        pending = (pending + data)[-4096:]


# Combined mode interrupts any current input or guest before arming the upload.
# The console descriptor remains open through upload and interactive operation.
def console_session(port_name: str, image: Path | None, upload_name: str | None) -> None:
    attributes = termios.tcgetattr(sys.stdin.fileno())
    port = SerialPort(port_name)
    try:
        tty.setraw(sys.stdin.fileno())
        if image is not None:
            if upload_name is None:
                raise UploadError("upload port unavailable")
            port.write(b"\x03")
            wait_console(port, b"pico> ")
            port.write(b"load\r")
            wait_console(port, b"waiting for ELF")
            send_file(image, upload_name, console=port)
        pending = b""
        while True:
            ready = select.select([sys.stdin.fileno(), port.fd], [], [])[0]
            if port.fd in ready:
                data, pending, detach = split_output(pending + read_console(port))
                write_output(data)
                if detach:
                    return
            if sys.stdin.fileno() in ready:
                data = os.read(sys.stdin.fileno(), 256)
                if not data or b"\x1d" in data:
                    return
                port.write(data)
    finally:
        termios.tcsetattr(sys.stdin.fileno(), termios.TCSADRAIN, attributes)
        port.close()


def interrupted(number: int, frame: object) -> None:
    del number, frame
    raise KeyboardInterrupt


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--send", action="store_true", help="upload only; type load in the console first"
    )
    parser.add_argument(
        "image", type=Path, nargs="?", help="ELF file to upload; omit for console only"
    )
    parser.add_argument("--console-port", help="console CDC device (first serial interface)")
    parser.add_argument("--upload-port", help="upload CDC device (second serial interface)")
    arguments = parser.parse_args()
    if arguments.send and arguments.image is None:
        parser.error("--send requires an image")
    if not arguments.send and not sys.stdin.isatty():
        parser.error("console mode requires an interactive terminal")
    signal.signal(signal.SIGTERM, interrupted)
    try:
        # Validate the local file before opening or interrupting the board.
        if arguments.image is not None:
            read_image(arguments.image)
        upload_name = None
        if arguments.image is not None:
            upload_name = arguments.upload_port or discover_port("if02", "--upload-port")
        if arguments.send:
            if upload_name is None:
                raise UploadError("upload port unavailable")
            message = send_file(arguments.image, upload_name)
            print(f"{arguments.image}: {message}; ready to run")
        else:
            console_name = arguments.console_port or discover_port("if00", "--console-port")
            print(
                "Connected; Ctrl-D or Ctrl-] detaches. Ctrl-C/Escape suspends the guest.",
                flush=True,
            )
            console_session(console_name, arguments.image, upload_name)
        return 0
    except (OSError, UploadError, TimeoutError) as error:
        print(f"pico: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("pico: cancelled", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
