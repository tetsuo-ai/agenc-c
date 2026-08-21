# `agenc-c`

> Drop-in C11 libraries for the AgenC ecosystem: one standalone folder
> per library, no third-party dependencies, one shared quality gate.

![C11](https://img.shields.io/badge/C-C11-00599C?logo=c&logoColor=white)
![Third-party dependencies: none](https://img.shields.io/badge/third--party_dependencies-none-2ea44f)
![License: MIT](https://img.shields.io/badge/license-MIT-yellow.svg)

This is the source of truth for the family. Every library is a fully
standalone folder: its own header, source, tests, Makefile, README, and
license. Adoption never changes: copy the headers and the one .c file
into your project. The monorepo exists so the conventions live in one
place, a family-wide change is one commit, and one `make check` proves
everything.

## Libraries

Build order is dependency order: each library may depend only on the
layers above it. The full plan is [LIBRARIES.md](LIBRARIES.md).

| Library | Layer | Status | What it is |
| --- | --- | --- | --- |
| [agenc-str](agenc-str/) | 0 | done | Predictable growable strings and views; the reference implementation for the family's style |
| [agenc-arena](agenc-arena/) | 1 | done | Region allocator with reset semantics, plus the `alloc_t` interface every later library allocates through |
| [agenc-test](agenc-test/) | 2 | planned | Single-header test harness |
| [agenc-err](agenc-err/) | 3 | planned | Small error type with a stable numeric code space |
| [agenc-bin](agenc-bin/) | 4 | planned | Bounds-checked binary reader and writer for hostile input |
| [agenc-hash](agenc-hash/) | 5 | planned | Fast hash, SipHash for untrusted keys, PCG-style PRNG |
| [agenc-ds](agenc-ds/) | 6 | planned | Dynamic array, slice-keyed hashmap, intrusive list |
| [agenc-log](agenc-log/) | 7 | planned | Leveled logging to a caller-supplied sink |
| [agenc-os](agenc-os/) | 8 | planned | Platform shim: files, paths, clocks, threads |
| [agenc-cli](agenc-cli/) | 9 | planned | Declarative flag parser and fail-closed JSON-subset reader |

## Conventions

Set by agenc-str and agenc-arena, binding for every library:

- C11, no third-party dependencies, hosted standard library only.
- All allocation goes through the `alloc_t` interface from agenc-arena;
  no hidden malloc. (agenc-str predates the interface and manages its
  own heap buffers; the rule binds every library from agenc-arena up.)
- Fallible calls return a `<name>_status_t` enum; stateful objects carry
  a sticky status; mutation is failure-atomic; library code never calls
  exit or abort (asserts detect contract violations in debug builds and
  compile out of release builds).
- All inputs are pointer plus length; parsers assume hostile input; all
  size arithmetic is overflow-checked and capped at `PTRDIFF_MAX`.
- No global mutable state, no threads spawned, no signal handlers.
- Each library ships tests in four build variants (plain, ASan and
  UBSan, strict ISO, release with NDEBUG and FORTIFY) and, where it
  parses input, a fuzz target.
- The root `LICENSE` and `.clang-format` are canonical; every library
  carries a byte-identical copy so its folder stays standalone, and
  `make conventions` enforces the match.

## Build and verify

```sh
make check    # conventions gate, then every library's full make check
```

Requirements: a C11 compiler, make, and clang-format. Individual
libraries build and test on their own with `make -C <library> check`.

## License

Every library is licensed under the [MIT License](LICENSE).
Copyright (c) 2026 TETSUO.AI INC.
