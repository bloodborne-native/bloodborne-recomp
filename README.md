# bloodborne-recomp

[![Discord](https://img.shields.io/badge/Discord-join%20the%20server-5865F2?logo=discord&logoColor=white)](https://discord.gg/QS6vHz45xA)

Bloodborne was written for the PS4. This project rebuilds the game's code for other machines, so
that it runs as native code instead of being translated while you play.

**This is the tool, not the game.** It reads your own copy of the game (CUSA03173, update 1.09) on
your own device. No game code is in this repository, and none is ever published.

**Just want to play?** Get the app from
[bloodborne-mac](https://github.com/bloodborne-native/bloodborne-mac/releases). You never need to
touch this repository: the app's launcher builds the recompiled code for you, under **Recompiled
code** in Settings, in about 14 minutes on an M3 Pro.

## Why

The PS4 runs x86-64 code. A Mac or an iPhone runs arm64, so the game's code cannot run on them as
it is. The usual answer is to translate that code while the game plays. It works, but the
translating takes processor time and memory that the game could use, and every layer between the
game and the machine limits what can be done with it.

The goal of this project is to remove those layers one by one, until the game talks directly to the
platform it runs on, as if it had been made for it.

## How it works

Recompiling does the translation once, ahead of time:

1. The recompiler reads your copy of the game's executable and finds its functions.
2. It writes each function out as C.
3. A compiler turns that C into a native library on your device.
4. The game loads the library and runs it in place of the translated code.

Anything that cannot be recompiled yet still runs in the translator, so the game stays playable at
every step. Calls recorded during play can be replayed against the generated code, and every
instruction form the generator handles is checked against the interpreter.

The same approach is behind N64Recomp and *Unleashed Recompiled*.

## Why it matters: the iPhone

iOS shows the difference best. Normal iOS apps cannot generate and run code on the fly (JIT), and
that is what translating a game while it plays depends on. Apps also have a much tighter memory
budget than on a Mac.

With recompiled code, everything is compiled before the game starts, and the game's code does not
have to be kept decoded in memory. That is what makes the
[iPhone proof of concept](https://github.com/bloodborne-native/bloodborne-ios) possible at all.

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

Recompiling is one layer of several. The game's code now runs natively, but it still expects a PS4:
it writes PS4 GPU commands that have to be decoded each frame, and it calls PS4 system libraries
that the runtime provides. That work is in
[bloodborne-runtime](https://github.com/bloodborne-native/bloodborne-runtime), through the
[native renderer](https://github.com/bloodborne-native/bloodborne-runtime/blob/main/docs/NATIVE_RENDERER.md).

Here, what remains of the CPU work is memory ordering, kept only around the engine's
synchronization. Other ideas were measured and gained little or nothing (flags, private frames,
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
