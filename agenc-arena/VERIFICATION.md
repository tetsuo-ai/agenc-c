# agenc-arena verification plan

Verification is evidence. Every claim of done comes with the ledger in
section 8 filled in; a skipped check is reported as skipped, never as
passing. The bug-class ordering follows RESEARCH.md section 6 and the
incident history (APR CVE-2009-2412, protobuf #20565, the Hanson/Briggs
fast-path bug).

## 1. Build matrix

Inherited from agenc-str and extended. All targets run the same test
binary source.

| Target | Configuration | Purpose |
| --- | --- | --- |
| make test | -std=c11 -pedantic-errors, strict warnings, asserts on | primary suite |
| make asan | -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all | memory and UB evidence |
| make iso | strictest ISO-conformance variant, asserts on | portability posture |
| make release | -O3 -DNDEBUG -D_FORTIFY_SOURCE=3 | production optimization and assert-off behavior |
| make fuzz | clang -fsanitize=fuzzer,address,undefined | op-sequence fuzzing (section 5) |
| make valgrind | plain build under valgrind with ARENA_ENABLE_VALGRIND | mempool-annotated memcheck (optional job, requires valgrind) |

Warning set (from agenc-str, binding):
`-Wall -Wextra -Werror -Wconversion -Wshadow -Wformat=2
-Wno-format-nonliteral -Wnull-dereference -Wstrict-prototypes
-Wmissing-prototypes`.

UBSan notes: the asan target includes `-fsanitize=undefined`, which
covers pointer-overflow; that check specifically catches cursor
arithmetic mistakes of the `cur + n > end` shape.

`make check` = format + test + asan + iso + release. Fuzz and valgrind
run on demand and before release tags.

Both GCC and Clang should pass the full matrix; CI or a local script runs
the matrix once per compiler (`CC=gcc`, `CC=clang`).

## 2. Test suite structure

`tests/test_arena.c`, agenc-str harness style: single EXPECT macro that
prints file:line and aborts, named constants in an enum, deterministic
cases first, seeded randomized cases second with the seed printed on
failure.

The parent-allocator vtable is the fault-injection seam. The suite
defines test allocators in plain code, no library test hooks needed:

- counting allocator: wraps alloc_libc, counts calls, records live
  bytes, detects leaks and double frees by tracking outstanding blocks.
- fail-Nth allocator: fails the Nth allocation (transient: only that
  one; persistent: that one and all later ones).
- misaligning allocator: returns pointers offset from malloc by 8 to
  simulate weak-alignment parents.
- checked-arg allocator: asserts the size/align arguments the arena
  passes back on xfree/xrealloc match what was allocated (verifies the
  arena honors the interface contract as a caller).

## 3. Test groups, ordered by bug-class severity

T1 arithmetic guards (memory-corruption class):

- size = 0, 1, PTRDIFF_MAX - 1, PTRDIFF_MAX, PTRDIFF_MAX + 1 (as
  size_t), SIZE_MAX, at every entry point.
- align = each power of two 1..ARENA_MAX_ALIGN, plus 0 (default), 3, 6,
  SIZE_MAX/2 + 1 (non-power-of-two and huge values are violations or
  failures per SPEC 4.4; the runtime-condition ones must fail cleanly).
- count * size overflow pairs around SIZE_MAX and PTRDIFF_MAX;
  header + size + (align - 1) combined overflow near the cap.
- growth doubling saturation: force enough blocks that doubling would
  overflow without the cap; assert the cap holds.
- fixed arena: buffer sizes 0, 1, ARENA_FIXED_OVERHEAD - 1, exactly
  ARENA_FIXED_OVERHEAD, ARENA_FIXED_OVERHEAD + 1; deliberately
  misaligned buffer starts (base + 1 through base + 15) with aligned
  requests.

T2 lifetime discipline (use-after-free class):

- use-after-temp-end and use-after-reset probes under ASan: allocate,
  rewind, assert the region is poisoned (`__asan_address_is_poisoned`
  spot checks), and death-test an actual dereference.
- temp nesting: begin/begin/end/end passes; out-of-order end and
  end-after-reset abort in debug builds (death tests); generation check
  fires when a temp from before a reset is ended.
- temp_end restores used, cursor block, and block chain exactly
  (compare arena_used and committed before/after; loop 1000 iterations
  of begin/alloc-various/end and assert committed stays flat, proving
  block recycling instead of growth).
- reset: retains first normal block, frees oversize blocks, clears
  status, preserves high_water, keeps the arena usable.

T3 overlap and content integrity (silent-corruption class):

- checksum survival: randomized op sequences (alloc sizes 1..8KB mixed
  with temps and reallocs); every allocation is filled with a
  pattern derived from its index; before every subsequent op and at the
  end, all live allocations verify their pattern. Catches overlap,
  header clobbering, and bad rewinds without sanitizer help.
- disjointness: for every pair of live allocations, [p, p+size) ranges
  do not intersect (checked on a bounded window of recent allocations).
- realloc matrix: grow/shrink in place when last; grow forcing a block
  jump; grow/shrink when not last (copy path); contents preserved to
  min(old, new) in every case; failure leaves old contents intact
  (fail-Nth parent); ptr == NULL old_size == 0 behaves as alloc.
- zero-size table: alloc(0), alloc_n with count 0 or size 0, memdup 0;
  all return NULL with arena_failed still false.

T4 OOM (crash and leak class), SQLite fail-Nth methodology:

- for each scenario S in a scenario table (plain allocs; growth across
  blocks; oversize; temp cycles; realloc chains; arena-as-allocator use):
  run S with the fail-Nth parent for N = 1, 2, 3, ... until S completes
  with no injected failure. Both transient and persistent modes. After
  every injected failure: arena_failed is true with the right status,
  the counting allocator reports no leak after deinit, and
  arena_clear_error followed by a small allocation works (transient
  mode).
- init-time failure: first-ever allocation fails; arena remains valid.
- sticky gating: after an injected failure, further allocs return NULL
  without touching the parent (counting allocator proves no calls).

T5 heap-disabled run (the plan's done-when gate):

- a dedicated test main links the suite's core cases against
  arena_init_fixed over a static `_Alignas(max_align_t) unsigned char`
  buffer, with alloc_null as the only other allocator in the binary.
  The counting allocator asserts zero parent calls. This is the proof
  that a fixed-buffer arena can run the test suite with the heap
  disabled.

T6 interface conformance (both directions):

- alloc_libc: contract table (zero size, NULL free, alignment up to
  max_align_t, over-aligned refusal, realloc preservation) plus a
  differential check against plain malloc behavior.
- alloc_null: everything fails cleanly, xfree accepts NULL and non-NULL.
- arena_allocator adapter: passes the same contract table; LIFO
  alloc/free pairs leave used unchanged; xfree of a non-last allocation
  is a harmless no-op; using the adapter after arena_reset is exercised
  only as a documented-violation death test.
- vtable-caller correctness: the checked-arg allocator runs under the
  whole suite, proving the arena always passes exact old sizes and
  aligns to its parent.

T7 statistics and alignment properties:

- every returned pointer satisfies its requested alignment (asserted
  inside the randomized T3 loop as well).
- used/committed/high_water: monotonic where promised, exact across
  temp cycles, high_water survives reset.
- with the misaligning parent, all alignment properties still hold.

## 4. NDEBUG and release behavior

The release target runs the full suite minus death tests (debug-only
detection). A dedicated case proves allocation failure handling is
identical with NDEBUG (no assert doubles as error handling), and that
debug fills are absent (contents of fresh allocations are simply
unspecified; the test only proves no crash and correct contracts).

## 5. Fuzzing

`tests/fuzz_arena.c`, libFuzzer, structure-aware op interpretation:

- input bytes decode a loop of ops: alloc (size, align class), zeroed
  alloc, memdup, realloc (target index, new size), temp_begin, temp_end,
  reset, trim, adapter alloc/free, with sizes biased small but
  occasionally huge to hit the guards.
- a shadow model tracks live allocations (pointer, size, checksum) and
  open temps; after every op the model and the arena must agree
  (checksums verified, alignment verified, invalid ops skipped by the
  model so the harness only performs contract-legal calls).
- runs under fuzzer,address,undefined with poisoning active; the ASan
  build is the point (it observes overlap and use-after-rewind
  directly).
- both backends fuzzed: growing (libc parent, budget-capped via a
  limiting parent allocator) and fixed (static buffer).
- corpus seeds: empty input, minimal one-op inputs, a temp-heavy
  sequence, an oversize-heavy sequence, regression inputs from every
  bug ever found (kept in tests/corpus/).
- PR budget: a short fixed run (about a minute) in check-adjacent use;
  longer campaigns before tags.

## 6. Static analysis

- gcc -fanalyzer over src/arena.c with normal compile options.
- clang --analyze likewise.
- clang-format --dry-run --Werror as the format gate (agenc-str rules).
- findings triaged, not counted; suppressions localized with reasons.

## 7. Regression discipline

Every bug fixed gets a test proven revert-sensitive: stash the source
fix, confirm the new test goes red, restore. A test that passes with and
without the fix guards nothing and is rewritten.

## 8. Evidence ledger template

Filled in and reported at every milestone claim and before any tag.

| Evidence | State (passed / failed / not applicable / unavailable + reason) |
| --- | --- |
| clang-format gate | |
| GCC c11 pedantic build + suite | |
| Clang c11 pedantic build + suite | |
| ASan + UBSan suite | |
| ISO variant suite | |
| Release NDEBUG FORTIFY suite | |
| Heap-disabled fixed-buffer run (T5) | |
| OOM fail-Nth loops, transient + persistent (T4) | |
| Checksum/overlap property loop (T3) | |
| Fuzz run (duration, execs, findings) | |
| Valgrind mempool job | |
| gcc -fanalyzer | |
| clang --analyze | |
| Death tests (debug violation detection) | |
| Revert-sensitivity of new regression tests | |

## 9. Out of scope for this library's verification

- ThreadSanitizer: the library holds no shared state and spawns no
  threads; concurrent use of one arena is a documented contract
  violation, not a synchronizable path. TSan lands with the first
  consumer that shares arenas across threads under external locks.
- Cross-architecture runs (big-endian, 32-bit): the code must be clean
  under -Wconversion and the ISO variant, and takes no layout or
  endianness assumptions; actual runs on other targets happen when the
  family gains CI runners for them and the ledger marks them
  unavailable until then.

## 10. Ledger: initial implementation, 2026-08-20

Toolchain: gcc 13.3.0, clang 18.1.3, clang-format 18.1.3, valgrind
(Ubuntu 24.04, x86_64, glibc). Suite sizes: 27242 checks per full
variant, 33907 under ASan (poisoning probes added), 27236 in release
(death tests are debug-only), 325 in the heapless binary.

| Evidence | State |
| --- | --- |
| clang-format gate | passed (make format, both compiler runs) |
| GCC c11 pedantic build + suite | passed (make check, CC=gcc) |
| Clang c11 pedantic build + suite | passed (make check, CC=clang) |
| ASan + UBSan suite | passed (make asan, -fno-sanitize-recover=all, poisoning active, both compilers) |
| ISO variant suite | passed (make iso, ARENA_STRICT_ISO launder path, both compilers) |
| Release NDEBUG FORTIFY suite | passed (make release, -O3 -D_FORTIFY_SOURCE=3, both compilers) |
| Heap-disabled fixed-buffer run (T5) | passed (make heapless; valgrind heap summary: 1 alloc total, the stdio buffer, none from arena code) |
| OOM fail-Nth loops, transient + persistent (T4) | passed (4 scenarios, run-until-clean, leak and consistency checks each injection) |
| Checksum/overlap property loop (T3) | passed (fill-pattern survival inside the suite and the fuzz harness's shadow model) |
| Fuzz run | passed (libFuzzer, fuzzer+address+undefined, 60 s sanity 122k execs, then 3602 s campaign 495,690 execs, both backends, zero findings, no artifacts) |
| Valgrind mempool job | passed (make valgrind, --error-exitcode=1 --leak-check=full, 0 errors, 0 leaks; caught and fixed two annotation bugs during M6) |
| gcc -fanalyzer | passed (src and tests, zero findings) |
| clang --analyze | passed (src, zero findings) |
| Death tests (debug violation detection) | passed (fork-based, temp out-of-order and temp-after-reset abort via SIGABRT) |
| Revert-sensitivity of new regression tests | passed for the two valgrind-layer fixes (errors reproduced before the fix, clean after); standing discipline for future bugs |
| Header self-containment | passed (alloc.h and arena.h compile alone, first in a TU, both compilers) |
| Cross-target/hardware test (32-bit, big-endian) | unavailable (no multilib libc on this machine; code carries no layout or endianness assumptions and is -Wconversion clean) |
| MemorySanitizer job | not applicable as a standing job (hooks are implemented and compile; no MSan target in the matrix yet) |
| Reproducible build | not applicable (no released artifacts; adoption is source copy) |

Addendum 2026-08-20, second review pass: two independent reviewer agents
(adversarial correctness, legibility against the reference) plus a
manual read produced one probe-proven defect and a set of contract and
comment corrections, all applied and re-verified. The defect:
arena_temp_end was not rollback-aware, so releasing or resizing a
pre-scope allocation inside a temp scope corrupted poison state and
resurrected freed bytes in the used statistic. Resolution: such
operations are now a documented contract violation (in-scope-only rule
in arena.h and SPEC 2.5/4.4), arena_temp_end detects the cursor
regression in debug builds and never raises a cursor in release builds,
and a revert-sensitive death test guards the detection. Also fixed: the
adapter's sticky-latch behavior is documented in alloc.h and arena.h
(it previously contradicted the interface's NULL-iff wording), the
last-allocation test rejects wrapping garbage sizes so adapter xfree
stays a no-op under violations, and the libc backend honors the
shrinks-never-fail promise by returning the old block when realloc
refuses a shrink. Full matrix re-run green under gcc and clang
(27278 checks), valgrind clean, analyzers clean, fuzz sanity clean.
