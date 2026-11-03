"""Raw ELF upload over the monitor's dedicated CDC serial interface."""

from __future__ import annotations

import argparse
import binascii
import errno
import fcntl
import os
import select
import struct
import sys
import termios
import time
from dataclasses import dataclass
from enum import IntEnum
from pathlib import Path

PAYLOAD_LIMIT = 1024
FRAME_LIMIT = 1100
HEADER = struct.Struct("<BBQIIH")


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
    def __init__(self, path: str):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        self.buffer = bytearray()
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
            if remaining <= 0 or not select.select([self.fd], [], [], remaining)[0]:
                raise TimeoutError("serial response timed out")
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


def send_file(path: Path, port_name: str, *, preflight: bool = True) -> str:
    data = path.read_bytes()
    if not 52 <= len(data) <= 1024 * 1024:
        raise UploadError("ELF must be between 52 bytes and 1 MiB")
    if preflight and (
        data[:7] != b"\x7fELF\x01\x01\x01" or struct.unpack_from("<HH", data, 16) != (2, 243)
    ):
        raise UploadError("expected a little-endian RISC-V ELF32 ET_EXEC")
    name = path.name.encode("ascii", errors="replace")[:63]
    port = SerialPort(port_name)
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


def discover_upload_port() -> str:
    candidates = sorted(Path("/dev/serial/by-id").glob("*RV32_SRAM_monitor*if02*"))
    if len(candidates) != 1:
        raise UploadError("specify --port with the monitor's upload CDC device")
    return str(candidates[0])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--port", help="upload CDC device (second serial interface)")
    arguments = parser.parse_args()
    try:
        port_name = arguments.port or discover_upload_port()
        message = send_file(arguments.image, port_name)
        print(f"{arguments.image}: {message}; ready to run")
        return 0
    except (OSError, UploadError, TimeoutError) as error:
        print(f"pico-send: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("pico-send: cancelled", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
