# `agenc-ds`

> Planned: exactly three containers: a typed dynamic array, an open-addressing hashmap keyed on slices, and an intrusive doubly linked list.

Layer 6 of the family build order. Not implemented yet. The scope and
ordering live in [../LIBRARIES.md](../LIBRARIES.md); the conventions are
set by [`agenc-str`](../agenc-str/) and [`agenc-arena`](../agenc-arena/).
Depends on agenc-arena, agenc-hash, and agenc-err.
