# Recompilation

> [!NOTE]
> [Documentation](README.md). Progress is tracked on the wiki: [Recompilation status](https://github.com/bloodborne-native/bloodborne-recomp/wiki/Recompilation-status).
> The game side (loading the recompiled library, recording calls) is in
> [bloodborne-runtime](https://github.com/bloodborne-native/bloodborne-runtime); the Mac app in
> [bloodborne-mac](https://github.com/bloodborne-native/bloodborne-mac).

Goal: the game's own code running as native arm64 code, compiled ahead of time instead of
translated while the game runs, and in the end the engine's graphics drawn with each platform's
own GPU API (Vulkan, Metal) instead of PS4 command buffers translated while the game runs.

The way there is a **static recompiler**: a tool that runs on the player's Mac, reads *their own*
`eboot.bin` and writes the game's functions out as C, which clang compiles into a native library
the game loads. This project publishes the recompiler, never the game's code, as *Unleashed
Recompiled* and N64Recomp do.

## What is published

| Published (this repository) | Never published |
|---|---|
| The recompiler, its runtime, the translator | The game's code |
| Record and replay, profiling and analysis tools | |
| Annotations: function names, signatures, struct layouts | |

The recompiled code is produced on the player's machine from their copy, and stays there.

## Principles

1. **Only FromSoftware's code first.** Havok, FMOD, Scaleform, YEBIS, Lua and zlib are licensed
   libraries; they can be recompiled like the rest later, the game's and engine's code comes first.
2. **Always playable.** Recompiled functions replace the translated ones one by one; anything not
   recompiled runs in the translator, which is also the fallback inside a generated function.
3. **The game's own data layouts.** Guest memory is host memory and x86-64 and arm64 share the
   LP64 layout rules: generated code works on the game's live objects.
4. **Checked by behaviour.** Calls recorded during play are replayed against the generated code;
   results, memory writes and calls must match exactly.
5. **Behind switches.** Generated functions can be turned off one by one, so a wrong one is found
   by bisection.

## The layers

The work is split by the [PS4 layers](https://github.com/bloodborne-native/bloodborne-mac/blob/main/docs/ROADMAP.md#the-ps4-layers), and a layer counts as native only when nothing in it
is translated any more. This repository owns two:

| Layer | Native | Where it stands | Next |
|---|---|---|---|
| [The game's code](layers/game-code.md) | 96% `███████████████████░` | Recompiled and built on the player's device; opt-in, the translator is the default | The runtime's share of the game's main thread (about 11% of its busy time in Iosefka's Clinic: `rc_call` 5%, host calls 3.5%, code left to the translator 2.5%) |
| [x86 memory ordering](layers/memory-ordering.md) | 0% `░░░░░░░░░░░░░░░░░░░░` | Kept on every access | Only around the engine's synchronization, with the maintainer's conditions: the generator's analysis, a safety case and a long play session |

Measured with little or no gain ("Where the CPU time goes"): clang already drops the flags nothing
reads, and plain accesses to private frames gain nothing. The order of the work: the app's
[priorities](https://github.com/bloodborne-native/bloodborne-mac/blob/main/docs/ROADMAP.md#priorities).

## Progress

*Updated 2026-11-02.* The game's executable has 179,698 functions, about 10.7 million instructions.

| | Progress | |
|---|---|---|
| Functions recompiled, running in the game | `████████████████████` 99.9% | 181,521 of 181,570, with the modules |
| Instructions recompiled | `███████████████████░` 96.2% | 10,284,725 of 10,691,122 |
| Instructions in plain C (of those recompiled) | `████████████████████` 99% | |
| CPU time recompiled, against the translator | `█████████████████░░░` 86% | 344% of one core against 402% (1st Floor Sickroom), 360% against 397% (Iosefka's Clinic, 2026-10-27) |
| Functions checked call by call (recordings) | `░░░░░░░░░░░░░░░░░░░░` 0.5% | 933 of 179,698 |
| Functions labelled by library (2026-11-02) | `███████████████████░` 95.5% | 171,706 of 179,698 |
| Functions named by hand (2026-11-02) | `░░░░░░░░░░░░░░░░░░░░` 0.2% | 885 of 179,698, 228 sure and 657 guesses (libGnm's command buffer methods; the engine's draw methods, state setters, mesh, model, material and lighting code; the synchronization primitives; the busiest functions; texture repositories) |
| Functions placed, of those to understand (2026-11-02) | `████████████████░░░░` 81% | 48,640 of 59,867 longer than five instructions, in FromSoftware's code or not labelled yet |
| Functions identified, of the target (2026-11-02) | `██████████░░░░░░░░░░` 52% | about 30,300 of about 58,000: 2,454 with the game's own name, 27,799 with a known role, much of that compiler and template code |

Every function is generated; the ones left to the translator are the 660 import stubs, the 130
functions that call `setjmp`, and 53 that the game's patches (51) or the runtime's live switches
(2) change. Instructions recompiled count what control flow reaches; the rest is padding and code
nothing reaches. Every recompiled function can run in the game; the call-by-call checks cover the
busiest, and the fuzzing covers every instruction form the generator handles.

## The recompiler

`tools/recomp/bbrecomp.c` reads `eboot.bin`, takes functions from the function table
(`tools/recomp/scan.c`), and writes one C function per game function against `src/recomp/rc.h`;
`tools/recomp/recomp.sh` compiles them into a library, one file per 128 KiB of the image, compiling
again only the files whose C changed:

- The code is decoded by following its control flow from the entry. Jumps become `goto`s (labels
  only where something jumps); returns become C returns. Calls of other recompiled functions are
  direct C calls; other calls go through the runtime.
- Guest registers live in local arrays: the 16 general-purpose ones, and the vector registers
  the function's instructions name. They go back to the CPU state around calls, interpreted
  instructions and returns, only those that need to: a register is stored there if it may have
  been written since it was last loaded, and loaded after a call only if something reads it
  before overwriting it (two analyses over the function's gotos, done on the generated text). The
  flags are computed lazily (clang drops what nothing reads).
- Memory accesses keep x86's ordering between threads (acquire/release), as the translator's do,
  the stack's too: the game's jobs write into their creator's stack.
- In C: moves, integer arithmetic and logic (with carry), shifts and rotates, multiplies and
  divisions, bit tests, scans and counts, BMI1, byte swaps, `rdtsc`, conditional sets and moves,
  exchanges, locked operations (compare-and-exchange loops), and the common SSE/AVX moves,
  arithmetic, dot products, blends, inserts and extracts, conversions, bitwise operations,
  compares, shifts, shuffles, packs, unpacks and rounding, each as the interpreter does
  it. A division that faults runs in the interpreter, as does anything else, one instruction at a
  time, so the output is complete from the start.
- Jump tables become a `switch` over their cases: the compilers' forms with the bound checked by
  `ja` to the default or `jbe` to the table, up to 32 instructions before the jump, and an index
  copied before or after the check. Any other indirect jump to an unknown place goes back to the
  translator; a jump out of the function is a tail call.
- Decoding follows the code from the entry and stops after calls that never return
  (`__stack_chk_fail`, `abort`, `exit`, `_exit`), so the data after them (often a jump table) is
  not taken for code. Where instructions decoded from different starting points still overlap, an
  instruction continues with an explicit jump to where it really goes on, never into its
  neighbour in address order (`BB_RECOMP_OVERLAPS=1` lists such functions).
- Left to the translator: the import stubs (one jump each to the PS4 libraries) and the functions
  that call `setjmp` (its return point must be guest code that can be resumed).
- A call the runtime runs in the translator gets a return address of its own; when a `longjmp` or
  an exception leaves several of them at once, the runtime goes back to the right one on the host
  as well.

The game loads the library (`BB_RECOMP_LIB`) after its patches: a function whose code changed since
it was generated (a game patch, a hook inside it: `bbcpu_recomp_hash`) stays with the translator,
and so does one holding code the runtime rewrites while the game runs (`bbcpu_recomp_exclude`: the
live effect switches). A function whose only change is one of the runtime's hooks at its entry (a
jump to a stub `int3; the replaced instructions; jmp back`, checked byte for byte, with the hash of
the function with those instructions put back) stays recompiled: a wrapper of its own runs the
trap handlers, as the int3 would, then the recompiled function (19 such functions with the render
boundary on, 64 at most).
`BB_RECOMP_OFF=OFFSET,...` (or `@FILE`) switches chosen ones off, `BB_RECOMP_ONLY` keeps only the
listed ones (bisecting), `BB_RECOMP_THREADS` / `BB_RECOMP_NOT_THREADS`
limit them to some threads (by name), `BB_RECOMP_NODIRECT=1` sends every call through the
translator. Translated code calls recompiled functions directly.

Two checks: the **recordings** (whole functions, real calls) and **per-instruction fuzzing**
(`tools/recomp/fuzz.sh`: every distinct instruction encoding in the game, at most 30 per mnemonic,
run from random states through the interpreter and through its generated C).

What the checks have found: the interpreter's `lock cmpxchg` reported a separate load's value
after a failed exchange, so two threads could take the same lock (a heap panic during save load,
made frequent by faster recompiled threads); the recorder missed `vmaskmovdqu`'s implicit write;
recompiled import stubs left the runtime's return address where `setjmp` saved it, so a later
`longjmp` returned into a frame that was gone (why stubs and `setjmp` callers stay with the
translator); and every call the runtime ran in the translator once returned to the same address,
so after a `longjmp` the innermost stopped where an outer one should have. Recordings of the 933
busiest functions match on every call for 911; the other 22 also fail when the original runs (they
read the time stamp counter or memory other threads change).

## The game's modules

`tools/recomp/modules.py` adds `sce_module/libc.prx` and `libSceFios2.prx` to the recompiler's view of
the image, at the offsets the game links them to, so `build_library.py` generates them with the
executable: 4,759 functions. 4,607 are generated; 5 that jump between stack frames, by name, and their
147 direct callers stay with the translator. Only 381 of the modules' 2,471 symbols have known names,
so another such function could be missed: `BB_RECOMP_OFF` turns functions off one by one. In the game
181,521 of 181,570 functions run recompiled, with the runtime's check of their bytes passing for all
the modules' generated functions but the 154 import stubs.

## Other architectures

`tools/recomp/host.sh` gives the scripts this machine's flags and library names; `BB_ARCH=x86_64` builds
for x86-64 on Apple Silicon, where it runs under Rosetta. Over 6,571 instruction forms the fuzz test
finds no crash, and no failure on arm64. On x86-64, 36 forms differ, all floating point (`vdpps`,
`vaddss`, `vmulss`), and in all of them both results are NaN: x86 keeps its first operand's NaN, and
the compiler may swap the operands. Ordinary numbers do not differ. Translated code is arm64 only: an
x86-64 host runs what is not recompiled in the interpreter.

## Tools

All write to `out/recomp/`.

| Tool | What it does |
|---|---|
| `tools/recomp/scan.c` | Every function: the unwind tables (exact start and size), then what calls, jumps and code pointers reach (decoded by following control flow); `functions.tsv`, `calls.tsv`, `slots.tsv`, `strings.tsv` |
| `tools/recomp/pointers.py` | The code addresses relocations put into data (`pointers.txt`), for `scan.c`, and where each is stored (`pointer_slots.tsv`), for `label.py` |
| `tools/recomp/imports.py` | The import stubs by name (`imports.tsv`) |
| `annotations/functions.tsv` | The project's names for functions (below, "Annotations"); `bbrecomp` writes them into the generated C |
| `annotations/classes.tsv` | The project's names for classes the executable does not name; `names.py` gives them to the classes' functions |
| `tools/recomp/label.py` | The library of each function (`modules.tsv`), from source paths and messages, spread to neighbours, vtables, callers and callees (`--check`: their agreement) |
| `tools/recomp/profile.py` | CPU time per function and library from a macOS `sample` (`profile.tsv`) |
| `tools/recomp/inspect.sh` | Ghidra's view of chosen functions (`c/<offset>.c`, `.s`), for analysis and annotations |
| `BB_RECORD` (bbcpu's `src/cpu/record.c`) | Calls of chosen functions recorded in the game (`records/<offset>.rec`) |
| `tools/recomp/replay.sh` | Recorded calls replayed and checked (`--recomp LIB`: the generated code) |
| `tools/recomp/recomp.sh` | Generated C for chosen functions, compiled (`librecomp.dylib`; `BB_RECOMP_LIB`) |
| `tools/recomp/build_library.py` | The whole library from a player's game in one resumable run, with only an LLVM toolchain (below); what the launcher runs |
| `tools/recomp/toolchain.sh` | That toolchain: LLVM's official release trimmed to clang, `ld64.lld` and clang's headers |
| `tools/recomp/fuzz.sh` | The generator's C for every instruction form, fuzzed against the interpreter |

```sh
./build.sh                          # Zydis into deps/, out/libbbcpu.a, out/recomp/scan and bbrecomp
GAME=/path/to/CUSA03173  # the game folder (1.09)
python3 tools/recomp/ps4elf.py "$GAME/eboot.bin" out/eboot.elf
python3 tools/recomp/pointers.py "$GAME/eboot.bin" out/recomp/pointers.txt
out/recomp/scan out/eboot.elf out/recomp out/recomp/pointers.txt
python3 tools/recomp/imports.py "$GAME/eboot.bin" out/recomp
python3 tools/recomp/label.py out/recomp

# Generate and compile functions (or every one: tail -n +2 out/recomp/functions.tsv | cut -f1).
tools/recomp/recomp.sh --file LIST          # out/recomp/librecomp.dylib; the game: BB_RECOMP_LIB
tools/recomp/fuzz.sh                        # every instruction form, generated C against the interpreter

# In the game: from a bloodborne-runtime checkout (this repository is its recomp/), with its
# tools/mac_bench.sh. A profile: 5 s of every thread, then the main thread's functions.
SAMPLE=1 tools/mac_bench.sh prof BB_JIT_MAP=$PWD/out/jit.map               # in the runtime
python3 recomp/tools/recomp/profile.py out/bench_prof_*.sample.txt out/jit.map out/bench_prof_*.log Thread_<id>

brew install ghidra
tools/recomp/inspect.sh 0x21b8710

# Record calls (OFFSET:SIZE from functions.tsv) in the game, from the runtime, and replay them here.
tools/mac_bench.sh rec BB_RECORD=0x22928a0:247,0x227e230:579 BB_RECORD_EVERY=7 \
    BB_RECORD_DIR=$PWD/recomp/out/recomp/records                             # in the runtime
tools/recomp/replay.sh out/recomp/records/0x22928a0.rec                      # here
```

## Building the library without Xcode

The launcher builds the library on the player's device with `tools/recomp/build_library.py`, the
app's copy of this repository's tools (its kit: `scan`, `bbrecomp`, the headers the generated C
includes, `tools/recomp/sdkless/` and the Python steps), and a downloaded LLVM toolchain. It needs
no Xcode and no macOS SDK.

```sh
tools/recomp/toolchain.sh OUT                 # llvm-23.1.4-macos-arm64.tar.xz (70 MB) and its SHA-256
python3 tools/recomp/build_library.py --game "$GAME" --work WORK --toolchain LLVM_DIR [--jobs N]
```

- **The toolchain:** LLVM 23.1.4's official macOS arm64 release is 1.5 GB; the build uses 360 MB
  of it (clang, `ld64.lld`, clang's headers, and LLVM's licence: Apache-2.0 with LLVM exception),
  packed as a 70 MB archive. It is published as an asset of this repository's
  `toolchain-llvm-23.1.4` release, and the launcher checks it against the SHA-256 it pins.
- **The flags:** `--target=<arm64 or x86_64>-apple-macos15 -O2` (`BB_ARCH`, by default this machine), the generator's own (`-ffp-contract=off
  -fno-strict-aliasing -fvisibility=hidden`), `-fno-stack-protector`, and `-nostdinc` with clang's
  headers and four stand-ins for the system headers the runtime's headers include
  (`tools/recomp/sdkless/`). Linked by `ld64.lld` with `-nostdlib`, `-undefined error` and only
  these allowed undefined: `memcpy`, `memset`, `memmove` and the 128-bit division helpers, all in
  libSystem, which the game process has loaded. **Not `-ffreestanding`:** it turns built-ins off,
  the code grows 32% and frames took 42.4 ms against 26.3 ms with the usual library.
- **Measured** (M3 Pro, 8 jobs): 5.5 minutes from the game folder to the library, compiling 358 files;
  182,514 functions with the modules; a 296 MB library. At the
  build's peak it needs about 2.5 GB (the toolchain, 1.4 GB of C, 400 MB of objects, the
  library); the C and the objects are deleted once it links. The game ran it with 181,521
  functions replaced and no faults, at 48.1 FPS in Iosefka's Clinic.
- **Progress and resuming:** one JSON object a line on stdout (steps, files compiled, done or
  error). `library.json` beside the library records the executable's SHA-256, this repository's
  version, the library interface (`api`: `BB_RECOMP_API_VERSION`), the compiler, the time and the
  size.
- **Updates compile only what changed:** the objects stay in `WORK/build/obj` (about 400 MB, on top
  of the 314 MB library), each with the fingerprint of the C it came from (its comments left out,
  so the annotations' names do not count, with the compiler and the flags). The next build, after
  an interrupted run or with another version of the recompiler, generates the C again and compiles
  only the files whose fingerprint changed; with nothing changed it took 23 s against 823 s for
  the whole library (M3 Pro). The generated C is still deleted once the library links.
- **Which library the launcher uses:** one built from this executable, by this version or by an
  earlier one with the same library interface. Such a library keeps loading and the launcher offers
  an update; one for another interface is built again. Libraries recorded before the `api` field
  are matched by the kit's `COMPATIBLE` list (every commit since `rc.h` last changed).

## Annotations

**What there is to understand.** Of the executable's 179,698 functions, 18,444 are static initialisers,
generated by the compiler, and about 63,500 are licensed middleware; neither needs naming. Without the
initialisers, FromSoftware's own labelled code longer than five instructions is 56,457 functions, and of
the 3,499 not labelled yet about half are FromSoftware's by their neighbours. So the target is about
58,000 functions, an estimate because the labels are approximate.

**Object files.** Stretches between markers are object files only when short: the median is 17 functions, and one of
13,874 once named FromSoftware's debug menu code after a Havok file. Stretches of more than 500 are not used, and a class that is the only one in five or more stretches is a base they share, not
what they hold.

**NexusRevolution2.** Dantelion's network library, version 2.46.42, lies in one range: its thread names and source paths are
there, so what has no class of its own in that range is placed in it (1,069 functions).

**Source directories.** A run of code between two functions that name a `.cpp` of one directory is that directory's: 537
functions in Dantelion and 112 in the game's own code. A `.cpp` named by more than 15 functions does not count, because its message is inlined.

**Debug pages.** A class's destructor removes its debug page or object entry by a label in wide text, which names 63 classes
(`MapItemMan`, `FrpgPhysWorld`, `WorldBackRead`). The label must be the only page that destructor removes, as
one that removes several owns them as a group, and said by one class only. Constructors are not used: what adds
a page also builds small menu callbacks, and samples read by hand got those wrong.

**Step templates.** `FD4Step` is a template, so each step class has its own copy of its update and of its debug
page. Both say the same wide text, which names 121 copies at once. Its small `setStep` and `nextStep` share the failure "destination step undefined": 110 more. The same way, 270 copies of `std::vector`'s reallocation are one body compared byte by byte, and 63 message getters fall back to a wide text such as `?WeaponName?`, and 1,288 are bodies of one template: method invokers' argument types (folded into one copy, so they carried other classes' names) and the debug menu's page and item helpers. Functions with the same instructions, constants aside, are copies of one template: a group's known names give the others theirs, which held for 99.3% of 15,664 names hidden from the rule, and a group's role read once names it all. A function that stores its class's vtable and is no slot of it is a constructor when the store is into its first argument, unless it frees the object or ends on its base class's vtable, which makes it a destructor, and a factory when it is a short function with one store into what it allocated. A virtual method that starts by storing a vtable into its own object is a destructor. A short function that takes a mutex, does one thing and releases it is a locked accessor.

**Shader passes.** A function that loads shader programs by name in wide text sets up that pass, and takes the
name the programs share: 53 functions.

**Largest groups.** Functions that compile to the same bytes but for addresses are ranked by size, and reading one
of a group names all of it. Thirteen reads named 1,036: the tree's erase and insert, class object getters, the helpers
that add and remove debug menu items, and those that set a play log's fields, each with the field's name.

**Settings.** A short function that reads one setting says its dotted name in wide text, such as
`Game.Option.Control.RotLR`: 98 functions.

**System calls.** A short function whose only call is one SDK import wraps that import, and takes its name: 108 functions.

**Standard library.** A short function that says only a container's failure text, such as `vector<T> too long`, is
that template. Longer ones have the template inlined in real work, so each kind has a size limit.

**Where it stands (2026-11-02).** Of the non-trivial functions in FromSoftware's code or not labelled yet:

| | Functions | What it says |
|---|---:|---|
| The game's own name | 2,454 | Registered methods, steps, messages, singletons, tables |
| A known role | 27,799 | Destructors, class getters, `std::function` slots, EzState opcodes, delegates: much is compiler or template code |
| In a named class or file | 6,904 | Where it belongs, not what it does; 3,703 of them only by their callers, which about half of known functions agree with |
| Only an anonymous class | 11,483 | A vtable or object file, no name |
| Nowhere yet | 11,227 | 5,198 of them have no reference at all |

Scripts and debug menus register methods with their names in wide text next to the code pointer, so those methods carry the names the game gives them.

A function known only by the wide text it loads is named `Says::` plus that text: a hint, not a role. It counts in the "named class or file" row as placed, never as identified.

Those 5,198 have no call, jump, address taken or data pointer anywhere in the executable, and Ghidra agrees on a sample: nothing can run them, so they matter only to the count. A function with one owner goes with it, `Owner::helper_` or `Owner::callback_`: that says who uses it, and a template used by one class lands there too. A helper or nearby label more than 1 MiB from everything solid in its class is dropped: two of three read were shared code the class only uses.

Text and compiler patterns are nearly used up, so what remains is mostly read by hand, a few functions at a
time. A public name for a family of functions, or a subsystem that needs one, is what would speed it up.

**What the executable gives by itself.** `tools/recomp/names.py` finds the vtables in the data and
the functions that write them, which places functions in classes without reading them, and
spreads to a class's helpers through the call graph. Messages name few functions directly (about 580),
and the class names in messages mostly belong to singletons other classes use. The names it writes stay with the player's copy; what is checked goes in
`annotations/functions.tsv`.

**DLRuntimeClass.** The engine has its own type information, which gives 1,410 class names (`GUIButton`,
`EzMenuDialog`). Each class keeps its class object behind a pointer in a global, and code tests that
pointer before creating the object, which finds the global for 1,404 of them. The class's vtable reads the
same global in its first slot, directly or through a getter, and the class object's own vtable in its
tenth. That names 1,676 instance vtables of 1,112 classes with no conflict, and 1,256 class objects:
29,696 functions now sit in a class with a real name, and all nine already named by hand agree. On this
compiler objects hold a vtable's address past two header slots, and with those counted there are 12,970
vtables holding 80,317 functions.

About 1,170 classes build a heap allocator of their own in place from an early slot (`SprjMemoryHeapAllocator`),
so a vtable a slot writes is not the class's description.

**The class tree.** A constructor calling its parent's, and slots two vtables share, give the parents of
3,456 classes. The largest family, 553 classes, stands on a Dantelion2 root object that releases itself
to its heap.

**Singletons and `this`.** FD4's singletons assert on their global and say their class's name, which
ties 157 globals to 141 classes. A function called on one, or called with a method's first argument,
may be a method of that class, and the linker keeps a file's functions together, so neighbours follow.
These are placements, not understanding. A first rule, on one call, was wrong for four of seven functions
read by hand: a singleton passed to a mutex or a container says nothing of what those are. With two
calls required, and a function's own messages winning, none of eight read was clearly wrong, about four
looked right and four could not be told from the code: small functions rarely show their class.

**Method registry.** A class object also holds a registry of named methods: code stores a function in an
invoker and calls the class object's `registerMethod` with the name. These are the functions event and AI
scripts call, 995 of them, each with its class (`SprjLuaEventProxy::SetEventFlagValue`,
`SprjAiFunc::IsApparentDeath`). The name is the game's, so these are the most reliable names the
executable gives.

A thin method's one real callee does what the method's name says (of three read, all did), which names
the code behind them; a step's name says nothing of its callee, so steps do not count.

**Slots that mean the same everywhere.** A class's first slot returns its class object (1,414). A class
object's slots give its name, label and type tags, destroy an instance and return its size, so the size of
1,256 classes is known (`EnemyIns` 1,024 bytes, `TexRepositoryImp` 216). A virtual function that writes
its own vtable to the object is its destructor; one writing to its second
argument is a clone. Of six sampled with that check, the five that could be read were right.

**Assertions.** YEBIS passes its failed text and the function's name to the C library's assert, which gives 673
exact names (`GPURenderTexture::SetMultisampleBufferMode`). It is middleware, but it runs the post-effects.

**Step tables.** FD4's state machines fill a table in code with each step's function and its whole name
in wide text (`SprjGraphicsStep::STEP_Update`), which gives 742 exact names.

**Recipes the compiler repeats.** Grouping functions by their bytes, with addresses masked, shows template
code. A deleting destructor that calls another function names that function the destructor, which with the
rule above gives 5,630 deleting destructors and 5,003 others. 1,018 tables are `std::function` implementations (192 of their call slots are `Owner::lambda_`, from the only function outside the table that builds it): copy, move, the call,
the target's type, delete-this. A function that writes a class object's vtable is its getter (819), and
1,148 functions belong to the method invokers, one template per signature (164). Their slots were read;
what a `_Do_call` calls is still what says what such a function does.

One read example also names a template's copies, which compile to the same bytes but for addresses
(vector `push_back`, tree erase), and short functions that test a pointer-to-member's low bit and call it
are delegates, in FromSoftware's code only: a Havok function of that form is a forwarder.

Two tables of 162 entries are the opcode handlers of EzState's expression evaluator, whose stack of tagged
values (integer, float, object) sits at `+0x480`; `EzStateEventImpl` runs each event argument through it.
Opcodes 0 to 127 push their number less 64, in all 256 handlers, and those from 128 are constants of
other sizes, a variable load, calls to the environment's functions (133 to 139 taken to add one argument each
from their sizes, only 132 was read), arithmetic and comparisons. One table also writes each value out as text.

**Object files.** The executable lists its static initialisers in one table of 18,444. 12,045 of them are
the three standard error categories, initialised again in each of 4,015 source files that include them,
so each set marks an object file and what lies between two sets is one source file: of 195 stretches
that name a `.cpp`, 180 name exactly one. FromSoftware's code with its standard library starts at the
first set, offset `0xc83ab0`. Where every class-named function of a stretch has one class, the others belong
with it, which agrees 98.4% when held out and places 8,255 functions; taking the class of most of them
agrees 91.6% and is not used. A stretch that names one `.cpp` takes that file's name, whatever classes it holds
(7,242 functions).

**How far placements go.** The tests above measure each rule. Vtable methods agree with a class-named
source file in 561 of 587 functions. Functions between two of one class agree in about 97% when held
out, for runs of up to 64. A function placed with its callers is the weakest: 47 of 207 agree, though a
file can hold several classes. So being placed is not being identified, and only the first kind says
what a function is.

**What was read first.** The most-called functions give the most per function read. Dantelion2's heaps
are objects whose vtable allocates at `+0x58` and frees at `+0x70`, which makes much of the code readable.

**Public source as a reference.** Names can come from public source. zlib's `inflate` (1.2.7, and a
second copy) is known by its messages. The script VM is Havok Script (`hksObject.cpp`, `ksTable`), built
around Lua and with a `Lua 5.0.2` string, not plain Lua or LuaPlus: 62 of its functions hold messages of
Lua 5.0.2's and take those names as guesses.

The annotations gives the game's functions names, signatures and the layouts of the objects they use, the
render functions of the engine first: the native renderer's the render boundary takes the engine's draws at that
boundary instead of decoding the PS4's command buffers.

**Progress.** Each bar has 20 blocks, one per 5% of the step's items; the annotations as a whole is the mean
of its steps (100%).

| Step | Progress | Items |
|---|---|---|
| The annotations.1 · the annotation file | 100% `████████████████████` | the file (`annotations/functions.tsv`); the generator writes its names into the C; the app's kit carries it |
| The annotations.2 · the render boundary | 100% `████████████████████` | done: CoreGraphics2's draw and dispatch methods, the state blocks they apply, the resource binder's steps, the render-target passes, the counts in the game, the compute passes |
| The annotations.3 · the objects above it | 100% `████████████████████` | done: the draw state, the shader stages and their resources, the shader object, models and meshes, materials and shader programs, the materials' textures and constants, deferred lighting, the upload-ring draw's callers |
| The annotations.4 · synchronization primitives | 100% `████████████████████` | done: Dantelion2's synchronization classes named, the middleware's locks named, the atomic instructions inventoried, the locks counted in the game |
| The annotations.5 · the hottest signatures | 100% `████████████████████` | the 20 busiest functions (the CPU profile in "Where the CPU time goes") named with their signatures |

`annotations/structs.h` describes the objects those functions use, as far as they are known (C, with
the offsets checked by `_Static_assert`). `annotations/functions.tsv` holds one row per named function: its offset in the 1.09
executable, the project's name for it (`Namespace::Class::method`, `#variant` for a second copy
of one method), `sure` or `guess`, the evidence, and the signature in the project's types (the
arguments in register order). `Gnm::` is Sony's libGnm, linked into the game; `CG::` is
Dantelion2's CoreGraphics2, whose classes use a `CG` prefix. Evidence: `pm4` (a libGnm function,
named from the packets it writes, as `gpu/shim/bbport_gnm_table.inc` in bloodborne-runtime does),
`calls` (the functions it calls and how its arguments reach them), `source` (a source path in its
messages names the class), `message` (an error text, a profiler label or a singleton's name in it
names the function), `assert` (an assertion says its name), `binding` (registered in its class under that name), `step` (a state machine's table holds its name). `DL::` is Dantelion2's core library, `FD4::` FromSoftware's engine, `FX::`
the visual effects (a guess); `hk::`, `FMOD::`, `Scaleform::` and `Lua::` the middleware. Class names are the project's unless the evidence is `source`. The
names are the project's work, not the game's: no decompiled code goes into the file or its
evidence.

`bbrecomp` reads the file named by `BB_RECOMP_ANNOTATIONS` (`recomp.sh` and `build_library.py` set
it; the app's kit carries the file) and writes each name after the function's offset and at every
call of it, `?` after a guess. The names change comments only: the full library's C, comments and
spaces removed, is the same with and without them. `build_library.py` generates the C again when
the file changes.

So far, 398 functions: first libGnm's command buffer methods (83), the engine's render boundary and
what is above it (73), the synchronization primitives (28) and the busiest functions (20), then what the
mapping below read by hand: hot and much-called functions and the texture repositories.

**The render boundary.** Every draw of CoreGraphics2 goes through seven methods of its graphics
context: `draw`, `drawInstanced`, `drawIndexed`, `drawIndexedInstanced`, `drawIndirect`,
`drawIndexedIndirect` and `dispatch`. Each takes the draw's whole state (`CgDrawState`, below) and:

1. applies three state blocks from it, each sending libGnm only what changed since the last
   draw: input assembly (topology, index buffer and format), output merger (blend for eight
   targets, depth and stencil) and rasterizer (viewport, scissor, culling, polygon offset);
2. runs the resource binder's step before the draw: resource synchronization (CoreGraphics2's
   `CGResourceSyncContext`), then the shader resources (one 64 KB function);
3. calls libGnm's draw, then the binder's step after it.

Render-target operations draw full-screen passes of their own: MSAA resolve, fast-clear
elimination, depth decompression and depth-stencil clears. Outside CoreGraphics2, Scaleform and
a library reached through a vtable (labelled Havok) call libGnm's draws themselves.

Counted in the game (bloodborne-runtime's `BB_DRAW_CENSUS=1`, 2026-10-28, a level at about 1,880
libGnm draws a frame): 85% of the draws go through the seven methods, 2% through the render-target
passes, 7% are CoreGraphics2's compute passes (nearly every compute dispatch, on the same
context and resource binder: `CG::GraphicsContext::dispatch` itself did not run), and 6% are
Scaleform's renderer and one small wrapper outside CoreGraphics2. The breakdown is in
bloodborne-runtime's NATIVE_RENDERER.md.

**Event conditions.** 40 classes of the event system write their own name on their debug page, `Condition:EndFlow` and so on, so each is `SprjEventCondition_` plus that name, with its test, description and raise slots.

**The Direct3D 11 layer.** YEBIS brings its own Direct3D 11 implementation over Gnm, reached through `ppfxPlatformContextGnmShim`: 1,289 functions labelled `d3d11`. Its other callers are a few SDK and Havok functions; CoreGraphics2, the game's renderer, does not draw through it, and none of it is on the busy list. Its device, device context, resources, views, states and shaders keep Microsoft's published vtable order, so their slots have public names: `Map` on the context, `D3D11Device::CreateTexture2D` and so on, 333 in all, from one table in `names.py` (rule 23). The context's slots 115 to 128 go beyond the published interface and are not named yet.

**The draw state** (`annotations/structs.h`, the annotations.3): one object carries everything a draw sets,
0x1e70 bytes, modelled on Direct3D 11: its stages and slot counts, DXGI index formats,
and a topology in Direct3D 11's order without its gaps. Known: the topology, the index
buffer, format and offset; 32 vertex buffers with their strides and offsets; the
rasterizer state, 16 viewports and 16 scissors; the blend and depth-stencil states; the eight
colour targets and the depth target; the blend colour, depth bounds and stencil reference; five
graphics stages of 0x4f8 bytes from 0x598, in Direct3D 11's order (vertex, hull, domain, geometry,
pixel: the pixel stage's libGnm calls name GCN stage 1, the geometry's 3, the hull's 5; compute
has a state of its own), each 16 samplers, 128 textures and 14 constant buffers (Direct3D 11's
counts but for the textures), then its shader. Extra viewports are set only with a geometry
shader, the one stage that can choose a viewport. The resource binder copies the samplers' S#s and the textures' T#s that changed into its
own descriptor tables in GPU memory it allocates, makes V#s of the constant buffers and points
each stage's user data at them. The
state objects it points to hold libGnm's register values. When a colour or depth target changes,
the output merger first resolves the old one's fast clear or decompresses it: most of the
render-target passes come from there.

A stage's shader object holds the code's GPU address (+0x108) and the PS4 shader's own header
(+0x100, the code's size in its first word): the same code the runtime's shader recompiler reads
from the command buffers, so the draw-state work's shaders ahead of time can key on it at the boundary.

The graphics context holds a draw state of its own (+0xb730), which its immediate draws fill:
vertices (and indices) copied into GPU memory from an upload ring, as Direct3D 9's
`DrawPrimitiveUP` did.

**Meshes.** Counted in the game (`BB_DRAW_CENSUS=1` tallies who calls the seven methods; of the
calls at the 16 busiest call sites), 65% come from one function, `CG::MeshRenderer::drawMesh`: for each face set of a mesh (an
index range with its topology, index format and culling mode) it fills the draw state with the
mesh's vertex buffers (stride, offset, base vertex), the input layout for the pass, the
rasterizer state for the culling mode, depth-stencil state and stencil reference, then draws it
indexed, or instanced for more than one instance. With tessellation on, the topology becomes
patches of 3 control points. Before drawing it hands the pass object the skeleton's matrices of
this frame and the last. `structs.h` has the face set, the model's vertex buffer and the mesh.

**Models and materials.** `drawMesh`'s one caller, `CG::ModelRenderer::drawModel`, draws a model
in a pass: for each mesh whose flags and level of detail match the pass, it looks up the mesh's
material, and when the material changes ends the last one and begins the new one with the
mesh's technique (virtual methods of the material: this is where the shaders and textures are
set), then draws the mesh for each instance (each instance sets its own data first) or once,
instanced.

In the game every mesh draw used one material class (`BB_DRAW_CENSUS=1` tallies the materials
`drawMesh` gets). Its `begin` (`FD4::Material::begin`, the class's name a guess) applies the
material: its effect (+0x80) picks a shader program for the technique and the pass type (so one
material has its own shaders for each kind of pass), `CG::ShaderProgram::bind` sets the five
stages' shaders through the context's setters, and the material's parameter block (+0x88) binds
its constants and textures; it also sets the depth-stencil state.

**Lights.** The next callers of the draw methods (18%, and 5.9% from a function of the same size,
one of three variants) are deferred lighting: lights in batches of up to 12, each batch's depth
range projected into the draw state's depth bounds, each light's constants written into constant
buffers, an optional stencil pass without a pixel shader, then a full-screen triangle strip.

The caller with 7.5% draws what was last written into an upload ring (a topology, vertices and
16-bit indices), for 8 callers that are virtual methods of one part of the engine; it sets the
draw state through the context's setters (`setTopology`, `setVertexBuffers`, `setIndexBuffer`, as
the five shader stages have theirs).

**A material's textures and constants.** The material's texture list (+0xb0, `CgMaterialTexture`,
0x38 bytes each) binds each texture into its pixel-stage slot through
`CG::GraphicsContext::setPixelTextures`, and into the domain stage too for some kinds (maps the
tessellation reads), and gathers their UV transforms into a constant buffer bound to the same
stages. Its parameter block (+0x88) holds a list of parameter objects (seven kinds counted in
the game with `BB_DRAW_CENSUS=1`), each writing float4 registers into the material's constant
buffer (`FD4::ConstantBuffer::setVectors`), which is then bound; an instance's overrides go into
a copy, and some parameters are looked up by name (such as `ExParam_SoftVolume`).

**Geometry written each frame.** The upload-ring draw's 8 callers are one method each of eight
renderer classes with consecutive vtables: each maps space in the upload ring, writes its
vertices on the CPU and draws them; one of them draws an instance's points, transformed by its
matrix, as lines. They are most likely the visual effects' particles, lines and ribbons (the
names, `FX::PrimitiveRenderer`, are a guess).

**Compute passes.** CoreGraphics2's own dispatches are memory operations of its `SurfaceOps`, each
with a compute shader of the engine's and the resource binder's compute steps: buffer copies (one,
or a batch split into 64-dword jobs), buffer fills with a 16-byte pattern, texture clears and
texture region copies (1D, 2D and 3D shaders).

**Synchronization (the annotations.4).** Dantelion2's synchronization classes are named from their own source
files: `DLLightMutex`, `PthreadMutex`, `DLReadWriteLock`, `DLSemaphore` and `DLConditionSignal`
(lock, unlock, wait, signal, create and destroy), and so are the middleware's locks (Havok's
critical section, FMOD's, Scaleform's mutex). The executable has 12,734 atomic instructions
(`lock`-prefixed, or `xchg` with memory) in 4,594 functions: the game's code 5,441 in 1,224,
Havok 4,286 in 1,921, Scaleform 993, FD4 697, Dantelion2 590, FMOD 12. Counted in the game
(`BB_DRAW_CENSUS=1`, a level, about 35 FPS), `DL::LightMutex::lock` is taken 158,700 times a
second, the read lock 13,800, condition signals 5,300, semaphore waits 1,800, FMOD's lock 1,400,
Scaleform's 71, Havok's 35. These are what the generator's memory ordering has to respect when it
relaxes everything else.

**The busiest functions (the annotations.5).** The 20 busiest functions in the CPU profile (the main
thread in the Hunter's Dream) are named with their signatures: the job system's main loop
(it runs jobs and waits for the workers), Lua's interpreter (`luaV_execute`), the game's
per-frame world update (it touches 27 of the game's managers), the skeleton's bone matrices
(Havok's transforms made 3×4 matrices, then world matrices, with Havok's matrix functions), a
model's bounds and its skinning buffer (a ring of matrix sets, which gives `drawMesh` this
frame's and the last), the scene's traversal for a pass, the resource commit, the
constant-buffer upload, a culling test, the event-flag lookup, Havok's cloth mesh deformation,
the C library's block copy, Havok's animation sampling (it blends a character's animation
layers into a pose), a model instance's submission to the scene, and a priority choice in a typed
list; several class names are guesses (the confidence column says which).

The annotations's steps are done. Next, with the annotations's names: the native renderer's the render boundary (bloodborne-runtime's
NATIVE_RENDERER.md) and the generator's CPU work (the app's roadmap).

## Record and replay

With `BB_RECORD`, the translator leaves the chosen functions' first instruction to the
interpreter, which runs each recorded call in a tracer (bbcpu's `src/cpu/record.c`): one instruction at a
time, reading the memory each one accesses before and after it runs. The record holds the
registers at entry, the memory the function read (the first time in each epoch), the memory it
wrote, and the calls it made with their registers. Calls run at full speed; after each one the
memory the function reads is recorded again, so what the callee changed is in the record.

`tools/recomp/replay.sh` replays each record in a child process: the game image is mapped where it
was, memory is rebuilt from the record, and the function runs in the interpreter. Its calls are not
run: each must match the recorded target and argument registers, and is answered with the
recorded registers and memory. The registers at the return and every byte written must match.

Replaying the original function checks the record; generated code is checked the same way.
A record is flagged when another thread changed memory the function read during the call; such
calls (locks, job queues) cannot be replayed alone. The game's code must only touch guest memory
for a record to be replayable: the variables the game imports (the stack canary) live in guest
memory for that reason.

Recording costs no visible slowdown; the job loop (`0x21b8710`) is the usual exception to exact
replays, since other threads write what it reads.


## What the image contains

162,959 functions with unwind information (median 80 bytes), about 10 million instructions;
179,698 functions in all with those found through calls, jumps and code pointers (about 10.7
million instructions). The executable segment also holds the read-only data, after the import stubs,
so `scan.c` takes no pointer into it for code. No
C++ RTTI. About 500 source paths in assertion and log messages name the libraries
(`tools/recomp/label.py`, 2026-11-02, all 179,698 functions):

| Library | Functions | Code | Known by |
|---|---:|---:|---|
| Game (`SPRJ\Source\SPRJ\Source\Game`, `Sys`) | ~37,000 | 7.8 MiB | source paths |
| Dantelion2 (FromSoftware's core library) | ~34,900 | 7.4 MiB | source paths |
| Havok 2014.1 (physics, AI, cloth) | ~27,600 | 9.7 MiB | source paths, names |
| Scaleform GFx (menus, HUD) | ~20,400 | 4.2 MiB | messages |
| FD4 (FromSoftware's engine) | ~16,300 | 4.1 MiB | source paths, names |
| FMOD Ex 4.44.50 | ~7,200 | 1.4 MiB | source paths |
| Lua (LuaPlus) | ~2,700 | 1.0 MiB | source paths |
| YEBIS (post-processing) | ~2,300 | 0.8 MiB | source paths |
| zlib | 44 | 34 KiB | messages |
| Other paths | 454 | 0.1 MiB | source paths of no library above |
| Not labelled yet | ~30,800 | 5.7 MiB | |

The labels start from the paths and messages (5,742 functions) and spread from there: to the
functions between two of the same library (109,686), then to the other entries of a vtable
(22,411) and to functions whose callers or callees all belong to one library (11,019). Labelled
functions hold 86.5% of the code. Hiding a tenth of the labels and running the last step again
gives back 7,096 of 11,497, 96.4% of them the same: the steps agree with each other, but the
boundaries between game, FD4 and Dantelion2 stay approximate. The functions left are small
(median 49 bytes) and most have no direct caller (25,057).

## Where the CPU time goes

The game's main thread is spread thin in play: in 20 s in the Hunter's Dream 933 functions ran,
the busiest 145 took half the time, and the largest, the job loop, 7.3%. No small set of functions
dominates, so the CPU side gets faster by recompiling all of them (recompiling every function) and making that code cheaper
(the speed work). The frame rate on the Mac is bound by rendering, so changes are measured in CPU time
(`ps -o cputime=` over 30 s in the level), in alternating runs.

**Now:** the library takes 14% less CPU time than the translator at the same frame rate: 344% of
one core against 402% at the 1st Floor Sickroom lamp (about 1,283 draws a frame), 360% against
397% in Iosefka's Clinic (M3 Pro).

**What got it there,** each measured at the Sickroom against the translator:

| Change | Why it mattered | Effect |
|---|---|---|
| One flat table for finding recompiled functions | 16,384 chained buckets held about 15 functions each, and every indirect call walked them | 529–539% to 469–485% (Hunter's Dream) |
| C for the instructions left to the interpreter | 11.8 million a second, 98% of them about 20 mnemonics (`vpshufb`, `tzcnt`, `idiv`, ...); 36,000 a second are left | the library level with the translator |
| Vector registers in locals | x86 ordering's barriers made clang store and reload vector registers around every access (FMOD's mixer: 90% of a core against 45% translated) | 409% to 381% |
| `rc_call` without thread-local storage | the per-call filter and counter read TLS; now a leaf with the slow paths out of line | about 4 points |
| Copying only the registers that need it around calls | 39 copies a call became 23; code 26% smaller | 373% to 344% |
| Jump tables bounded across blocks (307 more, 3,663 in all); six more C library calls on the host (`strncmp`, `strchr`, `strstr`, `exp2f`, `wcschr`, `wcscmp`) | 69,600 calls a second into the translator became 40,300 (Iosefka's Clinic) | 342% to 339% |
| Jump tables with code between the `add` and the `jmp`, a copied table register, or the `lea` hoisted out of a loop (99 more, 3,762 in all; 155,000 instructions the generator had never reached, 1.4% of the code, now in C) | 40,300 calls a second into the translator became 30,300; the generator counts its tables and the indirect jumps left (9,231, tail calls through registers among them) | measured with the next change |

**Tried without a measurable gain,** left out: a cache at each indirect call site (it took 7.8 of
11.4 million calls a second off `rc_call`; what `rc_call` costs is its indirect jump); vector
accesses as two 8-byte `ldapr`/`stlr`; `ldapur`/`stlur` with their offsets (and the M1 lacks
them); plain accesses for a thread's private stack slots (push, pop, return addresses); plain
accesses, scalar and vector, to a function's whole frame when no address in it leaves the function
(13% of the library's accesses; 344% against 345% in three alternating pairs in Iosefka's Clinic).
Without any barrier on vector accesses (unsafe, measured only) the library took about 2% less CPU
time per frame: vector barriers are not where ordering costs either. Host functions taking only integers
(85% of the 2.6 million host calls a second: memcpy, ldiv, memset, the mutexes) called directly
instead of through the general bridge and its 528-byte argument block: 349% against 348% in
three alternating pairs.

**The ceiling of memory ordering:** a library with none (unsafe, not published) took 311% against
the translator's 407%, about 33 points under the ordered library. Those points are the ordering of
ordinary accesses. Neither the frame (no gain) nor the vector barriers (about 2%) hold them, which
leaves the scalar accesses to shared memory, where no analysis in the generator can tell which ones
another thread reads.

**Open:** the calls that still go into the translator (9,400 a second in Iosefka's Clinic, from 69,600 before
the jump-table rules and the host C library calls: nearly all the game's operator new and delete,
which run its own heap;
`BB_RECOMP_TARGETS=1` names them, and the host functions behind the import stubs: in Cathedral
Ward about 2.5 million calls a second, 85% of them to functions taking only integers: `memcpy`
560,000, `ldiv` 520,000, `memset` 215,000, the mutex lock and unlock 190,000 each, `thread_self`
157,000; then `powf`, `sinf`/`cosf` and `modff` with floats), and the library running about 0.5 FPS under the translator at
the Sickroom (38.0 against 38.6), not explained.
