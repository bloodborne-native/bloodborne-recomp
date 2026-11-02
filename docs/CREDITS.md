# Credits and licenses

[← Documentation](README.md)

This project is licensed under the **GNU GPL v2 or later** ([LICENSE](../LICENSE)).

## Built on

- [**bbport**](https://github.com/deadinside28/bloodborne_pc) by deadinside28, the native Linux
  port of Bloodborne this work started from as a fork (through bloodborne_mac). The recompiler's
  reader of the game's executable, `tools/recomp/ps4elf.py`, is bbport's (`scripts/prepare.py`,
  `scripts/link_modules.py`, `scripts/link_libc.py`), copied unchanged so this repository stands
  alone.
- [**shadPS4**](https://github.com/shadps4-emu/shadPS4): the PS4 symbol names in
  `tools/recomp/import_names.inc` come from its symbol lists (aerolib).

## Libraries and tools

- [bbcpu](https://github.com/bloodborne-native/bbcpu) (GPL-2.0-or-later): the CPU layer. It was
  developed in this repository until 2026-11-02, and its authors are in its credits.
- [Zydis](https://github.com/zyantific/zydis) and Zycore (MIT): the x86-64 decoder, built by
  bbcpu's `build.sh` into `deps/`.
- [Ghidra](https://ghidra-sre.org) (Apache-2.0), optional: `tools/recomp/inspect.sh`.
- [LLVM](https://llvm.org) (Apache-2.0 with LLVM exception): clang and `ld64.lld` build the
  library without Xcode (`build_library.py`); `tools/recomp/toolchain.sh` trims LLVM's official
  release, published with its licence as this repository's `toolchain-llvm-23.1.4` release.

## Prior work

The approach follows the static recompilers of other games: [N64Recomp](https://github.com/N64Recomp/N64Recomp)
and [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) (*Unleashed Recompiled*). No code is
taken from them.

## Disclaimer

Not affiliated with Sony Interactive Entertainment or FromSoftware. *Bloodborne* is a trademark of
Sony Interactive Entertainment. No game code or files are included: the recompiler runs on the
player's own copy.
