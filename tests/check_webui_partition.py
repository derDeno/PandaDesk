"""After firmware/buildfs builds, run with the mklittlefs executable as argument."""

from pathlib import Path
import struct
import subprocess
import sys
import tempfile


root = Path(__file__).resolve().parents[1]
build = root / ".pio" / "build" / "pandadesk-c6"
table = (build / "partitions.bin").read_bytes()
filesystems = []
for offset in range(0, len(table), 32):
    magic, kind, subtype, address, size, label, flags = struct.unpack_from(
        "<HBBII16sI", table, offset
    )
    if magic != 0x50AA:
        break
    if kind == 1 and subtype in (0x81, 0x82, 0x83):
        filesystems.append((subtype, address, size, label.rstrip(b"\0")))

# PlatformIO 7.1.3 selects the last filesystem; there must be only our web UI.
assert filesystems == [(0x83, 0x620000, 0x1A0000, b"littlefs")], filesystems
image = build / "littlefs.bin"
assert image.stat().st_size == filesystems[0][2], "Image/partition size mismatch"
with tempfile.TemporaryDirectory(prefix="pandadesk-webui-") as directory:
    subprocess.run(
        [str(Path(sys.argv[1]).resolve()), "-b", "4096", "-p", "256",
         "-s", str(image.stat().st_size), "-u", ".", str(image)],
        cwd=directory,
        check=True,
    )
    for name in ("index.html", "version.txt"):
        assert (Path(directory) / name).read_bytes() == (root / "data" / name).read_bytes(), name

print("Web UI verified: littlefs at 0x620000, size 0x1A0000; web files match data/.")
