#!/usr/bin/env python3
"""Rewrite Base Object Swapper `|AlphaTest` swaps as disables.

Swapping a base to AlphaTest (00058E92, an invisible STAT) is a common way to hide an object, but
mist swapped that way leaves released references behind that the game later uses after their cell
is freed (see ../FINDINGS.md). This turns each such swap into BOS's usual way of removing an
object: keep the base, set the reference's initially-disabled flag (0x800) and move it 30000
units down, as `[Transforms]` entries:

    [Forms|LocTypeDungeon]               [Transforms|LocTypeDungeon]
    FXFog01|AlphaTest              ->    FXFog01|posR(0.0,0.0,-30000.0),flags(0x00000800)

    [References]                         [Transforms]
    0xbaf~Some.esp|AlphaTest       ->    0xbaf~Some.esp|posR(0.0,0.0,-30000.0),flags(0x00000800)

The original line is commented out in place and the new entries are appended at the end of the
file under the matching `[Transforms...]` section (BOS merges repeated sections, and the last entry
for a form wins). Only active lines of the form `<form>|AlphaTest` in `[Forms...]` and
`[References...]` sections are changed; everything else is copied byte for byte.

    python3 fix_alphatest.py                 SWAPS_IN/*.ini -> SWAPS_OUT/
    python3 fix_alphatest.py --no-move       only set the disabled flag
    python3 fix_alphatest.py IN_DIR OUT_DIR
"""
import argparse
from pathlib import Path
import re
import sys

HERE = Path(__file__).resolve().parent
DISABLED = 'flags(0x00000800)'
MOVE = 'posR(0.0,0.0,-30000.0)'

SECTION = re.compile(r'^\s*\[(?P<name>[^\]]+)\]\s*$')
ALPHATEST = re.compile(r'^\s*(?P<key>[^;#|\s][^|]*?)\s*\|\s*AlphaTest\s*$', re.IGNORECASE)
TRANSFORM_KEY = re.compile(r'^\s*(?P<key>[^;#|\s][^|]*?)\s*\|')


def target_section(section):
    """[Forms|cond] / [References|cond] -> [Transforms|cond]; None for other sections."""
    kind, bar, conditions = section.partition('|')
    if kind.strip() not in ('Forms', 'References'):
        return None
    return 'Transforms' + (bar + conditions if bar else '')


def fix(text, properties):
    """(new text, number of swaps rewritten, warnings)."""
    lines = text.splitlines(keepends=True)
    newline = '\r\n' if '\r\n' in text else '\n'
    section = None
    added = {}  # target section -> keys, in order
    existing = {}  # Transforms section -> keys already in the file
    warnings = []
    out = []
    for number, line in enumerate(lines, 1):
        body = line.rstrip('\r\n').lstrip('\ufeff')
        if match := SECTION.match(body):
            section = match['name'].strip()
            out.append(line)
            continue
        if section and section.partition('|')[0].strip() in ('Transforms', 'Properties'):
            if match := TRANSFORM_KEY.match(body):
                existing.setdefault(section.replace('Properties', 'Transforms', 1), set()).add(match['key'].lower())
        match = ALPHATEST.match(body)
        if not match or section is None:
            out.append(line)
            continue
        target = target_section(section)
        if target is None:
            warnings.append(f'line {number}: AlphaTest in [{section}], left as is')
            out.append(line)
            continue
        # [Forms] accepts comma lists of bases; [Transforms] takes one form per line.
        keys = [key.strip() for key in match['key'].split(',') if key.strip()]
        bucket = added.setdefault(target, [])
        bucket.extend(key for key in keys if key not in bucket)
        ending = line[len(line.rstrip('\r\n')):]
        out.append(f';{body.strip()}  ; replaced by [{target}] below{ending or newline}')

    if not added:
        return text, 0, warnings
    for target, keys in added.items():
        for key in keys:
            if key.lower() in existing.get(target, set()):
                warnings.append(f'{key} already has a [{target}] entry; the one added at the end wins')
    if out and not out[-1].endswith(('\n', '\r')):
        out[-1] += newline
    out.append(newline)
    out.append(f'; Added by fix_alphatest.py: the AlphaTest swaps commented out above, as disables.{newline}')
    for target, keys in added.items():
        out.append(f'[{target}]{newline}')
        out.extend(f'{key}|{properties}{newline}' for key in keys)
        out.append(newline)
    return ''.join(out), sum(len(keys) for keys in added.values()), warnings


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('input', nargs='?', type=Path, default=HERE / 'SWAPS_IN')
    parser.add_argument('output', nargs='?', type=Path, default=HERE / 'SWAPS_OUT')
    parser.add_argument('--no-move', action='store_true', help='only set the disabled flag, do not move down')
    args = parser.parse_args()

    files = sorted(p for p in args.input.glob('*.ini') if p.is_file())
    if not files:
        sys.exit(f'no .ini files in {args.input}')
    if args.input.resolve() == args.output.resolve():
        sys.exit('input and output must be different folders')
    args.output.mkdir(parents=True, exist_ok=True)
    properties = DISABLED if args.no_move else f'{MOVE},{DISABLED}'

    total = 0
    for path in files:
        raw = path.read_bytes()
        text = raw.decode('utf-8', errors='surrogateescape')
        fixed, count, warnings = fix(text, properties)
        (args.output / path.name).write_bytes(fixed.encode('utf-8', errors='surrogateescape') if count else raw)
        total += count
        print(f'{path.name}: {count} AlphaTest swap(s) rewritten' if count else f'{path.name}: unchanged')
        for warning in warnings:
            print(f'  warning: {warning}')
    print(f'{total} swap(s) rewritten in {len(files)} file(s); written to {args.output}')


if __name__ == '__main__':
    main()
