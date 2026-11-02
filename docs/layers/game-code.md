# The game's code

[← The PS4 layers](README.md)

The game's executable is x86-64 code built for the PS4. To run anywhere else, that code has to be
turned into code for the machine it is on, or at least separated from the PS4 it expects.

## What we did

- **bbcpu**, a CPU layer of our own: an x86-64 decoder, an interpreter and an x86-64 to arm64
  translator. It is what runs the game by default.
- **A static recompiler** that writes the game's functions out as C, which a compiler turns into a
  native library. It runs on the player's device, from their own copy of the game. Nothing it
  produces is published.
- **Checks:** calls recorded during play are replayed against the generated code, and every
  instruction form the generator handles is fuzzed against the interpreter.
- **Built by the launcher** with a downloaded compiler, with no Xcode, in about 14 minutes on an M3
  Pro. Later updates compile only what changed.
- **Fewer calls back to the translator:** 69,600 a second down to 9,400 in Iosefka's Clinic.

## Where it stands

10,284,725 of the 10,691,122 instructions are recompiled (96%), about 99% of them in plain C, and
181,521 of 181,570 functions run recompiled. CPU time is 14% lower than with the translator at the
same frame rate.

The library is opt-in: a player who has not built it runs on the translator.

What the code does is known for part of it: about 25,600 of the about 58,000 functions to understand have a
name or a known role, much of that compiler and template code, and 47,111 are placed in a class or a source
file ([Annotations](../RECOMPILATION.md#annotations)).

## What is left

- Real gains for players: the library stays opt-in until it has them.
- The import stubs, and the code rewritten while the game runs.
- Playing on other architectures: the recompiler builds for x86-64, tested under Rosetta, and the
  runtime is what is left. The launcher's toolchain is macOS arm64.

Details: [Recompilation](https://github.com/bloodborne-native/bloodborne-recomp/blob/main/docs/RECOMPILATION.md).
