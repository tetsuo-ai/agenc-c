# agenc-arena research survey

Findings behind the implementation and its verification plan. Four research
passes were run on 2026-08-20: practitioner arena implementations, the
academic region-allocation literature, allocator-interface precedents, and
correctness/sanitizer/verification practice. Every claim below was checked
against the primary source listed in the bibliography.

Terminology: in this repo "arena" means a region/bump allocator in the
Hanson (1990) sense. jemalloc and glibc use "arena" for a sharded
general-purpose heap, which is an unrelated concept. The same idea appears
in the literature as "region" (Tofte-Talpin), "zone" (Ross 1967, V8),
"pool" (Apache), and "obstack" (GNU).

## 1. Prior art at a glance

| Implementation | Layout | Growth | Align | Reset keeps blocks | OOM policy |
| --- | --- | --- | --- | --- | --- |
| Hanson CII arena | chained chunks | request + MEMINCR KB (lcc: 10) | fixed max | yes (global cache) | longjmp exception |
| glibc obstack | chained chunks | chunk_size min, +12.5% slack | per-obstack mask | frees dead chunks | global handler, exit |
| APR pools | blocks + size-class recycler | flat, recycled | fixed 8 | yes | per-pool abort_fn else NULL |
| nginx ngx_pool_t | equal blocks + large list | flat (pool size) | word, opt-out | yes (frees larges) | NULL |
| LLVM BumpPtrAllocator | slab vector, no headers | 4KB, doubles per 128 slabs | per-call, floor 8 | first slab only | delegates (fatal) |
| protobuf Arena | per-thread block chains | 256B doubling, 32KB cap | 8 + per-call | first block only | delegates |
| V8 Zone | segment chain | size + 2x prev, 8-32KB clamp | fixed 8 | one segment | fatal |
| Fleury raddebugger | reserve/commit VM + chain | 64MB reserve, 64KB commit | per-call, floor 8 | optional free list | abort |
| Wellons (nullprogram) | single region, two pointers | none (size up front) | per-call | by-value copy = mark | abort, longjmp, or NULL flag |
| gingerBill / Odin | single buffer; Dynamic_Arena chains | flat 64KB, out-of-band split | per-call, default 2*ptr | yes | NULL / error enum |
| talloc (contrast) | per-allocation headers, ownership tree | n/a | n/a | n/a | NULL |

## 2. Convergent findings

F1. Chained blocks obtained from a parent allocator is the portable layout.
Every non-OS-specific design is a linked chain of blocks with a small
header and a bump cursor. Reserve/commit virtual memory (Fleury) is better
where available but is exactly the OS dependency this layer excludes; the
parent-allocator vtable leaves that door open (an mmap-backed parent can be
supplied later by agenc-os without changing the arena).

F2. Growth is geometric with a cap, and oversized requests get a dedicated
block. protobuf doubles 256B to a 32KB cap. V8 grows size + 2x previous,
clamped 8-32KB. LLVM doubles every 128 slabs from 4KB with the multiplier
saturated at 2^30. Every chained design routes a request larger than the
block size to its own exactly-sized block on the same chain (obstack, nginx
large list, LLVM CustomSizedSlabs, protobuf, Odin out-of-band, V8).

F3. Reset retains memory. Hanson, nginx, and APR keep all normal blocks;
LLVM, V8, and protobuf keep the first block; nobody frees everything on
reset, because steady-state reuse is the point of resetting. Oversized
blocks are the exception: LLVM and nginx release those on reset.

F4. Per-call alignment with a small floor is the modern consensus. Raw
functions take (size, align); typed macros supply _Alignof(T); a floor of 8
or pointer-size keeps the common path branch-light (LLVM MinAlign). Older
designs (Hanson, APR, nginx, V8) hard-code a max alignment and cannot serve
over-aligned requests. Padding is computed with the power-of-two mask.

F5. Individual free is a no-op by contract. LLVM Deallocate only poisons.
The one useful exception is nginx letting oversized allocations be freed
individually, cheap because they are already individually tracked.

F6. The last-allocation fast path replaces obstack's growing object.
Wellons and gingerBill grow or shrink an allocation in place only when it
is the newest one, else allocate and copy. Obstack's stateful one-open-
object API is widely considered the wrong shape (non-composable, unstable
addresses, macro multiple-evaluation hazards).

F7. Sanitizer integration is table stakes. LLVM poisons slabs on creation,
unpoisons the exact user size per allocation, re-poisons on deallocate and
reset, and calls __msan_allocated_memory. V8 adds 24-byte redzones under
ASan and a 0xcd debug fill. protobuf routes everything through gated
helpers and probes at startup whether poisoning is live. Fleury poisons
whole blocks and unpoisons pushes.

F8. Statistics are cheap and universally regretted when absent. peak_used
(gingerBill), SpaceAllocated vs SpaceUsed (protobuf), dual counters (V8),
_obstack_memory_used. The ML Kit team called their region profiler the
breakthrough that made regions usable at all.

F9. Two known anti-patterns to avoid outright: Hanson's global cross-arena
chunk cache (thread-safety landmine; keep any cache per-arena), and
obstack's process-global failure handler (a library must confine failure
policy to the object or the call).

## 3. Divergent choices and the position taken here

D-zero. Zero-on-alloc by default: Wellons and gingerBill zero (zero-is-
initialized idioms); obstack, APR, nginx, LLVM, protobuf, V8, and Hanson do
not (cost on hot paths; zeroing full capacity defeats OS zero pages).
Position: functions return uninitialized memory, explicit _zeroed variants
and typed macros that zero. This matches Fleury's arrangement (zeroing
macro default with a no-zero escape) without hiding a memset in every call.

D-oom. Failure policy: abort (Fleury, V8, LLVM), NULL (nginx, APR,
gingerBill), longjmp (Hanson, Wellons option). Position: NULL plus sticky
status recorded on the arena, matching the family convention set by
agenc-str; no abort, no longjmp, no global or per-arena callback hooks.
Library code in this family never calls exit or abort (LIBRARIES.md).

D-scratch. Scratch arenas: Fleury's two thread-local scratch arenas with
conflict detection needs TLS and process-lifetime state, which the family
conventions exclude (no global mutable state, no threads spawned).
Wellons' by-value scratch and explicit temp marks are the same mechanism
without the globals. Position: explicit temp scopes only; a scratch pool
can be layered by applications.

D-cleanup. Cleanup callbacks at reset/destroy (APR, nginx, protobuf) serve
framework-shaped programs that mix fds into memory lifetimes. The compiler-
school arenas omit them. Position: omitted; revisit only after being missed
three times (LIBRARIES.md rule).

D-hier. Pool hierarchies (APR, talloc) trade bookkeeping and a wrong-pool
bug class for cascading destroy. Position: flat arenas plus temp marks; an
arena backed by another arena through the vtable composes the useful part.

## 4. Evidence from the literature

E1. Speed. Hanson 1990 measured about 8 VAX instructions per allocation
against about 26 for first fit; replacing quick fit with arenas in lcc
improved total compiler runtime 8-10%. Berger, Zorn, McKinley (OOPSLA
2002) found regions the only custom-allocator class that still beat
DLmalloc: the Lea allocator ran 21-47% slower across lcc and mudlle,
with the abstract capping region wins at 44%. The 2026 re-evaluation (van Kempen and Berger,
arXiv:2605.17119) shows clean-heap wins over mimalloc-class allocators
shrink to 0-15%, but under a realistically fragmented heap naive
allocation degrades up to 2x while region code is unaffected, traced to
cache misses. Google reports 15-30% more work per CPU from protobuf
arenas, attributed mostly to locality (abseil fast tip 7). The honest
pitch is locality, bulk free, and predictability.

E2. Memory blowup is the measured cost. Berger: regions raised peak
footprint by 6% to 63% (average 23%) versus immediate free, with drag up
to 3.34x on lcc. ML Kit saw 8% to over 3000% of the GC baseline. Cyclone
found cfrac-style interleaved lifetimes a poor fit. The three proven
countermeasures all belong in the core API: temp marks (the manual analog
of ML Kit storage-mode resets), cheap arenas so per-phase and per-request
arenas are idiomatic (Hanson fixed lcc by adding one lifetime group;
Cyclone's mini_httpd eliminated heap allocation with a per-request
region), and documentation naming the anti-patterns (producer-consumer,
unbounded buffers, event-driven lifetimes; protobuf keeps strings off
arena for exactly this reason).

E3. Hanson's published inline fast path had a real bug (the Briggs
correspondence appended to the author's copy of the paper): it bumped
the cursor before the overflow test, leaving a permanent gap when the
slow path extended the arena in place. Test first, in integer space,
then bump.

E4. Locality details are measured. Gay and Aiken 1998: putting hot small
objects in one region and cold large ones in another sped moss up 24%;
they offset successive region descriptors by 64 bytes to dodge cache-set
collisions. Bonwick 1994: power-of-two size+alignment patterns are
pessimal for cache and bus distribution. Immix (PLDI 2008) independently
confirms bump allocation into coarse blocks is a locality technology and
that one live object pinning a region is the fundamental space hazard.

E5. Slab/pool is the complement, never the competitor. Bonwick 1994:
object caches win for same-type objects with constructed state and
individual churn (allocating and freeing a stream head fell from 33us
to 5.7us combined). Batch-shaped lifetimes
belong to arenas. agenc-ds can layer pools on top of the arena later.

E6. Security. Cling (USENIX Security 2010) documents that custom
allocators conceal allocation sites from hardened heaps and that address
reuse across reset is exactly the type-unsafe reuse use-after-free
exploits need. Consequences adopted: poison and fill on reset in checking
builds, keep block headers away from user data via redzones under ASan,
and say plainly in the docs that credential-like long-lived data does not
belong in a general arena.

## 5. Interface precedents (allocator vtable)

P1. Alignment is a parameter in every post-2010 design: Vulkan
VkAllocationCallbacks, Zig std.mem.Allocator, Odin, C++ pmr, jemalloc
extent hooks. Callers repeat the original alignment on realloc (Vulkan)
and on both realloc and free (Zig, pmr precondition), so
implementations never store it; Vulkan's free takes neither size nor
alignment, which is exactly the header tax the sized design avoids.
Byte units, power of two; a log2 encoding invites unit bugs in C.

P2. Sized deallocation won. Lua osize, Zig slice lengths, Odin old_size,
FreeType cur_size (1996), pmr deallocate(p, bytes, alignment), C23
free_sized and free_aligned_sized, jemalloc sdallocx. SQLite documents the
cost of omitting it: an 8-byte header per allocation just to answer
xSize. Exact old size (pmr/Zig rule), or the arena rollback trick and
debug validation are impossible.

P3. Zero-size rules must be designed, since C23 made realloc(p, 0)
undefined after implementations diverged irreconcilably (WG14 N2464).
Adopted: alloc(0) returns NULL as success; realloc to 0 is forbidden
(free exists); realloc(NULL, 0, n) behaves as alloc; free(NULL) is a
no-op regardless of other arguments.

P4. NULL is the only failure signal in every C-facing design surveyed.
Failed realloc leaves the old block valid (FreeType, Vulkan). Lua 5.4
dropped the shrink-never-fails guarantee that 5.3 had; shrink may fail,
and a failed shrink is always recoverable.

P5. No zeroing promise in the interface. Odin zeroes by default and had
to double its mode enum with Non_Zeroed escapes. CPython keeps calloc as
a separate entry because a backend can beat malloc+memset; a 3-function
vtable cannot, so the honest contract is uninitialized memory.

P6. free_all does not belong in the vtable. Odin's Free_All is callable
by generic code that cannot know whether it is safe. pmr puts release()
on the concrete resource, not the interface. Bulk reclamation is the
arena's own API.

P7. Global allocator hooks are a failed design. GLib deprecated
g_mem_set_vtable to a stub (constructors allocate before main); SDL3
survives only by demanding installation before any allocation. Passed-in
allocator values only.

P8. Member names must dodge libc macros. C11 7.1.4 lets free be a
function-like macro (MSVC crtdbg does this), and a->free(...) would
expand it. SQLite's x prefix (xMalloc, xFree) is the precedent.

## 6. Correctness hazards (adopted obligations)

H1. Alignment math: compute padding as an integer and advance the
original pointer: padding = (size_t)(-(uintptr_t)p & (align - 1)). Never
cast a computed integer back to a pointer. This form is sound under the
published provenance model (ISO/IEC TS 6010:2025, PNVI-ae-udi) and works
on CHERI.

H2. Capacity checks happen in integer space before any pointer is
formed. Forming a pointer past one-past-the-end is UB by itself (CERT
ARR30-C), and compilers delete cur + n > end style checks. LLVM and
protobuf both compare in uintptr_t space.

H3. Cap every allocation, block size, and growth step at PTRDIFF_MAX.
Objects larger than PTRDIFF_MAX make in-object pointer subtraction UB and
provoke real miscompiles (TrustInSoft); glibc 2.30 and mimalloc adopted
the cap. Guard size + align - 1 and count * size with subtraction and
division forms (APR CVE-2009-2412 and glibc's 2026 memalign CVE are this
bug class).

H4. Do not assume parent blocks are more than max_align_t aligned, and
some implementations return even less for small blocks (WG14 N2293
documents the strong/weak alignment split; its weak list names the
Windows CRT and the jemalloc-family libcs such as FreeBSD, NetBSD, and
Bionic, and LLVM D118804 adds tcmalloc and mimalloc). Run the round-up on every allocation including the first.
aligned_alloc cannot be the over-alignment path: MSVC does not provide
it.

H5. The fixed-buffer backend has a real effective-type wrinkle: C11 has
no rule that a declared char array provides storage for typed objects
(C11 6.5p6-7; CERT EXP39-C covers the adjacent incompatible-access
rule), unlike C++ P0593. Practice (Wellons, every shipping
fixed-buffer allocator) relies on the de-facto guarantee that compilers
do not exploit this for raw character buffers. Adopted stance: accept
void *, document the intended _Alignas(max_align_t) unsigned char array
usage and the reliance, and keep a launder hook available for paranoid
builds.

H6. ASan shadow granularity is 8 bytes and asymmetric: poison covers
[p, AlignDown(p+size, 8)), unpoison covers [AlignDown(p, 8), p+size). All
allocations must start 8-aligned and redzones must be at least 8 bytes
and 8-aligned or poisoning silently develops holes. Unpoison the exact
user size, never the padded size (LLVM). protobuf PR #20565 is the
canonical regression: in-place growth failed to unpoison the extension,
and the ASan build took a different path than production. Valgrind's
MEMPOOL client requests map exactly onto arenas, with MEMPOOL_TRIM
matching temp-mark rewind; the headers are BSD-licensed but support stays
opt-in to preserve the no-third-party-dependency rule.

H7. Attribute facts verified against GCC docs: the malloc attribute's
assertion (result aliases nothing live, storage contains no valid
pointers) is true for bump allocations and false for realloc-like
functions or recycled unscrubbed memory. Never returns_nonnull on a
NULL-returning allocator. alloc_size must state the usable size. APR
annotates apr_palloc with alloc_size and nonnull only.

## 7. Bibliography

Practitioner sources:
- Ryan Fleury, Untangling Lifetimes: The Arena Allocator (2022),
  https://www.rfleury.com/p/untangling-lifetimes-the-arena-allocator
- raddebugger base_arena,
  https://github.com/EpicGamesExt/raddebugger (src/base/base_arena.h, .c)
- Chris Wellons, Arena allocator tips and tricks,
  https://nullprogram.com/blog/2023/09/27/ (also 2023/09/30, 2023/10/05,
  2025/09/30)
- gingerBill, Memory Allocation Strategies parts 2-4,
  https://www.gingerbill.org/article/2019/02/08/memory-allocation-strategies-002/
- Odin core:mem and base:runtime allocators,
  https://github.com/odin-lang/Odin
- Hanson, C Interfaces and Implementations arena,
  https://github.com/drh/cii (src/arena.c)
- GNU obstack, https://sourceware.org/glibc/manual/latest/html_node/Obstacks.html
- APR pools, https://apr.apache.org/docs/apr/1.7/group__apr__pools.html
- nginx pools, https://nginx.org/en/docs/dev/development_guide.html
- LLVM Allocator.h,
  https://github.com/llvm/llvm-project/blob/main/llvm/include/llvm/Support/Allocator.h
- protobuf arenas, https://protobuf.dev/reference/cpp/arenas/
- V8 Zone, https://github.com/v8/v8 (src/zone/)
- talloc, https://talloc.samba.org/talloc/doc/html/index.html

Papers:
- Hanson, Fast allocation and deallocation of memory based on object
  lifetimes, SP&E 20(1), 1990, https://doi.org/10.1002/spe.4380200104
  (author PDF with the Briggs correspondence:
  https://drhanson.s3.amazonaws.com/storage/documents/fastalloc.pdf)
- Tofte, Talpin, Region-based memory management, I&C 132(2), 1997,
  https://doi.org/10.1006/inco.1996.2613
- Tofte, Birkedal, Elsman, Hallenberg, A retrospective on region-based
  memory management, HOSC 17, 2004, https://elsman.com/mlkit/pdf/retro.pdf
- Gay, Aiken, Memory management with explicit regions, PLDI 1998,
  https://theory.stanford.edu/~aiken/publications/papers/pldi98a.pdf
- Gay, Aiken, Language support for regions, PLDI 2001,
  https://theory.stanford.edu/~aiken/publications/papers/pldi01.pdf
- Grossman et al., Region-based memory management in Cyclone, PLDI 2002,
  https://www.cs.umd.edu/projects/cyclone/papers/cyclone-regions.pdf
- Berger, Zorn, McKinley, Reconsidering custom memory allocation,
  OOPSLA 2002, https://people.cs.umass.edu/~emery/pubs/berger-oopsla2002.pdf
- Bonwick, The slab allocator, USENIX 1994,
  https://people.eecs.berkeley.edu/~kubitron/courses/cs194-24-S14/hand-outs/bonwick_slab.pdf
- Bonwick, Adams, Magazines and Vmem, USENIX ATC 2001
- Blackburn, McKinley, Immix, PLDI 2008,
  https://www.steveblackburn.org/pubs/papers/immix-pldi-2008.pdf
- van Kempen, Berger, Reconsidering "Reconsidering Custom Memory
  Allocation", 2026, https://arxiv.org/abs/2605.17119
- Akritidis, Cling: a memory allocator to mitigate dangling pointers,
  USENIX Security 2010,
  https://www.usenix.org/legacy/event/sec10/tech/full_papers/Akritidis.pdf
- abseil performance tip 7 (protobuf arena fleet data), https://abseil.io/fast/7

Interface and standards:
- Lua 5.4 lua_Alloc, https://www.lua.org/manual/5.4/manual.html#lua_Alloc
- Zig std.mem.Allocator,
  https://github.com/ziglang/zig/blob/master/lib/std/mem/Allocator.zig
  (Allocgate: https://github.com/ziglang/zig/pull/10055)
- Vulkan allocation callbacks,
  https://docs.vulkan.org/spec/latest/chapters/memory.html
- SQLite sqlite3_mem_methods, https://sqlite.org/c3ref/mem_methods.html
- CPython PyMemAllocatorEx, https://docs.python.org/3/c-api/memory.html
- C++ pmr::memory_resource, https://eel.is/c++draft/mem.res.class
- WG14 N2464 (realloc(p,0) UB),
  https://www.open-std.org/jtc1/sc22/wg14/www/docs/n2464.pdf
- WG14 N2699/N2801 (free_sized, free_aligned_sized),
  https://www.open-std.org/jtc1/sc22/wg14/www/docs/n2699.htm
- GLib g_mem_set_vtable deprecation,
  https://docs.gtk.org/glib/func.mem_set_vtable.html

Correctness and verification:
- ISO/IEC TS 6010:2025 (provenance), https://www.iso.org/standard/81899.html
  (drafts N3226/N3231; N2364 PNVI semantics)
- WG14 N2293 (malloc alignment strong/weak split),
  https://www.open-std.org/JTC1/SC22/WG14/www/docs/n2293.htm
- CERT ARR30-C, EXP39-C,
  https://cmu-sei.github.io/secure-coding-standards/
- TrustInSoft on PTRDIFF_MAX,
  https://www.trust-in-soft.com/resources/blogs/2016-05-20-objects-larger-than-ptrdiff_max-bytes
- APR CVE-2009-2412, https://nvd.nist.gov/vuln/detail/cve-2009-2412
- ASan manual poisoning,
  https://github.com/google/sanitizers/wiki/AddressSanitizerManualPoisoning
  (granularity notes: https://github.com/Doy-lee/ASANManualPoisoning)
- Valgrind memcheck mempool API, https://valgrind.org/docs/manual/mc-manual.html
  (recipe: https://developers.redhat.com/articles/2022/03/23/use-valgrind-memcheck-custom-memory-manager)
- protobuf realloc poisoning regression,
  https://github.com/protocolbuffers/protobuf/pull/20565
- SQLite OOM fail-Nth testing methodology, https://www.sqlite.org/testing.html
- GCC common function attributes,
  https://gcc.gnu.org/onlinedocs/gcc-14.2.0/gcc/Common-Function-Attributes.html
- OpenSSF compiler annotations matrix,
  https://best.openssf.org/Compiler-Hardening-Guides/Compiler-Annotations-for-C-and-C++.html
- CMASan (custom allocators mask ASan), IEEE S&P 2025,
  https://yonghwi-kwon.github.io/data/cmasan_sp25.pdf
- Rust fuzz book, structure-aware fuzzing,
  https://rust-fuzz.github.io/book/cargo-fuzz/structure-aware-fuzzing.html
