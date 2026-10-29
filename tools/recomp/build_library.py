#!/usr/bin/env python3
"""tools/recomp/build_library.py: the recompiled library (librecomp.dylib) from a player's own game,
in one run that a launcher can follow and resume (docs/RECOMPILATION.md, "Building the library").

  build_library.py --game GAME_DIR --work WORK_DIR --toolchain LLVM_DIR [--jobs N] [--keep]
  build_library.py --version     the recompiler version this kit builds with (a library records it)

GAME_DIR holds eboot.bin (1.09). LLVM_DIR is an LLVM release (bin/clang, bin/ld64.lld,
lib/clang/<major>/include): no Xcode and no macOS SDK are used. WORK_DIR receives
librecomp.dylib and library.json (what it was built from); the generated C and the objects stay in
WORK_DIR/build until the library is linked (--keep keeps them), so an interrupted run compiles
only the files left. The generated C is the game's code: it never leaves the player's machine.

Progress: one JSON object a line on stdout ({"event": "step", "step": ...}, {"event": "compile",
"done": n, "total": m}, {"event": "done", ...}, or {"event": "error", "message": ...} and exit 1).

Runs from a checkout (out/recomp/scan and bbrecomp from ./build.sh, src/ and deps/include) or from
an app's kit (KIT/bin, KIT/include, KIT/sdkless, KIT/annotations, KIT/tools with this script,
KIT/VERSION)."""
import argparse
import concurrent.futures
import datetime
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
FORMAT = 1
# What the library may leave to the game process: the C library's memory functions and the
# compiler's 128-bit division helpers, all in libSystem, which the game has loaded.
ALLOWED_UNDEFINED = ['_memcpy', '_memset', '_memmove', '___divti3', '___udivti3', '___modti3', '___umodti3']
# The generator's flags (recomp.sh), without an SDK: clang's own headers, stand-ins for the four the
# runtime's headers include, and built-ins kept on (-ffreestanding turns them off: 32% more code,
# frames 60% slower).
CFLAGS = ['--target=arm64-apple-macos15', '-O2', '-std=gnu11', '-ffp-contract=off', '-fno-strict-aliasing',
          '-fvisibility=hidden', '-fno-stack-protector', '-nostdinc', '-w']


def emit(event, **fields):
    print(json.dumps({'event': event, **fields}), flush=True)


def fail(message):
    emit('error', message=message)
    sys.exit(1)


class Kit:
    """Where the tools and headers are: an app's kit, or this checkout."""

    def __init__(self):
        kit = HERE.parent
        if (kit / 'bin' / 'bbrecomp').exists():
            self.bin, self.include, self.sdkless = kit / 'bin', [kit / 'include'], kit / 'sdkless'
            self.annotations = kit / 'annotations' / 'functions.tsv'
            self.version = (kit / 'VERSION').read_text().strip()
        else:
            repo = HERE.parent.parent
            self.bin, self.include, self.sdkless = repo / 'out' / 'recomp', [repo / 'src', repo / 'deps' / 'include'], HERE / 'sdkless'
            self.annotations = repo / 'annotations' / 'functions.tsv'
            try:
                self.version = subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip()
            except (OSError, subprocess.CalledProcessError):
                self.version = 'unknown'
        for tool in ('scan', 'bbrecomp'):
            if not (self.bin / tool).exists():
                fail(f'{self.bin / tool} is missing (./build.sh builds it).')
        if not self.annotations.exists():
            fail(f'{self.annotations} is missing.')


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


def run(command, step, **kwargs):
    result = subprocess.run([str(c) for c in command], capture_output=True, text=True, **kwargs)
    if result.returncode:
        tail = (result.stderr or result.stdout).strip().splitlines()[-5:]
        fail(f'{step} failed: ' + ' | '.join(tail))
    return result


def toolchain(root):
    clang, lld = root / 'bin' / 'clang', root / 'bin' / 'ld64.lld'
    resource = sorted((root / 'lib' / 'clang').glob('*/include'))
    if not clang.exists() or not lld.exists() or not resource:
        fail(f'{root} is not an LLVM toolchain (bin/clang, bin/ld64.lld, lib/clang/<version>/include).')
    return clang, resource[-1]


def prepare(kit, game, build, key):
    """The executable, the function list and the generated C, unless the last run made them from
    the same executable and recompiler."""
    stamp = build / 'prepared.json'
    if stamp.exists() and json.loads(stamp.read_text()) == key and any(build.glob('gen/recomp*.c')):
        return
    for sub in ('scan', 'gen', 'obj'):
        shutil.rmtree(build / sub, ignore_errors=True)
        (build / sub).mkdir(parents=True)
    py = [sys.executable]
    tools = HERE
    emit('step', step='extract')
    run(py + [tools / 'ps4elf.py', game / 'eboot.bin', build / 'eboot.elf'], 'Reading the executable')
    run(py + [tools / 'pointers.py', game / 'eboot.bin', build / 'scan' / 'pointers.txt'], 'Finding code pointers')
    emit('step', step='scan')
    run([kit.bin / 'scan', build / 'eboot.elf', build / 'scan', build / 'scan' / 'pointers.txt'], 'Finding functions')
    run(py + [tools / 'imports.py', game / 'eboot.bin', build / 'scan'], 'Naming imports')
    functions = build / 'scan' / 'functions.tsv'
    offsets = [line.split('\t', 1)[0] for line in functions.read_text().splitlines()[1:] if line.strip()]
    (build / 'scan' / 'all.txt').write_text('\n'.join(offsets) + '\n')
    emit('step', step='generate', functions=len(offsets))
    run([kit.bin / 'bbrecomp', build / 'eboot.elf', functions, build / 'gen' / 'recomp.c', '@' + str(build / 'scan' / 'all.txt')],
        'Generating C', env={**os.environ, 'BB_JIT': '0', 'BB_RECOMP_ANNOTATIONS': str(kit.annotations)})
    stamp.write_text(json.dumps(key))


def compile_all(kit, clang, resource, build, jobs):
    sources = sorted(build.glob('gen/recomp*.c'))
    obj = build / 'obj'
    for leftover in obj.glob('*.tmp'):  # from an interrupted compile
        leftover.unlink()
    for leftover in obj.glob('*.o-*'):
        leftover.unlink()
    target = {source: obj / (source.name + '.o') for source in sources}
    todo = [s for s in sources if not (target[s].exists() and target[s].stat().st_mtime >= s.stat().st_mtime)]
    done = len(sources) - len(todo)
    emit('compile', done=done, total=len(sources))
    flags = CFLAGS + ['-isystem', resource, '-isystem', kit.sdkless] + [f'-I{p}' for p in kit.include]

    def one(source):
        partial = target[source].with_suffix('.o-partial')
        result = subprocess.run([str(clang), *map(str, flags), '-c', str(source), '-o', str(partial)], capture_output=True, text=True)
        if result.returncode:
            return source, result.stderr.strip().splitlines()[-3:]
        partial.rename(target[source])
        return source, None

    # Counted as each file finishes: pool.map gives results in submission order, so one large file
    # held the count back while the files after it were done (the launcher showed 9 of 383 with 43
    # compiled).
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for future in concurrent.futures.as_completed([pool.submit(one, s) for s in todo]):
            source, error = future.result()
            if error:
                fail(f'Compiling {source.name} failed: ' + ' | '.join(error))
            done += 1
            emit('compile', done=done, total=len(sources))
    return [target[s] for s in sources]


def link(clang, objects, out):
    emit('step', step='link')
    partial = out.with_suffix('.dylib-partial')
    allowed = [arg for symbol in ALLOWED_UNDEFINED for arg in ('-Wl,-U,' + symbol,)]
    run([clang, '--target=arm64-apple-macos15', '-dynamiclib', '-fuse-ld=lld', '-nostdlib', '-Wl,-undefined,error',
         *allowed, '-install_name', '@rpath/librecomp.dylib', *objects, '-o', partial], 'Linking')
    partial.rename(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--version', action='store_true')
    parser.add_argument('--game', type=pathlib.Path)
    parser.add_argument('--work', type=pathlib.Path)
    parser.add_argument('--toolchain', type=pathlib.Path)
    parser.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) - 2))
    parser.add_argument('--keep', action='store_true', help='keep the generated C and objects')
    args = parser.parse_args()
    started = time.monotonic()
    kit = Kit()
    if args.version:
        print(kit.version)
        return
    if not (args.game and args.work and args.toolchain):
        parser.error('--game, --work and --toolchain are required')
    if not (args.game / 'eboot.bin').exists():
        fail(f'No eboot.bin in {args.game}.')
    clang, resource = toolchain(args.toolchain)
    build = args.work / 'build'
    build.mkdir(parents=True, exist_ok=True)
    emit('step', step='check')
    key = {'format': FORMAT, 'recomp': kit.version, 'eboot_sha256': sha256(args.game / 'eboot.bin'),
           'annotations_sha256': sha256(kit.annotations)}
    prepare(kit, args.game, build, key)
    objects = compile_all(kit, clang, resource, build, args.jobs)
    library = args.work / 'librecomp.dylib'
    link(clang, objects, library)
    version = run([clang, '--version'], 'Reading the compiler version').stdout.splitlines()[0]
    manifest = {**key, 'functions': len((build / 'scan' / 'all.txt').read_text().split()),
                'compiler': version, 'built': datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds'),
                'seconds': round(time.monotonic() - started), 'size': library.stat().st_size}
    (args.work / 'library.json').write_text(json.dumps(manifest, indent=1) + '\n')
    if not args.keep:
        shutil.rmtree(build, ignore_errors=True)
    emit('done', library=str(library), **manifest)


if __name__ == '__main__':
    main()
