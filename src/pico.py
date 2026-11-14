#!/usr/bin/env python3
"""Connect to the Pico monitor and upload a RISC-V ELF executable."""

from __future__ import annotations

import argparse
import binascii
import errno
import fcntl
import os
import select
import shlex
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
    CLAIM = 9
    ATTACHED = 10
    INPUT = 11
    OUTPUT = 12
    RELEASE = 13
    RELEASED = 14
    PING = 15
    ERROR = 16


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
            try:
                fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as error:
                raise UploadError(f"serial interface already in use: {path}") from error
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
    path: Path,
    port_name: str,
    *,
    preflight: bool = True,
    console: SerialPort | None = None,
    port: SerialPort | None = None,
) -> str:
    data = read_image(path, preflight=preflight)
    name = path.name.encode("ascii", errors="replace")[:63]
    owned = port is None
    if port is None:
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
        if owned:
            port.close()


def discover_port(interface: str, option: str) -> str:
    candidates = sorted(Path("/dev/serial/by-id").glob(f"*RV32_SRAM_monitor*{interface}*"))
    if len(candidates) != 1:
        raise UploadError(f"specify {option} with the monitor's {interface} CDC device")
    return str(candidates[0])


def check_port_pair(console_name: str, upload_name: str) -> None:
    """Explicit device paths must still refer to the same USB board."""

    def usb_device(name: str) -> Path | None:
        device = Path("/sys/class/tty") / Path(name).resolve().name / "device"
        for parent in device.resolve().parents:
            if (parent / "idVendor").exists() and (parent / "idProduct").exists():
                return parent
        return None

    console_device = usb_device(console_name)
    upload_device = usb_device(upload_name)
    if console_device is not None and upload_device is not None and console_device != upload_device:
        raise UploadError("console and upload interfaces belong to different USB devices")


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
def console_session(
    port_name: str,
    image: Path | None,
    upload_name: str | None,
    load_options: list[str] | None = None,
) -> None:
    attributes = termios.tcgetattr(sys.stdin.fileno())
    port = SerialPort(port_name)
    uploader: SerialPort | None = None
    try:
        if image is not None:
            if upload_name is None:
                raise UploadError("upload port unavailable")
            uploader = SerialPort(upload_name, port)
        tty.setraw(sys.stdin.fileno())
        if image is not None:
            if upload_name is None or uploader is None:
                raise UploadError("upload port unavailable")
            port.write(b"\x03")
            wait_console(port, b"pico> ")
            command = shlex.join(["load", *(load_options or [])]).encode()
            if len(command) > 2304:
                raise UploadError("load command exceeds 2304 bytes")
            port.write(command + b"\r")
            wait_console(port, b"waiting for ELF")
            send_file(image, upload_name, port=uploader)
            uploader.close()
            uploader = None
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
        if uploader is not None:
            uploader.close()
        port.close()


def claim_guest(port: SerialPort, session: int) -> None:
    """The claim retires upload RESULT replay only after the host received it."""
    for _attempt in range(5):
        port.write(Frame(Kind.CLAIM, session).encode())
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            try:
                reply = port.receive(deadline - time.monotonic())
            except TimeoutError:
                break
            if reply.session != session:
                continue
            if reply.kind == Kind.ATTACHED:
                return
            if reply.kind == Kind.ERROR:
                message = reply.payload.decode("ascii", errors="replace")
                if "idle monitor" not in message and "change pending" not in message:
                    raise UploadError(message)
                # Monitor load diagnostics and the final prompt may still be draining.
                time.sleep(0.05)
                break
    raise UploadError("guest attachment timed out; suspend the guest and retry")


def guest_session(port_name: str, image: Path | None = None) -> None:
    attributes = termios.tcgetattr(sys.stdin.fileno())
    port = SerialPort(port_name)
    session = time.monotonic_ns()
    attached = False
    try:
        tty.setraw(sys.stdin.fileno())
        if image is not None:
            message = send_file(image, port_name, port=port)
            write_output(f"{image}: {message}; ready to run\r\n".encode())
        claim_guest(port, session)
        attached = True
        write_output(b"Guest attached; Ctrl-] detaches. Ctrl-C/Escape suspends.\r\n")
        heartbeat = 0.0
        while True:
            if time.monotonic() >= heartbeat:
                port.write(Frame(Kind.PING, session).encode())
                heartbeat = time.monotonic() + 1
            if select.select([sys.stdin.fileno()], [], [], 0)[0]:
                data = os.read(sys.stdin.fileno(), 128)
                if not data or b"\x1d" in data:
                    return
                port.write(Frame(Kind.INPUT, session, payload=data).encode())
            try:
                reply = port.receive(0.05)
            except TimeoutError:
                continue
            if reply.session != session:
                continue
            if reply.kind == Kind.OUTPUT:
                write_output(reply.payload)
            elif reply.kind == Kind.RELEASED:
                attached = False
                return
            elif reply.kind == Kind.ERROR:
                raise UploadError(reply.payload.decode("ascii", errors="replace"))
    finally:
        try:
            if attached:
                port.write(Frame(Kind.RELEASE, session).encode(), timeout=0.2)
                deadline = time.monotonic() + 2
                while time.monotonic() < deadline:
                    reply = port.receive(deadline - time.monotonic())
                    if reply.session == session and reply.kind == Kind.RELEASED:
                        break
        except (OSError, TimeoutError, UploadError):
            pass
        finally:
            termios.tcsetattr(sys.stdin.fileno(), termios.TCSADRAIN, attributes)
            port.close()


def interrupted(number: int, frame: object) -> None:
    del number, frame
    raise KeyboardInterrupt


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument(
        "--send", action="store_true", help="upload, then attach guest I/O; type load first"
    )
    parser.add_argument("--attach", action="store_true", help="attach guest I/O without uploading")
    parser.add_argument("--detach", action="store_true", help="exit after --send completes")
    parser.add_argument(
        "image", type=Path, nargs="?", help="ELF file to upload; omit for console only"
    )
    parser.add_argument("--console-port", help="console CDC device (first serial interface)")
    parser.add_argument("--upload-port", help="upload CDC device (second serial interface)")
    arguments, load_options = parser.parse_known_args()
    if arguments.send and arguments.image is None:
        parser.error("--send requires an image")
    if arguments.attach and (arguments.send or arguments.image is not None):
        parser.error("--attach cannot be combined with an image or --send")
    if arguments.detach and not arguments.send:
        parser.error("--detach requires --send")
    if load_options and (arguments.send or arguments.image is None):
        parser.error("load options require an image in combined console mode")
    if not (arguments.send and arguments.detach) and not sys.stdin.isatty():
        parser.error("interactive mode requires a terminal; use --send FILE --detach for scripts")
    signal.signal(signal.SIGTERM, interrupted)
    try:
        # Validate the local file before opening or interrupting the board.
        if arguments.image is not None:
            read_image(arguments.image)
        upload_name = None
        if arguments.image is not None or arguments.attach:
            upload_name = arguments.upload_port or discover_port("if02", "--upload-port")
        if arguments.attach or (arguments.send and not arguments.detach):
            if upload_name is None:
                raise UploadError("guest port unavailable")
            guest_session(upload_name, arguments.image)
        elif arguments.send:
            if upload_name is None:
                raise UploadError("upload port unavailable")
            message = send_file(arguments.image, upload_name)
            print(f"{arguments.image}: {message}; ready to run")
        else:
            console_name = arguments.console_port or discover_port("if00", "--console-port")
            if upload_name is not None:
                check_port_pair(console_name, upload_name)
            print(
                "Connected; Ctrl-D or Ctrl-] detaches. Ctrl-C/Escape suspends the guest.",
                flush=True,
            )
            console_session(console_name, arguments.image, upload_name, load_options)
        return 0
    except (OSError, UploadError, TimeoutError) as error:
        print(f"pico: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("pico: cancelled", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
