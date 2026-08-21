# agenc-arena implementation plan

Library #1 of the family plan in /home/tetsuo/git/AgenC/C/LIBRARIES.md:
an arena (region) allocator with reset semantics plus the allocator
vtable every later agenc-* library allocates through.

Status 2026-08-20: implemented. Milestones M0 through M8 are complete
with every gate green under gcc 13.3 and clang 18.1; the evidence ledger
is VERIFICATION.md section 10. Three SPEC corrections were made during
implementation and are recorded in SPEC.md sections 3, 4.4, and 4.6
(per-block Valgrind mempools; invalid align as a defined ERR_ARG at the
arena boundary; the malloc attribute withheld from memdup and realloc).
M8's publish step happened at the family level: the library ships as a
standalone folder of the tetsuo-ai/agenc-c monorepo.

Companion documents in this folder:

- RESEARCH.md: the survey and literature findings this plan cites
  (F/D/E/P/H numbers refer to its sections).
- SPEC.md: the full API contract the implementation must satisfy.
- VERIFICATION.md: the test plan, build matrix, and evidence ledger.

House contract: agenc-str is the reference implementation for layout,
style, error model, and build discipline. C11, -pedantic-errors, the
agenc-str warning set, clang-format rules copied verbatim, MIT license,
a standalone folder in the agenc-c monorepo. Adoption is copying
include/alloc.h, include/arena.h, and src/arena.c.

## 1. Goals

- One allocator interface (alloc_t) with an exact, misuse-resistant
  contract, frozen on day one, that agenc-err through agenc-cli are
  written against.
- A fast, portable arena: chained blocks from a parent allocator,
  fixed-buffer backend, temp scopes, reset that retains memory, and an
  adapter presenting the arena as an alloc_t.
- The family's correctness posture: failure-atomic operations, sticky
  status, NULL-only failure signaling, no global state, no threads, no
  exit or abort in library code, hostile-input-grade size arithmetic.
- First-class checking builds: ASan poisoning, MSan marking, debug
  fills, optional Valgrind mempool annotation, misuse detection for temp
  scopes.

## 2. Non-goals

- Per-object free as a real operation (F5): xfree through the adapter is
  LIFO-rollback or no-op. Users needing individual free use the libc
  backend or a future pool in agenc-ds.
- Cleanup/destructor callbacks (D-cleanup), pool hierarchies (D-hier),
  thread-local scratch pools (D-scratch), reap-style hybrids: all
  excluded until missed three times.
- OS-specific backing (mmap, VirtualAlloc): that arrives later as an
  agenc-os parent allocator behind the same vtable, with no arena
  changes (F1).
- Zeroing promises in the vtable (P5) and any global default-allocator
  hook (P7).
- Beating mimalloc on a clean heap. The measured value is locality,
  fragmentation immunity, bulk free, and predictability (E1); the docs
  say so honestly.

## 3. Architecture

```
include/alloc.h    the interface: alloc_t, contract, inline call
                   helpers, alloc_libc(), alloc_null()
include/arena.h    arena_t, arena_temp_t, lifecycle, allocation,
                   temp scopes, arena_allocator(), queries, macros
src/arena.c        both implementations, one file
tests/test_arena.c suite (VERIFICATION.md)
tests/fuzz_arena.c libFuzzer op-sequence harness
Makefile, README.md, LICENSE, .clang-format
```

Memory shape (growing arena):

```
arena_t ── current ──> [block C hdr | ....########_____ ] newest
                          prev │            cursor ^
                               v
                       [block B hdr | ############### ] full
                          prev │
                               v
                       [block A hdr | #####          ] first, kept by reset
arena_t ── free_blocks ──> recycled normal blocks (reused before parent)
oversize blocks: on the chain, flagged, returned to parent on rewind/reset
```

Positions for temp scopes are (block, offset-in-block, total-used,
generation); rewind walks newer blocks off the chain to the free list.

## 4. Decision log

Each decision cites its evidence in RESEARCH.md.

D1 vtable shape: { ctx, xalloc, xrealloc, xfree }, alignment as a
size_t parameter on all three, callers repeat exact old size and align.
Evidence: P1, P2, P8; SQLite's header tax; C23 free_sized direction.
Alternatives rejected: Lua single-function (no alignment, NULL
overload), Odin mode enum (runtime dispatch in every backend), Zig
4-function resize probe (contradicts the 3+ctx family plan; door left
open via a future struct type, never mutation of this one).

D2 zero-size and NULL rules: alloc(0) = NULL-as-success, realloc-to-0
forbidden, realloc(NULL, 0, n) = alloc, free(NULL) = unconditional
no-op. Evidence: P3, P4 (WG14 N2464; Vulkan wording).

D3 failure signaling: NULL from the interface; NULL plus sticky status
on the arena; no abort, longjmp, or callback hooks anywhere. Evidence:
D-oom, F9 (obstack's global handler as anti-pattern); family rule that
library code never exits. Sticky gating matches agenc-str: chain, check
once, arena_clear_error to resume.

D4 memory layout: chained blocks, newest first, header {prev, cap,
used, flags}; block metadata otherwise lives in arena_t. Fixed-buffer
arena is the no-chain case of the same code path. Evidence: F1;
Fleury's NoChain + backing-buffer precedent.

D5 growth: min_block 4096 doubling to max_block 65536, both
configurable per arena; oversize requests get dedicated exact-size
blocks; every size capped at PTRDIFF_MAX with subtraction/division
guards. Evidence: F2, H3; Tofte retrospective (no universal page size,
so per-arena knobs).

D6 retention: reset keeps normal blocks (the oldest normal stays
current, the rest go to the per-arena free list) and returns oversize blocks; temp-end recycles
the blocks it vacates to the free list; arena_trim releases the free
list; no cross-arena cache ever. Evidence: F3, F9; APR/nginx/Hanson
retention practice; Hanson's global freechunks as the landmine.

D7 alignment: per-call size_t align, 0 = max_align_t, arena supports up
to 65536 internally; libc backend refuses above max_align_t (MSVC has
no aligned_alloc; header-free routing is impossible; over-aligned means
use an arena). Padding is computed in integer space and applied by
advancing the original pointer. Evidence: F4, H1, H4; N2293
weak-alignment libcs.

D8 fast path: test-before-bump with integer arithmetic only, never
forming a pointer before the capacity check passes. Evidence: E3 (the
published Hanson macro bug), H2 (ARR30-C, deleted checks).

D9 zeroing: functions return uninitialized memory with _zeroed
variants; typed ARENA_NEW macros zero by default. Evidence: D-zero;
Fleury's arrangement; ZII school vs systems-arena practice.

D10 realloc: last-allocation grow/shrink in place, otherwise
alloc+copy+abandon; full xrealloc contract compatibility so the arena
backs the vtable. Evidence: F6 (obstack growing object rejected);
protobuf #20565 for the sanitizer interaction.

D11 temp scopes: explicit arena_temp_begin/end value objects carrying
generation and depth for debug validation; no TLS scratch pool in the
library. Evidence: D-scratch; E2 (temp marks are the blowup
countermeasure); Odin temp_count precedent, extended with generations.

D12 checking builds: ASan poison-block/unpoison-exact/re-poison with
16-byte 8-aligned redzones, MSan allocated-memory marks, 0xA5/0x5A
debug fills, opt-in Valgrind mempool mapping (headers not vendored).
Evidence: F7, H6; no-third-party-deps rule.

D13 statistics: used, committed, high_water as cheap always-on
counters. Evidence: F8; ML Kit profiler lesson.

D14 two prefixes, one repo: alloc_ for the interface, arena_ for the
arena, following the ds_vec_/ds_map_/ds_list_ precedent. LIBRARIES.md
places the interface inside agenc-arena; a separate repo for four
typedefs is not worth the dependency edge.

D15 attributes: malloc plus alloc_size on the four bump entry points,
alloc_size alone on memdup and realloc (their results carry caller
bytes), no alloc_align (the align parameter may be 0 meaning default,
which the attribute does not model), returns_nonnull nowhere, all
behind GNU/Clang compiler detection. Evidence: H7 (GCC semantics
verified; APR precedent).

D16 sticky-status scope: a recorded failure gates later allocations
(agenc-str semantics) rather than only recording. Trade-off noted:
probe-and-fallback callers must arena_clear_error between attempts;
consistency across the family wins. The header documents the recovery
path.

## 5. Public API summary

See SPEC.md for the binding contract.

- alloc.h: alloc_t, alloc_libc, alloc_null, alloc_alloc, alloc_realloc,
  alloc_free, alloc_zeroed.
- arena.h: arena_init, arena_init_sized, arena_init_fixed, arena_deinit,
  arena_reset, arena_trim, arena_clear_error; arena_alloc,
  arena_alloc_zeroed, arena_alloc_n, arena_alloc_n_zeroed, arena_memdup,
  arena_realloc; arena_temp_begin, arena_temp_end; arena_allocator;
  arena_status, arena_status_name, arena_ok, arena_failed, arena_used,
  arena_committed, arena_high_water; ARENA_NEW, ARENA_NEW_N,
  ARENA_NEW_N_UNINIT.

## 6. Milestones

Each milestone ends with `make check` green under GCC and Clang and the
VERIFICATION.md ledger updated. No milestone starts before the previous
one's gate is met.

M0 scaffold: repo layout, LICENSE (MIT), .clang-format and Makefile
derived from agenc-str, empty suite that runs and fails on zero tests.
Gate: make check runs all variants.

M1 alloc.h: interface header with full contract comments, libc and null
backends, inline helpers with debug assertions. Tests: T6 contract
tables for both backends. Gate: T6 green; header compiles standalone
first in a TU.

M2 fixed arena core: arena_t, init/init_fixed/deinit, arena_alloc,
arena_alloc_n (+zeroed), memdup, queries, sticky status, integer-space
fast path. Tests: T1 (fixed cases), T3 zero-size table, T5 heap-disabled
run wired. Gate: T5 passes with zero parent calls.

M3 growing arena: block chain, growth policy, oversize blocks, free-list
retention, reset, trim. Tests: T1 growth/saturation, T4 OOM fail-Nth
loops, counting-allocator leak checks. Gate: T4 exhaustive over the
scenario table, no leaks.

M4 temp scopes and realloc: arena_temp_begin/end with generation and
depth checks, arena_realloc with last-allocation fast path. Tests: T2
suite including death tests, T3 realloc matrix, block-recycling flatness
loop. Gate: T2 and T3 green; committed stays flat across 1000 temp
cycles.

M5 adapter and conformance: arena_allocator, LIFO rollback xfree,
checked-arg parent proving the arena's own vtable calls are exact.
Tests: T6 adapter rows, T7. Gate: a representative workout runs green
under the libc, tracking, and arena-adapter parents, plus a dedicated
arena-on-arena test; the full suite runs once with mixed parents per
test.

M6 checking builds: ASan poisoning with redzones, MSan marks, debug
fills, Valgrind opt-in, ARENA_DISABLE_SANITIZER_HOOKS. Tests: T2 poison
probes, protobuf-#20565-shaped realloc regression, capacity notes.
Gate: asan target green with poisoning active; valgrind job clean.

M7 fuzz and hardening: fuzz_arena.c with shadow model, corpus seeds,
attribute macros (D15), -fanalyzer and clang --analyze triage. Gate: a
one-hour fuzz run with zero findings on both backends; analyzers triaged
to zero actionable findings.

M8 delivery: README.md in agenc-str style (quick start, programming
model, contract tables, terminology note distinguishing jemalloc
arenas), final ledger, git init + initial history if publishing is
requested. Gate: full ledger, README examples compiled as a demo target.

## 7. Risks and mitigations

- Fixed-buffer effective-type reliance (H5): documented stance plus a
  single internal launder point; risk accepted like every shipping
  fixed-buffer allocator.
- Sticky gating surprises probe-style callers (D16): documented recovery
  path; revisit only with three real complaints.
- ASan capacity divergence (redzones change how much fits per block):
  tests parameterize expected capacity by build; documented in SPEC 3.
- Free-list first-fit could fragment under mixed block sizes after cap
  changes: sizes are uniform-ish by construction (doubling to a cap);
  arena_trim is the escape hatch; revisit with evidence.
- Interface freeze risk (missing in-place-resize probe, Zig lesson):
  explicitly deferred; a v2 struct is the escape path, documented in the
  header (SPEC 1.7).

## 8. Done-when

- SPEC.md contracts implemented verbatim in the two headers, header
  comments carrying the full contract like str.h does.
- VERIFICATION.md ledger fully green (or explicitly unavailable rows)
  under GCC and Clang.
- The heap-disabled fixed-buffer run (T5) passes: the LIBRARIES.md
  completion criterion for this library.
- agenc-err (library #3) can be started against alloc.h without asking
  any interface question this plan leaves open.
