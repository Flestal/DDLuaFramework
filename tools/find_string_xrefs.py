import json
import struct
import sys
from pathlib import Path


def u16(data, off):
    return struct.unpack_from("<H", data, off)[0]


def u32(data, off):
    return struct.unpack_from("<I", data, off)[0]


def i32(data, off):
    return struct.unpack_from("<i", data, off)[0]


def u64(data, off):
    return struct.unpack_from("<Q", data, off)[0]


def main():
    if len(sys.argv) < 3:
        raise SystemExit("usage: find_string_xrefs.py <pe> <string> [string ...]")

    path = Path(sys.argv[1])
    data = path.read_bytes()
    pe = u32(data, 0x3C)
    if data[pe:pe + 4] != b"PE\0\0":
        raise SystemExit("not a PE file")

    section_count = u16(data, pe + 6)
    optional_size = u16(data, pe + 20)
    optional = pe + 24
    if u16(data, optional) != 0x20B:
        raise SystemExit("expected PE32+")
    image_base = u64(data, optional + 24)
    section_table = optional + optional_size
    sections = []
    for index in range(section_count):
        off = section_table + index * 40
        name = data[off:off + 8].split(b"\0", 1)[0].decode("ascii", "replace")
        virtual_size = u32(data, off + 8)
        virtual_address = u32(data, off + 12)
        raw_size = u32(data, off + 16)
        raw_offset = u32(data, off + 20)
        sections.append({
            "name": name,
            "rva": virtual_address,
            "virtual_size": virtual_size,
            "raw_size": raw_size,
            "raw_offset": raw_offset,
        })

    def file_to_rva(file_offset):
        for section in sections:
            start = section["raw_offset"]
            end = start + section["raw_size"]
            if start <= file_offset < end:
                return section["rva"] + file_offset - start
        return None

    text = next(section for section in sections if section["name"] == ".text")
    text_bytes = data[text["raw_offset"]:text["raw_offset"] + text["raw_size"]]
    results = []
    for requested in sys.argv[2:]:
        needle = requested.encode("utf-8")
        positions = []
        cursor = 0
        while True:
            found = data.find(needle, cursor)
            if found < 0:
                break
            positions.append(found)
            cursor = found + 1

        for file_offset in positions:
            target_rva = file_to_rva(file_offset)
            if target_rva is None:
                continue
            xrefs = []
            for i in range(len(text_bytes) - 7):
                # x64 RIP-relative MOV/LEA with a REX prefix.
                if 0x40 <= text_bytes[i] <= 0x4F and text_bytes[i + 1] in (0x8B, 0x8D) and text_bytes[i + 2] & 0xC7 == 0x05:
                    resolved = text["rva"] + i + 7 + i32(text_bytes, i + 3)
                    if resolved == target_rva:
                        xrefs.append(text["rva"] + i)
                # Same addressing form without a REX prefix.
                if text_bytes[i] in (0x8B, 0x8D) and text_bytes[i + 1] & 0xC7 == 0x05:
                    resolved = text["rva"] + i + 6 + i32(text_bytes, i + 2)
                    if resolved == target_rva:
                        xrefs.append(text["rva"] + i)
            results.append({
                "string": requested,
                "string_rva": f"0x{target_rva:X}",
                "string_va": f"0x{image_base + target_rva:X}",
                "xrefs_rva": [f"0x{x:X}" for x in sorted(set(xrefs))],
                "xrefs_va": [f"0x{image_base + x:X}" for x in sorted(set(xrefs))],
            })

    print(json.dumps({"image_base": f"0x{image_base:X}", "results": results}, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
