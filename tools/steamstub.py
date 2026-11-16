#!/usr/bin/env python3
"""Decrypt the SteamStub (variant 3.1, x64) wrapped .text of SkyrimSE.exe into build/, for objdump.

    python3 tools/steamstub.py [SkyrimSE.exe] [output]

Same steps as Steamless: XOR-decode the stub header in front of the entry point, AES-ECB decrypt
its IV, then AES-256-CBC decrypt the stolen bytes plus the first code section (via openssl).
"""
from pathlib import Path
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import addrlib  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
HEADER_SIZE = 0xF0
SIGNATURE = 0xC0DEC0DF


def sections(data):
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    count = struct.unpack_from('<H', data, pe + 6)[0]
    optional_size = struct.unpack_from('<H', data, pe + 20)[0]
    entry = struct.unpack_from('<I', data, pe + 24 + 16)[0]
    table = pe + 24 + optional_size
    result = []
    for i in range(count):
        name, vsize, va, raw_size, raw = struct.unpack_from('<8sIIII', data, table + 40 * i)
        result.append((name.rstrip(b'\0').decode(), vsize, va, raw_size, raw))
    return entry, result


def file_offset(sections_, rva):
    for _, vsize, va, raw_size, raw in sections_:
        if va <= rva < va + max(vsize, raw_size):
            return raw + rva - va
    raise ValueError(f'RVA {rva:#x} outside sections')


def xor_decode(block):
    out, key = bytearray(block[:4]), struct.unpack_from('<I', block, 0)[0]
    for i in range(4, len(block), 4):
        value = struct.unpack_from('<I', block, i)[0]
        out += struct.pack('<I', value ^ key)
        key = value
    return bytes(out)


def openssl(mode, key, data, iv=None):
    args = ['openssl', 'enc', '-d', f'-aes-256-{mode}', '-nopad', '-K', key.hex()]
    if iv is not None:
        args += ['-iv', iv.hex()]
    return subprocess.run(args, input=data, capture_output=True, check=True).stdout


def unpack(source, target):
    data = bytearray(source.read_bytes())
    entry, table = sections(data)
    start = file_offset(table, entry) - HEADER_SIZE
    header = xor_decode(bytes(data[start:start + HEADER_SIZE]))
    if struct.unpack_from('<I', header, 4)[0] != SIGNATURE:
        raise SystemExit(f'{source.name}: no SteamStub 3.1 header in front of the entry point')
    code_va = struct.unpack_from('<Q', header, 0x48)[0]
    aes_key, aes_iv, stolen = header[0x58:0x78], header[0x78:0x88], header[0x88:0x98]
    _, _, va, raw_size, raw = next(s for s in table if s[2] == code_va)
    iv = openssl('ecb', aes_key, aes_iv)
    plain = openssl('cbc', aes_key, stolen + bytes(data[raw:raw + raw_size]), iv)
    data[raw:raw + raw_size] = plain[:raw_size]
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    print(f'Decrypted .text ({raw_size:#x} bytes at RVA {va:#x}) into {target}')


if __name__ == '__main__':
    args = sys.argv[1:]
    unpack(Path(args[0]) if args else addrlib.GAME / 'SkyrimSE.exe',
           Path(args[1]) if len(args) > 1 else ROOT / 'build/SkyrimSE.unpacked.exe')
