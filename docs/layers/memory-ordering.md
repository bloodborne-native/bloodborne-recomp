# x86 memory ordering

[← The PS4 layers](README.md)

x86 keeps a strict order for how one core's reads and writes become visible to the others. arm64
does not, and the game publishes data between threads with plain stores, which relies on x86's order.

## What we did

Every guest load acquires and every store releases, in both the translator and the generated code.
Without it, data published between threads goes missing: glyphs rendered by a worker did.

## Where it stands

0%: the ordering is kept on every access, which is correct and expensive. Only arm64 needs this
layer. x86-64 keeps this order in hardware.

## What is left

Keeping ordering only around the engine's synchronization. That needs the generator's analysis, a
safety case and a long play session, because a mistake here shows up as rare corruption.
