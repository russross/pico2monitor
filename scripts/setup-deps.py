#!/usr/bin/env python3
"""Install pinned Pico build dependencies below the project directory."""

from __future__ import annotations

import hashlib
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Asset identity and both hashes keep the generated emulator and local fix paired.
EMULATOR_URL = "https://api.github.com/repos/c1570/rp2350js/releases/assets/724171802"
EMULATOR_SHA256 = "44731efa8dfcb06a3c0395ed0b2058b82d8eb10fbbaf6b4b74121d7be9c7a170"
PATCHED_EMULATOR_SHA256 = "acac377069a63640131e2a49345c57037ccf5288f7999aaa130ee159ccd5fd00"


@dataclass(frozen=True)
class Archive:
    destination: str
    marker: str
    url: str
    sha256: str
    strip: bool


ARCHIVES = (
    Archive(
        "sdk",
        "pico_sdk_init.cmake",
        "https://api.github.com/repos/raspberrypi/pico-sdk/tarball/2.3.1",
        "dc11eba18e41fc661fe222bbcef6c8db1682489ef557614edf592079beec34f8",
        True,
    ),
    Archive(
        "sdk/lib/tinyusb",
        "src/tusb.h",
        "https://api.github.com/repos/hathach/tinyusb/tarball/86ad6e56c1700e85f1c5678607a762cfe3aa2f47",
        "aa7bb3c995768c53bafe25c4801a1dc81443a77baaea87158d4cf5926feb073d",
        True,
    ),
    Archive(
        "toolchain",
        "bin/riscv32-pico-elf-gcc",
        "https://github.com/raspberrypi/pico-sdk-tools/releases/download/v2.3.1-0/riscv-toolchain-16-x86_64-lin.tar.gz",
        "fc36f1b37f99de54a358115443638023de34e15fe199e718e50cca4d212e7e9f",
        False,
    ),
    Archive(
        "cmake",
        "bin/cmake",
        "https://github.com/Kitware/CMake/releases/download/v4.4.4/cmake-4.4.4-linux-x86_64.tar.gz",
        "e5bb807f7728cb60cd8b27ebc97a2edb469b68655f21e844a600c3575b76f5bb",
        True,
    ),
    Archive(
        "picotool",
        "picotool",
        "https://github.com/raspberrypi/pico-sdk-tools/releases/download/v2.3.1-0/picotool-2.3.1-x86_64-lin.tar.gz",
        "878a1a1e7d8336b8be60300c7218f366d9790c0d3a211b181d6d172e80b1248e",
        True,
    ),
)


def install(archive: Archive) -> None:
    destination = ROOT / "deps" / archive.destination
    if (destination / archive.marker).exists():
        return
    print(f"Downloading {archive.destination}", flush=True)
    with tempfile.TemporaryDirectory(dir=ROOT) as temporary:
        folder = Path(temporary)
        download = folder / "download.tar.gz"
        urllib.request.urlretrieve(archive.url, download)
        with download.open("rb") as stream:
            checksum = hashlib.file_digest(stream, "sha256").hexdigest()
        if checksum != archive.sha256:
            raise RuntimeError(f"{archive.destination}: archive checksum mismatch")
        unpacked = folder / "unpacked"
        with tarfile.open(download) as tar:
            tar.extractall(unpacked, filter="data")
        # Release archives may include loose metadata beside their root directory.
        source = unpacked
        if archive.strip:
            directories = [entry for entry in unpacked.iterdir() if entry.is_dir()]
            if len(directories) != 1:
                raise RuntimeError(f"{archive.destination}: expected one archive root directory")
            source = directories[0]
        if not (source / archive.marker).is_file():
            raise RuntimeError(f"{archive.destination}: archive missing {archive.marker}")
        if destination.exists():
            if not destination.is_dir() or any(destination.iterdir()):
                raise RuntimeError(
                    f"{destination}: incomplete dependency exists; inspect it before replacing"
                )
            destination.rmdir()
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(str(source), destination)


def main() -> int:
    try:
        for archive in ARCHIVES:
            install(archive)
        header = ROOT / "deps/rp2350js-c.h"
        if not header.exists():
            header.parent.mkdir(parents=True, exist_ok=True)
            request = urllib.request.Request(
                EMULATOR_URL, headers={"Accept": "application/octet-stream"}
            )
            with urllib.request.urlopen(request) as response:
                data = response.read()
            if hashlib.sha256(data).hexdigest() != EMULATOR_SHA256:
                raise RuntimeError("emulator asset checksum mismatch")
            header.write_bytes(data)
        checksum = hashlib.sha256(header.read_bytes()).hexdigest()
        if checksum == EMULATOR_SHA256:
            subprocess.run(
                ["patch", "-p1", "-i", str(ROOT / "scripts/rp2350js-c.patch")], cwd=ROOT, check=True
            )
        # Verify patch output too, including the first installation.
        if hashlib.sha256(header.read_bytes()).hexdigest() != PATCHED_EMULATOR_SHA256:
            raise RuntimeError("emulator header differs from the pinned release and ORC.B patch")
        return 0
    except (OSError, RuntimeError, tarfile.TarError, subprocess.CalledProcessError) as error:
        print(f"setup-deps: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
