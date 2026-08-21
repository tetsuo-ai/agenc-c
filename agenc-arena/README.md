# `arena`

> A region allocator with reset semantics for C11, plus the allocator
> interface the agenc-* library family is built on.

![C11](https://img.shields.io/badge/C-C11-00599C?logo=c&logoColor=white)
![Third-party dependencies: none](https://img.shields.io/badge/third--party_dependencies-none-2ea44f)
![Checked with ASan and UBSan](https://img.shields.io/badge/checked-ASan_%7C_UBSan_%7C_Valgrind_%7C_fuzzed-8a2be2)
![License: MIT](https://img.shields.io/badge/license-MIT-yellow.svg)

`arena` groups allocations by lifetime instead of tracking them one by
one: allocate freely, then release everything at once with a temp scope,
a reset, or deinit. Allocation is a bounds-checked pointer bump. Backing
memory comes from a pluggable parent allocator (`alloc_t`), from libc by
default, or from a caller-supplied fixed buffer for heap-free use.

Terminology note: this is an arena in the region/bump sense (Hanson
1990). jemalloc and glibc use the word "arena" for a sharded
general-purpose heap, which is unrelated.

## Why `arena`?

| Design choice | What it gives you |
| --- | --- |
| Lifetime-batched allocation | One temp end, reset, or deinit frees everything; no per-object bookkeeping, headers, or free calls. |
| Pluggable parent allocator | A four-word `alloc_t` vtable with an exact contract: alignment on every call, exact sized deallocation, defined zero-size rules. |
| Fixed-buffer backend | The same API serving from a static or stack buffer; the test suite runs with the heap disabled. |
| Failure-atomic, sticky errors | A failed call changes nothing but the recorded status; chain allocations and check once. |
| Overflow-guarded arithmetic | Every size is capped at `PTRDIFF_MAX`; count and alignment math is checked before any pointer forms. |
| Checking builds | ASan poisoning with redzones, MSan marks, Valgrind mempool annotation, debug fill patterns, temp-scope misuse detection. |

There are no third-party dependencies. The implementation uses C11 and
the standard C library.

## Quick start

Copy [`include/alloc.h`](include/alloc.h),
[`include/arena.h`](include/arena.h), and [`src/arena.c`](src/arena.c)
into your project, then compile them with your application:

```sh
cc -std=c11 -Wall -Wextra -Iinclude app.c src/arena.c -o app
```

A complete program:

```c
#include <stdio.h>

#include "arena.h"

int main(void)
{
    arena_t a;
    char *text;

    arena_init(&a, alloc_libc());
    text = arena_memdup(&a, "hello, arena", 13);
    if (arena_failed(&a)) {
        fprintf(stderr, "%s\n", arena_status_name(arena_status(&a)));
        arena_deinit(&a);
        return 1;
    }
    puts(text);
    arena_deinit(&a); /* frees text and everything else at once */
    return 0;
}
```

Run `make demo` for a printable walkthrough of growing arenas, temp
scopes, fixed buffers, and the allocator adapter.

## The programming model

### Allocate in bulk, free in bulk

An arena obtains blocks from its parent allocator (4KB doubling to 64KB
by default, configurable with `arena_init_sized`) and serves allocations
by bumping a cursor. Requests larger than the block size get their own
exactly-sized block. Individual allocations are never freed; lifetime
belongs to the arena:

```c
arena_t a;
arena_init(&a, alloc_libc());

struct node *n = ARENA_NEW(&a, struct node);      /* zeroed, aligned */
double *xs = ARENA_NEW_N(&a, double, 1024);
char *copy = arena_memdup(&a, src, len);

arena_reset(&a);   /* everything above is gone; blocks are retained */
arena_deinit(&a);  /* blocks go back to the parent */
```

### Scope temporary work with temp marks

Temp scopes are the tool that keeps arena memory bounded in loops and
recursive work. They nest, cost nothing to open, and rewind exactly:

```c
for (size_t i = 0; i < requests; i++) {
    arena_temp_t temp = arena_temp_begin(&a);
    handle_request(&a, &requests[i]);
    arena_temp_end(temp); /* per-iteration memory fully reclaimed */
}
```

Debug builds detect temps ended out of order or after a reset and abort
with a message. A callee that needs temporary memory should take a
scratch arena parameter distinct from the arena its results go to.

### Run without a heap

```c
static _Alignas(max_align_t) unsigned char buffer[1 << 16];
arena_t a;
arena_init_fixed(&a, buffer, sizeof(buffer));
```

The fixed backend never touches a parent allocator and fails with
`ARENA_ERR_ALLOC` when the buffer is exhausted. Up to
`ARENA_FIXED_OVERHEAD` bytes go to bookkeeping.

### Plug the arena into anything that takes an alloc_t

Every agenc-* library allocates through the `alloc_t` interface. The
libc backend is the default; `arena_allocator(&a)` presents an arena as
the same interface, so a whole data structure can live inside one arena
and vanish with one deinit. Freeing the most recent allocation through
the adapter rolls the cursor back, so LIFO alloc/free pairs cost zero
net memory.

### Chain errors, check once

The first failure records a sticky status; later allocation calls return
NULL without changing the arena until `arena_clear_error` or
`arena_reset`. Failed calls never disturb existing allocations, and a
failed realloc leaves the old block valid.

## API at a glance

| Area | Operations |
| --- | --- |
| Interface | `alloc_t`, `alloc_libc`, `alloc_null`, `alloc_alloc`, `alloc_realloc`, `alloc_free`, `alloc_zeroed` |
| Lifecycle | `arena_init`, `arena_init_sized`, `arena_init_fixed`, `arena_deinit`, `arena_reset`, `arena_trim`, `arena_clear_error` |
| Allocate | `arena_alloc`, `arena_alloc_zeroed`, `arena_alloc_n`, `arena_alloc_n_zeroed`, `arena_memdup`, `arena_realloc` |
| Typed macros | `ARENA_NEW`, `ARENA_NEW_N` (zeroing), `ARENA_NEW_N_UNINIT` |
| Temp scopes | `arena_temp_begin`, `arena_temp_end` |
| Adapter | `arena_allocator` |
| Observe | `arena_status`, `arena_status_name`, `arena_ok`, `arena_failed`, `arena_used`, `arena_committed`, `arena_high_water` |

The public headers are the complete API reference; every function
documents its inputs, ownership, invalidation rules, and failure modes
in [`include/alloc.h`](include/alloc.h) and
[`include/arena.h`](include/arena.h).

## Status values

| Status | Meaning |
| --- | --- |
| `ARENA_OK` | Success |
| `ARENA_ERR_ARG` | Invalid object, pointer, alignment, or other argument |
| `ARENA_ERR_ALLOC` | The parent allocator refused, or a fixed buffer is exhausted |
| `ARENA_ERR_OVERFLOW` | A size computation exceeds `PTRDIFF_MAX` or wraps |

## Important contracts

- Arenas fit batch-shaped lifetimes: per-request, per-frame, per-phase
  work. Producer-consumer queues, unbounded buffers, and long-lived
  objects with individual lifetimes belong on the libc backend instead;
  routing them through an arena grows memory without bound.
- One arena, one owner. Independent arenas may be used concurrently;
  sharing one across threads requires external synchronization for every
  operation, including temp scopes and the adapter.
- Pointers into an arena die at the enclosing temp end, reset, or
  deinit. Checking builds poison rewound memory so stale pointers fail
  loudly.
- Under AddressSanitizer each allocation carries a poisoned redzone
  (`ARENA_ASAN_REDZONE` bytes), so less fits per block than in plain
  builds. Capacity-sensitive code should not assume exact block layouts.
- Do not keep long-lived secrets in a general arena: reset reuse is
  type-unsafe by construction, like every region allocator.

## Build and verify

Requirements: a C11 compiler and `make`. clang-format, clang (for the
fuzzer), and valgrind are needed for the corresponding targets.

| Command | Purpose |
| --- | --- |
| `make demo` | Build and run the public-API walkthrough |
| `make test` | Run the suite with strict warnings and assertions |
| `make asan` | Run the suite under ASan and UBSan with poisoning active |
| `make iso` | Run the strict-ISO variant (fixed-buffer launder barrier) |
| `make release` | Run an optimized `NDEBUG`/FORTIFY build |
| `make heapless` | Run the fixed-buffer suite with no heap use |
| `make fuzz` | Build and run the libFuzzer op-sequence harness |
| `make valgrind` | Run the suite under memcheck with mempool annotations |
| `make format` | Check the repository's clang-format rules |
| `make check` | Format, demo build, test, asan, iso, release, heapless |

The test suite covers allocation-failure injection loops over every
scenario, temp-scope misuse death tests, overlap and content-survival
properties, overflow boundaries, alignment matrices including a
deliberately misaligning parent, and the sanitizer poisoning contract
itself.

## License

Licensed under the [MIT License](LICENSE). Copyright (c) 2026 TETSUO.AI INC.
