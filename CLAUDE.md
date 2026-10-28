# bloodborne-recomp

A static recompiler for Bloodborne (PS4, CUSA03173 1.09) and bbcpu, the CPU layer it stands on.
It reads the player's own `eboot.bin` and writes the game's functions as C, compiled into a native
library the game loads (`BB_RECOMP_LIB`, in bloodborne-runtime). Plan, milestones and results:
`docs/RECOMPILATION.md` and the wiki's Recompilation-status page.

## Layout

- `src/cpu/`: bbcpu. Decoder (Zydis), interpreter (`interp*.c`), arm64 translator
  (`jit_arm64.c`), record/replay tracer (`record.c`), natives (`native.c`), the recompiled-code
  runtime (`recomp.c`: loading, `rc_call`/`rc_tail`/`rc_bail`, frames with their own return
  addresses, the import fast path). Hosts link `out/libbbcpu.a` and provide `runtime_low_map`.
- `src/recomp/rc.h`: what generated code includes (API version `BB_RECOMP_API_VERSION`; bump it
  when RcApi or RcFunction change).
- `tools/recomp/`: `scan.c` (function table: unwind tables + calls, jumps, lea, relocation
  pointers), `bbrecomp.c` (the generator), `recomp.sh`, `replay.c`/`replay.sh`, `fuzz.sh`,
  `ps4elf.py`, `pointers.py`, `imports.py`, `profile.py`, `label.py`, `inspect.sh` (Ghidra).
- `annotations/functions.tsv`: the project's names for the game's functions (R4).
- `tests/`: fuzzers and CPU tests. `out/` and `deps/` are not tracked.

## Build and check

```sh
./build.sh                                    # Zydis into deps/, out/libbbcpu.a, scan, bbrecomp
tools/cpu/test_jit.sh                         # CPU tests (CI runs them with the arm64 encodings)
GAME=~/Downloads/PS4/Bloodborne/CUSA03173     # the user's dump
python3 tools/recomp/ps4elf.py "$GAME/eboot.bin" out/eboot.elf
python3 tools/recomp/pointers.py "$GAME/eboot.bin" out/recomp/pointers.txt
out/recomp/scan out/eboot.elf out/recomp out/recomp/pointers.txt
python3 tools/recomp/imports.py "$GAME/eboot.bin" out/recomp
tools/recomp/recomp.sh --file LIST            # LIST: offsets; all: tail -n +2 out/recomp/functions.tsv | cut -f1
tools/recomp/fuzz.sh                          # must stay at 0 failed, 0 crashed
tools/recomp/replay.sh RECORD.rec --recomp out/recomp/librecomp.dylib
```

A change to the generator or the runtime is checked by: the fuzzer; replays of recorded calls
(records come from the game with `BB_RECORD`, see RECOMPILATION.md); and a game run through
bloodborne-runtime (the runtime's `tools/test_run.sh NAME BB_RECOMP_LIB=$PWD/recomp/out/recomp/librecomp.dylib`,
run from the runtime checkout).
The full build (241,031 functions) takes ~11 minutes and makes a ~400 MB library; only files whose
C changed are compiled again.

## Debugging recompiled code

- Crash markers in a game log: `STOP:`, `Host fault`, `Guest fault`, or the run failing to reach
  the level. `== Stall during rendering` is **not** one (healthy runs print it).
- A GPU assertion while loading (`Unimplemented PM4 type 0`) was two game threads writing one
  command buffer (fixed in bloodborne-runtime: `runtime_cmdbuf.c`, bbcpu guards); recompiled
  functions call each other directly and are not guarded, so if it comes back with the library
  only, look there first. `BB_CB_PROBE=1` names the threads.
- Bisect with `BB_RECOMP_ONLY=@FILE` (keep only these) or `BB_RECOMP_OFF=@FILE`; also
  `BB_RECOMP_THREADS` / `BB_RECOMP_NOT_THREADS` (by thread name) and `BB_RECOMP_NODIRECT=1`.
  `BB_RECOMP_TARGETS=1` prints the busiest calls into the translator every 10 s.
- Lessons so far: import stubs and `setjmp` callers stay with the translator; every runtime call
  into the translator has its own return address; jump tables are decoded from their `cmp`/`ja`
  bound. `BB_REPLAY_WATCH=ADDRESS` traces one byte in a replay.
- REA (an MCP server with Ghidra, on the user's machine) answers "who calls this", "decompile
  this" and "what references this address" about the game's code; its output is game code and
  never goes into a repository. Open `out/recomp/eboot_ghidra.elf` (inspect.sh writes it: Ghidra's
  loader does not know the PS4's ELF type), not `out/eboot.elf`. A new session analyses the whole
  image before answering: its MCP entry sets `GHIDRA_HEADLESS_MAXMEM=6G` and
  `REA_GHIDRA_STARTUP_TIMEOUT_MS` (REA's default, 330 s, is too short for this image). Close it
  with a `snapshot_path` in `out/recomp/rea/` and open later sessions from that snapshot.
- `tools/recomp/inspect.sh` is the same without REA: Ghidra headless on the project
  `out/recomp/ghidra`, analysed once (6 GB heap). Disassembly with the annotations' names at each
  call is often enough for small functions (`llvm-objdump` over `out/eboot.elf`).
- Fault diagnostics: `BB_JIT_FAULT_MAP=1` reports the exact translated guest instruction;
  `BB_JIT_WATCH` records selected field accesses without function-entry callbacks. Usage is in
  `docs/ARM64_NATIVE.md`. Their game-derived output stays outside Git and the wiki.
- Speed: the frame rate is bound by rendering; compare CPU time (`ps -o cputime=` over 30 s in
  the level) between alternating runs with and without the library.

## Where it stands and what is next

- R2 done: all 241,031 functions generated (99.1% of instructions in C), 240,978 run in the game.
  Same frame rate and **less CPU time than the translator** since vector registers moved to
  locals, `rc_call` lost its TLS reads and calls copy only the registers that need it (`ps -o
  cputime=` over 30 s, 2026-10-25: 344% of one core against 402% at the 1st Floor Sickroom lamp,
  about 1,283 draws a frame; 357% against 392% at the runtime's test save in Cathedral Ward, one
  run each). R3 is done (2026-10-25): 99.1% of instructions in C, and the library about 14% under
  the translator's CPU time at the same frame rate. What took it there: vector registers in locals
  (the FMOD mixer), `rc_call` as a leaf (4 points at the Sickroom), and copying only the registers
  that need it around calls (29 points; the generator's register sync: markers filled in from two
  analyses of the generated text). Tried after that without a measurable gain (local branches,
  Sickroom, 3 runs each against the translator): a cache at each indirect call site
  (`r3-call-cache`: 7.8 of 11.4 million calls a second off `rc_call`, the same CPU time, code
  +5.6%; `rc_call` costs its indirect jump, not the lookup); vector accesses as two 8-byte
  `ldapr`/`stlr` instead of a barrier (`r3-vec-order`: 348% against 400%, code +4.6%);
  `ldapur`/`stlur` with their offsets (`-mcpu=apple-m2` and later, not the M1: 341% against 397%,
  code −3%). A library with no x86 ordering at all (`r3-order-ceiling`, unsafe; relaxed atomics,
  since plain C accesses hung before the first frame) took 311% against 407%: about 33 points, the
  ceiling of relaxing ordering. Neither the vector barriers nor the `add`s explain it: it is the
  ordering of the ordinary accesses itself. What is left, optional: plain accesses to a function's
  own frame when it never computes an address in its stack (no `lea` from `rsp`/`rbp`, `rsp`
  copied nowhere but `rbp`), so no other thread can reach that frame; it needs the generator's
  analysis, a safety case and a long play session (the maintainer's conditions), and the frame
  rate is bound by rendering. The library runs about 0.5 FPS under the translator at the Sickroom
  in every round (38.0 against 38.6), unexplained so far. To measure: the Sickroom, alternating
  with the translator (copies of the Sickroom and Dream saves are in the runtime's
  `out/r3-saves/`; `SAVE_FROM=DIR` goes in the environment: `tools/test_run.sh` does not read it
  from its `ENV=VALUE` arguments, and without it copies the player's current save); `BB_PROBE=`
  runs another build of `bb-probe` (keep the name) for A/B runs without rebuilding. `sample` gets
  about 2 samples a thread from a library with its debug map: profile a copy made with `strip -S`
  and `codesign -s - -f`.
- Data from earlier sessions, in `out/recomp/` of the user's clone (game-derived, never commit):
  `records_r3/` (50 recorded calls of each of the 60 busiest functions at the Sickroom lamp;
  2,944 of 3,018 match, the rest also fail with the original), `librecomp_main_a8b96c3.dylib`
  (the library before vector locals, for A/B runs), `records40/` (40 recorded calls of each of the 933 busiest functions: 911 match, the rest also
  fail with the original), `hot.txt` (those 933), `ran.txt` (the 49,095 a 3-minute route ran),
  `all.txt`, `jitmap.txt`. Regenerate `functions.tsv` and the rest with the commands above.
- R5 (the native renderer) is planned and worked on in bloodborne-runtime
  (`docs/NATIVE_RENDERER.md`). Its part here: calls of hooked functions (bbcpu guards, natives)
  from recompiled code must go through the runtime, not directly to the recompiled callee.
- The library is built from the launcher (done 2026-10-28): `build_library.py` with a downloaded
  LLVM toolchain and no macOS SDK (`toolchain.sh`; RECOMPILATION.md, "Building the library
  without Xcode"; not `-ffreestanding`: 32% more code, slower frames).
- Now (maintainer, 2026-10-28: started before 0.9.0 final; the app's `docs/ROADMAP.md`): R4,
  aimed at the engine's render boundary (for N6), then its synchronization primitives and the
  hottest signatures. Names go in `annotations/functions.tsv` (RECOMPILATION.md, "Annotations");
  REA's output informs them but never goes in the file. Alongside, in the generator: flags only
  where read, a function's own frame as host locals, ordering kept only for the engine's
  synchronization.

## Working rules (all four repositories)

- **The project is four repositories**, nested as submodules:
  [bloodborne-mac](https://github.com/bloodborne-native/bloodborne-mac) (the app) ⊃ `runtime/` =
  [bloodborne-runtime](https://github.com/bloodborne-native/bloodborne-runtime) ⊃ `recomp/` =
  [bloodborne-recomp](https://github.com/bloodborne-native/bloodborne-recomp); the app also has
  `launcher/` = [bloodborne-launcher](https://github.com/bloodborne-native/bloodborne-launcher).
  Commit in the repository that owns the file, push it, then bump the submodule pointer in the
  parent and push that too. Submodules are checked out at a commit: run `git checkout main` (and
  `git pull`) there before editing.
- **Before any push, check `git remote get-url origin`.** Push to `main`; ask before
  force-pushing anything that existed before this session.
- **Commits:** a short subject, a body that says what changed and why (numbers when measured),
  and the trailer the session asks for.
- **Credits:** built on [bbport](https://github.com/deadinside28/bloodborne_pc) by deadinside28
  and on [shadPS4](https://github.com/shadps4-emu/shadPS4); keep them credited. Code taken from
  another project or fork is credited in the same commit: CREDITS.md, the wiki's credits page,
  the commit message and a short code comment.
- **No game code, ever:** no decompiled or generated game code, no game files, no records of game
  memory in any repository or wiki.
- **Testing the game:** `tools/test_run.sh` (a copy of the save; the real one is never written),
  in English, one game at a time (`pgrep -x bb-probe` first). Test windows open on the user's
  screen: say so before long batches, and stop when asked.
- **Every test run says what it tests, on screen:** `TEST_TITLE="..." TEST_DETAIL="..."
  tools/test_run.sh NAME ...` puts a banner at the top of the game's window (the title defaults to
  NAME). The title names the test and the build (e.g. "M2 skeleton: new build"); the detail
  says the settings and what is being checked. Never start a test window without them.
- **Never capture the user's screen.** Pictures come from the game: `tools/grab.sh`.
- **Shell:** no `rm -rf` on variable paths; no `xargs ... sh -c '...'`.
- **Docs:** plain, precise English; numbers measured, not guessed. Docs say what is true now;
  investigation logs do not go in docs (conclusions do). The wiki mirrors the docs; update both.
