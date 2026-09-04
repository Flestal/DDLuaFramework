#!/usr/bin/env python3
"""Verify DDLua internal bridge RVAs against a Darkest.exe image."""

from __future__ import annotations

import argparse
import configparser
import hashlib
import struct
from pathlib import Path


def pe_sections(image: bytes) -> list[tuple[int, int, int]]:
    pe_offset = struct.unpack_from("<I", image, 0x3C)[0]
    if image[pe_offset : pe_offset + 4] != b"PE\0\0":
        raise ValueError("not a PE image")
    section_count = struct.unpack_from("<H", image, pe_offset + 6)[0]
    optional_size = struct.unpack_from("<H", image, pe_offset + 20)[0]
    section_table = pe_offset + 24 + optional_size
    result: list[tuple[int, int, int]] = []
    for index in range(section_count):
        offset = section_table + index * 40
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from(
            "<IIII", image, offset + 8
        )
        result.append((virtual_address, max(virtual_size, raw_size), raw_offset))
    return result


def rva_to_raw(rva: int, sections: list[tuple[int, int, int]]) -> int:
    for virtual_address, size, raw_offset in sections:
        if virtual_address <= rva < virtual_address + size:
            return raw_offset + rva - virtual_address
    raise ValueError(f"RVA 0x{rva:X} is outside all PE sections")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("executable", type=Path)
    parser.add_argument("profile", type=Path)
    parser.add_argument("build_id")
    args = parser.parse_args()

    image = args.executable.read_bytes()
    sections = pe_sections(image)
    config = configparser.ConfigParser()
    config.read(args.profile, encoding="utf-8")
    if args.build_id not in config:
        raise SystemExit(f"missing profile [{args.build_id}]")

    section = config[args.build_id]
    required = {
        "named_effect_dispatch_rva",
        "named_effect_dispatch_bytes",
        "resolved_effect_apply_rva",
        "resolved_effect_apply_bytes",
        "actor_buff_begin_offset",
        "actor_buff_end_offset",
        "buff_entry_size",
        "buff_stat_type_offset",
        "buff_amount_offset",
    }
    missing = sorted(required.difference(section))
    if missing:
        raise SystemExit("missing required bridge keys: " + ", ".join(missing))
    if not any(key.startswith("stat_") for key in section):
        raise SystemExit("profile has no symbolic stat mapping")
    if not any(key.endswith("_dot_tick_rva") for key in section):
        raise SystemExit("profile has no symbolic DoT primitive")

    failed = False
    print(f"sha256={hashlib.sha256(image).hexdigest().upper()}")
    for key, value in section.items():
        if not key.endswith("_rva"):
            continue
        name = key[: -len("_rva")]
        expected = bytes.fromhex(section[f"{name}_bytes"])
        rva = int(value, 0)
        raw = rva_to_raw(rva, sections)
        actual = image[raw : raw + len(expected)]
        matched = actual == expected
        failed |= not matched
        print(f"{name}=0x{rva:08X} {'OK' if matched else 'MISMATCH'}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
