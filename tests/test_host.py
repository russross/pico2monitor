"""Host framing rejects corrupted data and serial reads tolerate fragmentation."""

import os
import pty
import threading
import unittest

from pico import DETACH, Frame, Kind, SerialPort, UploadError, split_output


class HostTests(unittest.TestCase):
    def test_serial_owner_is_exclusive_and_released_on_close(self) -> None:
        master, slave = pty.openpty()
        name = os.ttyname(slave)
        port = SerialPort(name)
        try:
            with self.assertRaisesRegex(UploadError, "already in use"):
                SerialPort(name)
            port.close()
            port = SerialPort(name)
        finally:
            port.close()
            os.close(master)
            os.close(slave)

    def test_detach_sequence_fragmentation(self) -> None:
        for split in range(len(DETACH) + 1):
            output, pending, detached = split_output(b"hello" + DETACH[:split])
            self.assertEqual(output, b"hello")
            if split == len(DETACH):
                self.assertTrue(detached)
            else:
                self.assertFalse(detached)
                self.assertEqual(split_output(pending + DETACH[split:]), (b"", b"", True))
        self.assertEqual(split_output(b"\x1b[x"), (b"\x1b[x", b"", False))

    def test_binary_payload_and_corruption(self) -> None:
        payload = bytes(range(256)) * 4
        frame = Frame(Kind.DATA, 0x123456789ABCDEF0, 7, 4096, payload)
        packet = frame.encode()
        self.assertLess(len(packet), 1100)
        self.assertEqual(Frame.decode(packet[1:-1]), frame)
        damaged = bytearray(packet[1:-1])
        damaged[len(damaged) // 2] ^= 1
        with self.assertRaises(UploadError):
            Frame.decode(bytes(damaged))
        with self.assertRaises(UploadError):
            Frame.decode(b"\xff")

    def test_fragmented_serial_response_and_deadline(self) -> None:
        master, slave = pty.openpty()
        port = SerialPort(os.ttyname(slave))
        frame = Frame(Kind.ACK, 9, 3, 2048)

        def writer() -> None:
            for byte in b"\0broken\0" + frame.encode():
                os.write(master, bytes([byte]))

        thread = threading.Thread(target=writer)
        thread.start()
        try:
            self.assertEqual(port.receive(1), frame)
            with self.assertRaises(TimeoutError):
                port.receive(0.01)
        finally:
            thread.join()
            port.close()
            os.close(master)
            os.close(slave)


if __name__ == "__main__":
    unittest.main()
