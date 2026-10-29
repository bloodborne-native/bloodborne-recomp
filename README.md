# bloodborne-recomp

[![Discord](https://img.shields.io/badge/Discord-join%20the%20server-5865F2?logo=discord&logoColor=white)](https://discord.gg/QS6vHz45xA)

A static recompiler for Bloodborne (PS4, CUSA03173 1.09): it reads **your own** copy of the game's
executable and writes its functions out as C, which a compiler turns into a native library. No
game code is in this repository, and none is ever published: only the tool.

It also holds **bbcpu**, the CPU layer the recompiled code stands on: an x86-64 decoder, an
interpreter, an x86-64 → arm64 translator, and the record/replay tracer that checks recompiled
functions against the original. Nothing here depends on an operating system: programs link
`out/libbbcpu.a` and give it `runtime_low_map` (memory for guest stacks).

| Repository | |
|---|---|
| **bloodborne-recomp** (this one) | The recompiler and bbcpu: portable |
| [bloodborne-runtime](https://github.com/bloodborne-native/bloodborne-runtime) | Loads the game: the PS4 libraries, the renderer; loads the recompiled library |
| [bloodborne-launcher](https://github.com/bloodborne-native/bloodborne-launcher) | The independent launcher, packaged in the macOS app |
| [bloodborne-mac](https://github.com/bloodborne-native/bloodborne-mac) | Packages the macOS app, built on runtime and recomp |

## Status

| Milestone | Progress | |
|---|---|---|
| R0–R2 · every function recompiled | `████████████████████` 100% | 240,978 of the game's 241,821 functions run recompiled; 99.1% of their instructions in plain C |
| R3 · speed | `████████████████████` 100% | 14% less CPU time than the translator at the same frame rate |
| R4 · annotations | `████████████████████` 100% | 204 functions named; the engine's draw state, models, materials and shaders described ([`annotations/`](annotations)) |

The launcher builds the library on the player's device with the LLVM toolchain these releases
hold (below). **Next:** the CPU work's last part, memory ordering kept only around the engine's
synchronization, with the maintainer's conditions; the rest is measured (calls left to the
translator 69,600 a second to 9,400; flags, private frames, vector barriers and the host-call path
gain little or nothing). The library also no longer keeps the whole game decoded after checking it
(the game's peak footprint 7.8 GB to 5.7 GB on the Mac). Details:
[docs/RECOMPILATION.md](docs/RECOMPILATION.md) and the
[status page](https://github.com/bloodborne-native/bloodborne-recomp/wiki/Recompilation-status).
All docs: [docs/README.md](docs/README.md).

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
