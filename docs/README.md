# Documentation

[← Back to the project](../README.md)

- [Recompilation](RECOMPILATION.md): the plan, the milestones, how the recompiler works, its tools,
  record and replay, what the game's executable contains, and the results so far.
- [The PS4 layers](layers/README.md): the game's code and x86 memory ordering: what was done and what
  is left.
- [Annotations](RECOMPILATION.md#annotations): the project's names for the game's functions
  ([`annotations/functions.tsv`](../annotations/functions.tsv)) and the layouts of the objects they
  use ([`annotations/structs.h`](../annotations/structs.h)): what is known of the engine's render code.
- [bbcpu](https://github.com/bloodborne-native/bbcpu): the x86-64 to arm64 translator and
  interpreter the recompiled code stands on, a project of its own.
- [Credits and licenses](CREDITS.md)
- Progress: the wiki's [Recompilation status](https://github.com/bloodborne-native/bloodborne-recomp/wiki/Recompilation-status).

The game side (loading the recompiled library, recording calls in the game) is
[bloodborne-runtime](https://github.com/bloodborne-native/bloodborne-runtime); the macOS app is
[bloodborne-mac](https://github.com/bloodborne-native/bloodborne-mac).
