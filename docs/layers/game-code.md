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

10,861,735 of the 11,268,132 instructions are recompiled (96%), 99.1% of them in plain C, and
240,978 of 241,821 functions run recompiled. CPU time is 14% lower than with the translator at the
same frame rate.

The library is opt-in: a player who has not built it runs on the translator.

## What is left

- Making the library the default.
- The import stubs, and the code rewritten while the game runs.
- Building for other architectures: the build scripts target arm64 today.

Details: [Recompilation](https://github.com/bloodborne-native/bloodborne-recomp/blob/main/docs/RECOMPILATION.md).
