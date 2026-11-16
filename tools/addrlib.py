#!/usr/bin/env python3
"""Read an Address Library (versionlib-*.bin) database and disassemble functions by ID.

    python3 tools/addrlib.py offset 20539 19813            ID -> offset
    python3 tools/addrlib.py id 0x2CADA3                   offset -> containing ID (+delta)
    python3 tools/addrlib.py dis 20539 [bytes]             objdump the function starting at ID
"""
import bisect
import os
from pathlib import Path
import struct
import subprocess
import sys

GAME = Path(os.environ.get('SPCF_GAME', Path.home() / '.local/share/Steam/steamapps/common/SkyrimNext'))
DATABASE = Path(os.environ.get('SPCF_VERSIONLIB', GAME / 'Data/SKSE/Plugins/versionlib-1-6-640-0.bin'))
# tools/steamstub.py makes it
EXE = Path(os.environ.get('SPCF_EXE', Path(__file__).resolve().parents[1] / 'build/SkyrimSE.unpacked.exe'))
IMAGE_BASE = 0x140000000


def load(path=DATABASE):
    data = path.read_bytes()
    pos = 0

    def take(fmt):
        nonlocal pos
        value = struct.unpack_from(fmt, data, pos)
        pos += struct.calcsize(fmt)
        return value[0] if len(value) == 1 else value

    if take('<i') != 2:
        raise SystemExit(f'{path.name}: not an Address Library v2 database')
    take('<4i')
    name_length = take('<i')
    pos += name_length
    pointer_size, count = take('<i'), take('<i')
    ids, previous_id, previous_offset = {}, 0, 0
    for _ in range(count):
        kind = take('<B')
        low, high = kind & 0xF, kind >> 4
        id_ = {0: lambda: take('<Q'), 1: lambda: previous_id + 1, 2: lambda: previous_id + take('<B'),
               3: lambda: previous_id - take('<B'), 4: lambda: previous_id + take('<H'),
               5: lambda: previous_id - take('<H'), 6: lambda: take('<H'), 7: lambda: take('<I')}[low]()
        base = previous_offset // pointer_size if high & 8 else previous_offset
        offset = {0: lambda: take('<Q'), 1: lambda: base + 1, 2: lambda: base + take('<B'),
                  3: lambda: base - take('<B'), 4: lambda: base + take('<H'), 5: lambda: base - take('<H'),
                  6: lambda: take('<H'), 7: lambda: take('<I')}[high & 7]()
        if high & 8:
            offset *= pointer_size
        ids[id_] = offset
        previous_id, previous_offset = id_, offset
    return ids


def containing(ids, offset):
    by_offset = sorted((o, i) for i, o in ids.items())
    index = bisect.bisect_right([o for o, _ in by_offset], offset) - 1
    found, id_ = by_offset[index]
    return id_, offset - found


def disassemble(offset, length):
    start = IMAGE_BASE + offset
    return subprocess.run(['objdump', '-d', '-M', 'intel', '--no-show-raw-insn', f'--start-address={start:#x}',
                           f'--stop-address={start + length:#x}', str(EXE)],
                          capture_output=True, text=True, check=True).stdout


def main():
    command, *args = sys.argv[1:]
    ids = load()
    if command == 'offset':
        for a in args:
            print(f'{a}: {ids[int(a)]:#x}')
    elif command == 'id':
        for a in args:
            id_, delta = containing(ids, int(a, 16))
            print(f'{a}: {id_}+{delta:#x}')
    elif command == 'dis':
        print(disassemble(ids[int(args[0])], int(args[1], 0) if len(args) > 1 else 0x100))


if __name__ == '__main__':
    main()
