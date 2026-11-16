#!/usr/bin/python3
"""Read the current T5 boot selector's timings, optionally saving them."""

import argparse
import os
from pathlib import Path
import struct
import tempfile
from datetime import datetime, timezone


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--save", action="store_true")
    args = parser.parse_args()
    handoff = Path("/proc/device-tree/chosen/t5-loader-handoff")
    if not handoff.is_file():
        return
    data = handoff.read_bytes()
    if len(data) != 128 or data[:8] != b"T5D115OK":
        return
    fields = struct.unpack("<16Q", data)
    if fields[1] != 2 or fields[11] != 0 or not fields[15]:
        return
    decompressed = fields[14] & 0xFFFFFFFF
    verified = fields[14] >> 32
    handoff_ms = fields[15]
    if not 0 <= decompressed <= verified <= handoff_ms:
        raise ValueError("Invalid loader timings")
    boot_id = Path("/proc/sys/kernel/random/boot_id").read_text().strip()
    report = (
        f"Boot ID {boot_id}\n"
        f"Recorded {datetime.now(timezone.utc).isoformat()}\n"
        f"Selected Linux, gzip {fields[13]} bytes, raw {fields[12]} bytes\n"
        f"Menu confirmation through decompression {decompressed} ms\n"
        f"Image validation including CRC32 {verified - decompressed} ms\n"
        f"Kernel handoff preparation {handoff_ms - verified} ms\n"
        f"Total up to kernel handoff {handoff_ms} ms\n"
    )
    print(report, end="")
    if args.save:
        directory = Path("/var/log/t5-boot")
        directory.mkdir(mode=0o750, exist_ok=True)
        with tempfile.NamedTemporaryFile(dir=directory, delete=False, mode="w") as file:
            file.write(report)
        os.replace(file.name, directory / f"{boot_id}.log")
        logs = sorted(directory.glob("*.log"), key=lambda path: path.stat().st_mtime,
                      reverse=True)
        for old in logs[10:]:
            old.unlink()


if __name__ == "__main__":
    main()
