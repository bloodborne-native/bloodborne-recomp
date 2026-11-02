# bloodborne-recomp

[![Discord](https://img.shields.io/badge/Discord-join%20the%20server-5865F2?logo=discord&logoColor=white)](https://discord.gg/QS6vHz45xA)

A **static recompiler** for Bloodborne (PS4, CUSA03173 1.09): it reads the game's x86-64 executable
and writes its functions out as C, which a compiler turns into a native library the game loads
(arm64 today). In plain words, it rebuilds the PS4 game's code so that it stops being PS4 code, and
runs as native code on the machine in front of you.

**This is the tool, not the game.** It reads your own copy of the game (CUSA03173, update 1.09) on
your own device. No game code is in this repository, and none is ever published.

**Just want to play?** Get the app from
[bloodborne-mac](https://github.com/bloodborne-native/bloodborne-mac/releases). You never need to
touch this repository: the app's launcher builds the recompiled code for you, under **Recompiled
code** in Settings, in about 14 minutes on an M3 Pro.

## Why

A PS4 game is more than a different processor. Its executable is built for one machine: the PS4's
system, its libraries, its GPU. Running it anywhere else takes layers that pretend to be a PS4, and
that is true on every platform:

- On **arm64** (Apple Silicon, phones), the x86-64 code itself has to be translated.
- On an **x86-64 PC**, the processor can run the code, but it is still a PS4 executable: it has to be
  loaded and patched in memory, and it still expects a PS4 underneath.

Every one of those layers costs time and memory, and limits what can be done with the game.

The goal of this project is to remove them one by one, until the game talks directly to the
platform it runs on, as if it had been made for it. Whatever that platform is.

## How it works

The usual way to run foreign code is **dynamic**: a JIT translator converts it while the game runs.
A **static** recompiler converts it once, ahead of time, before the game starts:

1. The recompiler reads your copy of the game's executable and finds its functions.
2. It writes each function out as C.
3. A compiler turns that C into a native library on your device.
4. The game loads the library and runs it in place of the translated code.

The generated code is not a port to new source code: it is the game's own logic, instruction by
instruction. Guest memory is host memory and x86-64 and arm64 share the LP64 layout rules, so it
works directly on the game's live objects. It stands on bbcpu, this repository's CPU layer.

Anything that cannot be recompiled yet still runs in bbcpu's translator, so the game stays playable
at every step. Calls recorded during play can be replayed against the generated code, and every
instruction form the generator handles is checked against the interpreter.

The same approach is behind N64Recomp and *Unleashed Recompiled*.

## What it makes possible

The point is a game that is truly native. Once its code is C, it is source a compiler can build for
any target and optimize like any other program, and it is no longer tied to the PS4's executable:

- **One way to reach any platform.** The generated C is not written for one processor, so the same
  approach serves arm64 and x86-64. Today's build scripts target arm64 on Apple's systems; other
  targets are still to be done.
- **Less overhead.** The processor does the game's work, not the work of a translator.
- **A way out of the PS4.** The game's calls to its GPU and system libraries are code that can be
  found, named and replaced, which is how the engine's rendering was mapped for the native renderer.
- **Places a translator cannot go.** Some platforms do not allow generating code while running
  (JIT). iOS is one, and the
  [iPhone proof of concept](https://github.com/bloodborne-native/bloodborne-ios) runs because the
  code is already compiled when the game starts.

## What has been done

| Milestone | Progress | |
|---|---|---|
| R0–R2 · every function recompiled | `████████████████████` 100% | 240,978 of the game's 241,821 functions run recompiled; 99.1% of their instructions in plain C |
| R3 · speed | `████████████████████` 100% | 14% less CPU time than the translator at the same frame rate |
| R4 · annotations | `████████████████████` 100% | 204 functions named; the engine's draw state, models, materials and shaders described ([`annotations/`](annotations)) |

- **Built on the player's device.** The launcher builds the library with no Xcode or developer
  setup, and later updates compile only what changed.
- **Less memory.** The game's peak footprint on the Mac went from 7.8 GB to 5.7 GB, because the
  library no longer keeps the whole game decoded after checking it.
- **Fewer calls back to the translator:** from 69,600 a second to 9,400.

## What is still not native

Recompiling is one layer of [several](https://github.com/bloodborne-native/bloodborne-mac/blob/main/docs/ROADMAP.md#the-ps4-layers), and
a layer counts as native only when nothing in it is translated any more. Recompiled code runs
natively, but the library is opt-in, and it still expects a PS4:
it writes PS4 GPU commands that have to be decoded each frame, and it calls PS4 system libraries
that the runtime provides. That work is in
[bloodborne-runtime](https://github.com/bloodborne-native/bloodborne-runtime), through the
[native renderer](https://github.com/bloodborne-native/bloodborne-runtime/blob/main/docs/NATIVE_RENDERER.md).

This repository owns two layers: [the game's code](docs/layers/game-code.md) and
[x86 memory ordering](docs/layers/memory-ordering.md). What remains of the CPU work is memory
ordering, kept only around the engine's synchronization. Other ideas were measured and gained little or nothing (flags, private frames,
vector barriers, the host-call path). Details:
[docs/RECOMPILATION.md](docs/RECOMPILATION.md) and the
[status page](https://github.com/bloodborne-native/bloodborne-recomp/wiki/Recompilation-status).
All docs: [docs/README.md](docs/README.md).

## What is in this repository

- **The recompiler:** reads the game's executable and generates the C.
- **bbcpu:** the CPU layer the recompiled code stands on: an x86-64 decoder, an interpreter, an
  x86-64 → arm64 translator, and the record and replay tracer that checks recompiled functions
  against the original. Nothing here depends on an operating system: programs link
  `out/libbbcpu.a` and give it `runtime_low_map` (memory for guest stacks).
- **Annotations:** the project's names for the game's functions and the layouts of its objects.

### The project's repositories

| Repository | |
|---|---|
| **bloodborne-recomp** (this one) | The recompiler and bbcpu: portable |
| [bloodborne-runtime](https://github.com/bloodborne-native/bloodborne-runtime) | Loads the game: the PS4 libraries, the renderer; loads the recompiled library |
| [bloodborne-launcher](https://github.com/bloodborne-native/bloodborne-launcher) | The independent launcher, packaged in the macOS app |
| [bloodborne-mac](https://github.com/bloodborne-native/bloodborne-mac) | Packages the macOS app, built on runtime and recomp |
| [bloodborne-ios](https://github.com/bloodborne-native/bloodborne-ios) | The iPhone app (a proof of concept), with the recompiled library built for the phone |

## Build and use

Needs clang, cmake, ninja and Python 3 (on macOS: `xcode-select --install` and
`brew install cmake ninja`). Zydis (the x86 decoder) is built into `deps/` the first time.

```sh
./build.sh
GAME=/path/to/CUSA03173                             # your dump, with the 1.09 update
python3 tools/recomp/ps4elf.py "$GAME/eboot.bin" out/eboot.elf
python3 tools/recomp/pointers.py "$GAME/eboot.bin" out/recomp/pointers.txt
out/recomp/scan out/eboot.elf out/recomp out/recomp/pointers.txt
python3 tools/recomp/imports.py "$GAME/eboot.bin" out/recomp
tail -n +2 out/recomp/functions.tsv | cut -f1 > out/recomp/all.txt
tools/recomp/recomp.sh --file out/recomp/all.txt    # out/recomp/librecomp.dylib
```

The game loads the library with `BB_RECOMP_LIB=out/recomp/librecomp.dylib` (bloodborne-runtime).
`tools/recomp/fuzz.sh` checks every instruction form the generator handles against the
interpreter; `tools/recomp/replay.sh` checks recorded calls ([docs/RECOMPILATION.md](docs/RECOMPILATION.md)).

## Why the releases hold LLVM

The recompiled library is game code, so it is never published: each player's launcher builds it
on their own device, from their own copy of the game (about 14 minutes on an M3 Pro). That build
needs a C compiler and a linker, and players should not need Xcode or a developer setup for it.

So the [releases](https://github.com/bloodborne-native/bloodborne-recomp/releases) hold a compiler
for the launcher to download, not builds of the recompiler. The `toolchain-llvm-<version>` releases
contain LLVM's official macOS arm64 release, trimmed by
[`tools/recomp/toolchain.sh`](tools/recomp/toolchain.sh) to what the build uses: `clang`,
`ld64.lld`, clang's own headers and LLVM's licence (70 MB, against 1.5 GB for the whole release).
The script checks the official archive against a pinned SHA-256 before trimming it. The launcher
checks the download against the trimmed archive's own SHA-256, and deletes the toolchain once the
library is built. LLVM is under the Apache License 2.0 with LLVM Exceptions; the full text is in
the archive. How the library is built: [docs/RECOMPILATION.md](docs/RECOMPILATION.md#building-the-library-without-xcode).

## Credits

Full list: [docs/CREDITS.md](docs/CREDITS.md).

Started inside [bloodborne_mac](https://github.com/PabloVSouza/bloodborne_mac), itself built on
[bbport](https://github.com/deadinside28/bloodborne_pc) by deadinside28 and on
[shadPS4](https://github.com/shadps4-emu/shadPS4). `tools/recomp/ps4elf.py` is bbport's
executable reader; `tools/recomp/import_names.inc` comes from shadPS4's symbol lists (aerolib).
[Zydis](https://github.com/zyantific/zydis) (MIT) decodes x86. The approach follows the static
recompilers of other games (N64Recomp, XenonRecomp / *Unleashed Recompiled*).

Licensed under the [GNU GPL v2 or later](LICENSE). Not affiliated with Sony Interactive
Entertainment or FromSoftware; *Bloodborne* is a trademark of Sony Interactive Entertainment.
