# `agenc-test`

> Planned: a single-header test harness with a TEST macro, assertion macros that print both values on failure, a filtering runner, and a CI exit code.

Layer 2 of the family build order. Not implemented yet. The scope and
ordering live in [../LIBRARIES.md](../LIBRARIES.md); the conventions are
set by [`agenc-str`](../agenc-str/) and [`agenc-arena`](../agenc-arena/).
Deliberately dependency-free, not even on the arena, so it can test the
arena itself.
