#!/usr/bin/env python3
"""Decode the opt-in DDLua corridor-return diagnostic trace."""

from __future__ import annotations

import argparse
import json
import struct
from collections import Counter
from pathlib import Path


MAGIC = 0x43524444
VERSION = 1
RECORD_SIZE = 640
HEADER = struct.Struct("<IHHIIIIIqQ")
ARGS = struct.Struct("<8Q")
VALUES = struct.Struct("<fffIIiI")
U32_32 = struct.Struct("<32I")
EVENTS = {
    1: "session_start",
    10: "move_enter",
    11: "move_exit",
    20: "return_roll",
    29: "hero_actor",
    30: "hero_buff",
}
CONTENT = {
    0: "nothing",
    1: "battle",
    2: "ambush",
    3: "trap",
    4: "obstacle",
    5: "happening",
    6: "guarded_curio",
    7: "curio",
    8: "hunger",
    9: "treasure",
    10: "guarded_treasure",
    11: "ambush_curio",
    12: "ambush_treasure",
    13: "hidden_door",
    14: "prisoner",
}


def hex_or_zero(value: int) -> str:
    return f"0x{value:X}" if value else "0x0"


def visible_snapshot(data: bytes) -> dict[str, object]:
    nul = data.find(b"\0")
    candidate = data if nul < 0 else data[:nul]
    text = ""
    if candidate and all(0x20 <= byte <= 0x7E for byte in candidate):
        text = candidate.decode("ascii")
    return {"hex": data.hex(), "ascii": text}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path)
    parser.add_argument("--jsonl", type=Path)
    parser.add_argument("--summary", type=Path)
    args = parser.parse_args()

    raw = args.trace.read_bytes()
    if len(raw) % RECORD_SIZE:
        raise SystemExit("trace is truncated or has the wrong record size")

    rows: list[dict[str, object]] = []
    counts: Counter[str] = Counter()
    for offset in range(0, len(raw), RECORD_SIZE):
        block = raw[offset : offset + RECORD_SIZE]
        (
            magic,
            version,
            event_id,
            record_size,
            pid,
            tid,
            depth,
            flags,
            qpc,
            sequence,
        ) = HEADER.unpack_from(block, 0)
        if magic != MAGIC or version != VERSION or record_size != RECORD_SIZE:
            raise SystemExit(f"invalid record at byte offset {offset}")
        cursor = HEADER.size
        arguments = list(ARGS.unpack_from(block, cursor))
        cursor += ARGS.size
        (
            chance,
            torchlight,
            alternate_torchlight,
            area_kind,
            tile_count,
            selected_tile,
            _reserved,
        ) = VALUES.unpack_from(block, cursor)
        cursor += VALUES.size
        before = list(U32_32.unpack_from(block, cursor))
        cursor += U32_32.size
        after = list(U32_32.unpack_from(block, cursor))
        cursor += U32_32.size
        knowledge = list(U32_32.unpack_from(block, cursor))
        cursor += U32_32.size
        snapshot_size = struct.unpack_from("<H", block, cursor)[0]
        cursor += 2
        if snapshot_size > RECORD_SIZE - cursor:
            raise SystemExit(f"invalid snapshot size at record {len(rows)}")
        event = EVENTS.get(event_id, f"unknown_{event_id}")
        count = min(tile_count, 32)
        row: dict[str, object] = {
            "index": len(rows),
            "event": event,
            "pid": pid,
            "tid": tid,
            "depth": depth,
            "flags": f"0x{flags:X}",
            "qpc": qpc,
            "sequence": sequence,
            "arguments": [hex_or_zero(value) for value in arguments],
        }
        if event == "return_roll":
            content_value = arguments[2]
            row.update(
                {
                    "content": CONTENT.get(content_value, f"unknown_{content_value}"),
                    "content_value": content_value,
                    "chance": chance,
                    "torchlight": torchlight,
                    "alternate_torchlight": alternate_torchlight,
                    "alternate_torchlight_bits": f"0x{struct.unpack('<I', struct.pack('<f', alternate_torchlight))[0]:08X}",
                    "area_kind": area_kind,
                    "area_reversed": bool(flags & 0x40),
                    "tile_count": tile_count,
                    "selected_tile": selected_tile,
                    "placed": bool(flags & 0x02),
                    "before_content": before[:count],
                    "after_content": after[:count],
                    "tile_knowledge": knowledge[:count],
                }
            )
        elif event == "hero_actor":
            row.update({"hero_index": arguments[2], "buff_count": arguments[3]})
        elif event == "hero_buff":
            row.update(
                {
                    "hero_index": arguments[2],
                    "buff_index": arguments[3],
                    "stat_type": f"0x{arguments[4]:X}",
                    "amount": chance,
                }
            )
        snapshot = block[cursor : cursor + snapshot_size]
        if snapshot:
            row["snapshot"] = visible_snapshot(snapshot)
        rows.append(row)
        counts[event] += 1

    jsonl = args.jsonl or args.trace.with_suffix(".jsonl")
    with jsonl.open("w", encoding="utf-8") as stream:
        for row in rows:
            stream.write(json.dumps(row, ensure_ascii=False) + "\n")
    summary_path = args.summary or args.trace.with_suffix(".summary.json")
    summary = {
        "trace": str(args.trace.resolve()),
        "record_size": RECORD_SIZE,
        "record_count": len(rows),
        "event_counts": dict(sorted(counts.items())),
        "jsonl": str(jsonl.resolve()),
    }
    summary_path.write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
