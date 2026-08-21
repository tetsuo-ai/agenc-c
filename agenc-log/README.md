# `agenc-log`

> Planned: leveled logging that writes to a caller-supplied sink function, a compile-time minimum level so release builds can drop debug logging, and one install call with no other global state.

Layer 7 of the family build order. Not implemented yet. The scope and
ordering live in [../LIBRARIES.md](../LIBRARIES.md); the conventions are
set by [`agenc-str`](../agenc-str/) and [`agenc-arena`](../agenc-arena/).
