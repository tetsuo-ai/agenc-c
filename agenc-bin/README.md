# `agenc-bin`

> Planned: a bounds-checked cursor over byte slices: typed little/big-endian reads, peek, skip, and align, all failing softly without ever touching memory past the end, with a mirrored writer.

Layer 4 of the family build order. Not implemented yet. The scope and
ordering live in [../LIBRARIES.md](../LIBRARIES.md); the conventions are
set by [`agenc-str`](../agenc-str/) and [`agenc-arena`](../agenc-arena/).
Consumes the agenc-str view type and ships the family's fuzz-entry-point
convention, so every parser built on it gets a fuzz target almost free.
