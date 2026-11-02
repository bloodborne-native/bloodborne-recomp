#!/usr/bin/env python3
"""tools/recomp/modules.py GAME_DIR ELF SCAN_DIR OUT_ELF: the game's modules (sce_module/libc.prx and
libSceFios2.prx) added to the recompiler's view of the image, at the offsets the game links them to
(src/runtime_prepare.c, bb_boot_link). Writes OUT_ELF, with their segments, and adds their functions,
calls and import stubs to SCAN_DIR's tables. Prints the offsets to generate.

Code that jumps between stack frames cannot be C (a longjmp cannot come back into a C function that
went on), so these stay with the translator, with their callers: docs/RECOMPILATION.md."""
import os
import pathlib
import re
import struct
import subprocess
import sys
import tempfile

import ps4elf

HERE = pathlib.Path(__file__).resolve().parent
MODULES = ('libc.prx', 'libSceFios2.prx')
PHDR = struct.Struct('<IIQQQQQQ')
PT_LOAD = 1
PT_SCE_RELRO = 0x61000010
STACK_JUMPERS = re.compile(r'^_*(sig)?(set|long)jmp|^_*(get|set|make|swap)context$|^_*Unwind|^__cxa_(throw|rethrow)$')


def phdrs(image):
    offset, = struct.unpack_from('<Q', image, 32)
    count, = struct.unpack_from('<H', image, 56)
    return [PHDR.unpack_from(image, offset + i * PHDR.size) for i in range(count)]


def rows(path):
    return [line.split('\t') for line in path.read_text().splitlines()[1:] if line.strip()]


def add_rows(path, rows_):
    with open(path, 'a') as f:
        f.writelines('\t'.join(row) + '\n' for row in rows_)


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    game, elf, scan_dir, out_elf = (pathlib.Path(a) for a in sys.argv[1:])
    scan = pathlib.Path(os.environ['BB_SCAN'])
    known = {}
    for name, label in re.findall(r'\{"([^"]+)","([^"]+)"\}', (HERE / 'import_names.inc').read_text()):
        known[name.split('#')[0]] = label
    image = bytearray(elf.read_bytes())
    headers = phdrs(image)
    end = max(h[3] + h[6] for h in headers if h[0] in (PT_LOAD, PT_SCE_RELRO))
    base = (end + 65535) & ~65535
    added, jumpers, stubs = [], set(), set()
    for module in MODULES:
        path = game / 'sce_module' / module
        with tempfile.TemporaryDirectory() as work:
            work = pathlib.Path(work)
            subprocess.run([sys.executable, HERE / 'ps4elf.py', path, work / 'module.elf'], check=True, capture_output=True)
            subprocess.run([sys.executable, HERE / 'pointers.py', path, work / 'pointers.txt'], check=True, capture_output=True)
            subprocess.run([scan, work / 'module.elf', work, work / 'pointers.txt'], check=True, capture_output=True)
            subprocess.run([sys.executable, HERE / 'imports.py', path, work], check=True, capture_output=True)
            blob = (work / 'module.elf').read_bytes()
            moved = lambda text: f'{int(text, 16) + base:#x}'
            functions = [[moved(row[0]), *row[1:]] for row in rows(work / 'functions.tsv')]
            add_rows(scan_dir / 'functions.tsv', functions)
            add_rows(scan_dir / 'calls.tsv', [[moved(a), moved(b)] for a, b, *_ in rows(work / 'calls.tsv')])
            imports = [[moved(a), name] for a, name in rows(work / 'imports.tsv')]
            add_rows(scan_dir / 'imports.tsv', imports)
        added += [row[0] for row in functions]
        stubs |= {row[0] for row in imports}
        for symbol in ps4elf.module(path)['symbols']:
            name = known.get(symbol['name'].split('#')[0], symbol['name'])
            if symbol['value'] and STACK_JUMPERS.search(name):
                jumpers.add(f"{symbol['value'] + base:#x}")
        shift = len(image)
        image += blob
        size = 0
        for header in phdrs(blob):
            if header[0] in (PT_LOAD, PT_SCE_RELRO):
                size = max(size, header[3] + header[6])
            if header[0] == PT_LOAD:
                loaded = list(header)
                loaded[2] += shift
                loaded[3] += base
                loaded[4] += base
                headers.append(tuple(loaded))
        base = (base + size + 65535) & ~65535
    callers = {row[0] for row in rows(scan_dir / 'calls.tsv') if row[1] in jumpers}
    table = len(image)
    for header in headers:
        image += PHDR.pack(*header)
    struct.pack_into('<Q', image, 32, table)
    struct.pack_into('<H', image, 56, len(headers))
    out_elf.write_bytes(image)
    print('\n'.join(a for a in added if a not in jumpers | callers))


main()
