#!/usr/bin/env python3

from __future__ import annotations

import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


RECORD_SIZE = 640
HEADER = struct.Struct("<IHHIIIIIqQ")


def record(event: int, arguments: list[int], snapshot: bytes = b"") -> bytes:
    data = bytearray(RECORD_SIZE)
    HEADER.pack_into(data, 0, 0x52544444, 1, event, RECORD_SIZE, 10, 20, 0, 0, 30, 40)
    struct.pack_into("<10Q", data, HEADER.size, *(arguments + [0] * (10 - len(arguments))))
    struct.pack_into("<HH", data, HEADER.size + 80, 0, len(snapshot))
    data[384 : 384 + len(snapshot)] = snapshot
    return bytes(data)


def main() -> int:
    decoder = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        trace = root / "native.bin"
        trace.write_bytes(
            record(1, [0x140000000, 8, 0x2000000])
            + record(10, [1, 2, 3, 4], b"generic_effect_name\0")
        )
        result = subprocess.run(
            [sys.executable, str(decoder), str(trace)],
            check=True,
            capture_output=True,
            text=True,
        )
        summary = json.loads(result.stdout)
        assert summary["record_count"] == 2
        assert summary["event_counts"] == {"named_enter": 1, "session_start": 1}
        rows = [json.loads(line) for line in trace.with_suffix(".jsonl").read_text(encoding="utf-8").splitlines()]
        assert rows[1]["snapshot_strings"] == ["generic_effect_name"]
    print("native diagnostic trace decoder verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

