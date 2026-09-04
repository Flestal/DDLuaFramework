#!/usr/bin/env python3
"""Decode DDLua's fixed-size, policy-free native diagnostic trace."""

from __future__ import annotations

import argparse
import json
import struct
from collections import Counter
from pathlib import Path


MAGIC = 0x52544444
VERSION = 1
RECORD_SIZE = 640
HEADER = struct.Struct("<IHHIIIIIqQ")
U64_10 = struct.Struct("<10Q")
COUNTS = struct.Struct("<HH")
U64_32 = struct.Struct("<32Q")
EVENTS = {
    1: "session_start",
    10: "named_enter",
    11: "named_exit",
    20: "apply_enter",
    21: "apply_pass",
    22: "apply_cancel",
    30: "buff_query",
    31: "dot_request",
    32: "action_end",
}


def strings(data: bytes) -> list[str]:
    found: list[str] = []
    current = bytearray()
    for value in data + b"\0":
        if 0x20 <= value <= 0x7E:
            current.append(value)
        else:
            if len(current) >= 4:
                found.append(current.decode("ascii"))
            current.clear()
    for offset in (0, 1):
        current_chars: list[str] = []
        view = data[offset:]
        for index in range(0, len(view) - 1, 2):
            code = int.from_bytes(view[index : index + 2], "little")
            if 0x20 <= code <= 0x7E:
                current_chars.append(chr(code))
            else:
                if len(current_chars) >= 4:
                    found.append("".join(current_chars))
                current_chars.clear()
    return list(dict.fromkeys(found))


def rva(address: int, base: int, size: int) -> str | None:
    if base <= address < base + size:
        return f"0x{address - base:X}"
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path)
    parser.add_argument("--jsonl", type=Path)
    parser.add_argument("--summary", type=Path)
    args = parser.parse_args()

    content = args.trace.read_bytes()
    if len(content) % RECORD_SIZE:
        raise SystemExit(
            f"truncated trace: {len(content)} bytes is not a multiple of {RECORD_SIZE}"
        )

    records: list[dict[str, object]] = []
    counts: Counter[str] = Counter()
    base = 0
    image_size = 0
    for offset in range(0, len(content), RECORD_SIZE):
        block = content[offset : offset + RECORD_SIZE]
        (
            magic,
            version,
            event_id,
            record_size,
            process_id,
            thread_id,
            depth,
            flags,
            qpc,
            return_address,
        ) = HEADER.unpack_from(block, 0)
        if magic != MAGIC or version != VERSION or record_size != RECORD_SIZE:
            raise SystemExit(f"invalid record at byte offset {offset}")
        arguments = list(U64_10.unpack_from(block, HEADER.size))
        count_offset = HEADER.size + U64_10.size
        stack_count, snapshot_size = COUNTS.unpack_from(block, count_offset)
        stack_offset = count_offset + COUNTS.size
        stack = list(U64_32.unpack_from(block, stack_offset))[:stack_count]
        snapshot = block[stack_offset + U64_32.size :][:snapshot_size]
        event = EVENTS.get(event_id, f"unknown_{event_id}")
        if event_id == 1:
            base = arguments[0]
            image_size = arguments[2]
        record = {
            "index": len(records),
            "event": event,
            "event_id": event_id,
            "pid": process_id,
            "tid": thread_id,
            "depth": depth,
            "flags": flags,
            "qpc": qpc,
            "return_address": f"0x{return_address:X}",
            "return_rva": rva(return_address, base, image_size),
            "arguments": [f"0x{value:X}" for value in arguments],
            "stack": [f"0x{value:X}" for value in stack],
            "stack_rvas": [value for address in stack if (value := rva(address, base, image_size))],
            "snapshot_hex": snapshot.hex(),
            "snapshot_strings": strings(snapshot),
        }
        records.append(record)
        counts[event] += 1

    output = args.jsonl or args.trace.with_suffix(".jsonl")
    with output.open("w", encoding="utf-8") as stream:
        for record in records:
            stream.write(json.dumps(record, ensure_ascii=False) + "\n")

    summary = {
        "trace": str(args.trace.resolve()),
        "record_size": RECORD_SIZE,
        "record_count": len(records),
        "main_module_base": f"0x{base:X}",
        "main_module_size": image_size,
        "event_counts": dict(sorted(counts.items())),
        "jsonl": str(output.resolve()),
    }
    summary_path = args.summary or args.trace.with_suffix(".summary.json")
    summary_path.write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
