import struct
import sys
from pathlib import Path


def read_u16(data, offset):
    return struct.unpack_from("<H", data, offset)[0]


def read_u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def main():
    if len(sys.argv) < 3:
        raise SystemExit("usage: pe_function_bounds.py <pe> <rva> [rva ...]")
    data = Path(sys.argv[1]).read_bytes()
    pe = read_u32(data, 0x3C)
    section_count = read_u16(data, pe + 6)
    optional_size = read_u16(data, pe + 20)
    section_table = pe + 24 + optional_size
    sections = []
    for index in range(section_count):
        offset = section_table + index * 40
        name = data[offset:offset + 8].split(b"\0", 1)[0].decode("ascii", "replace")
        sections.append((name, read_u32(data, offset + 12), read_u32(data, offset + 16), read_u32(data, offset + 20)))
    pdata = next(section for section in sections if section[0] == ".pdata")
    entries = []
    for offset in range(pdata[3], pdata[3] + pdata[2] - 11, 12):
        begin, end, unwind = struct.unpack_from("<III", data, offset)
        if begin and end > begin:
            entries.append((begin, end, unwind))
    for raw in sys.argv[2:]:
        address = int(raw, 0)
        matches = [entry for entry in entries if entry[0] <= address < entry[1]]
        if matches:
            begin, end, unwind = min(matches, key=lambda entry: entry[1] - entry[0])
            print(f"{raw}: begin=0x{begin:X} end=0x{end:X} size=0x{end-begin:X} unwind=0x{unwind:X}")
        else:
            print(f"{raw}: no pdata entry")


if __name__ == "__main__":
    main()
