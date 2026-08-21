# agenc-arena API specification

The contract for the two public headers. The header comments are the
final authority once written; this document is the design-time contract
they must implement. Rationale and evidence live in RESEARCH.md; decision
history lives in PLAN.md.

Scope of the library:

- `include/alloc.h`: the allocator interface (vtable) every agenc-*
  library allocates through, plus the libc and null backends.
- `include/arena.h`: the region allocator with reset semantics, a
  fixed-buffer backend, temp scopes, and an adapter that presents an
  arena as an allocator.
- `src/arena.c`: one implementation file for both headers.

Language: C11, no third-party dependencies, hosted standard library only.
Adoption is copying the two headers and the one source file.

Prefixes: `alloc_` for the interface, `arena_` for the region allocator.
Two prefixes in one repo follows the agenc-ds precedent (ds_vec_, ds_map_,
ds_list_). The `agenc-` prefix stays on the folder and repo name only.

## 1. The allocator interface (alloc.h)

### 1.1 Types

```c
typedef void *(*alloc_xalloc_fn)(void *ctx, size_t size, size_t align);
typedef void *(*alloc_xrealloc_fn)(void *ctx, void *ptr, size_t old_size,
                                   size_t new_size, size_t align);
typedef void (*alloc_xfree_fn)(void *ctx, void *ptr, size_t size,
                               size_t align);

typedef struct alloc {
    void *ctx;
    alloc_xalloc_fn xalloc;
    alloc_xrealloc_fn xrealloc;
    alloc_xfree_fn xfree;
} alloc_t;
```

An `alloc_t` is a value: four words, copied freely, stored by value inside
any object that will allocate later. Two allocators are the same allocator
if and only if all four members compare equal. Every block must be
released through an allocator equal to the one that allocated it.
Consumers store the allocator they were created with and use it for all
later calls; that stored copy is the identity.

The member names carry an x prefix because C11 7.1.4 permits stdlib
functions to be masked by function-like macros (MSVC crtdbg defines
free(p)), and a member named `free` would make `a->xfree(...)` unwritable
as `a->free(...)`. SQLite's xMalloc/xFree is the precedent.

All three function pointers must be non-NULL. There is no optional-member
probing.

### 1.2 Alignment rules (all three functions)

- `align` is 0 or a power of two.
- 0 means the default alignment, `_Alignof(max_align_t)`: the block is
  suitably aligned for any object of the requested size or smaller
  (malloc semantics).
- Every implementation must support all powers of two up to and including
  `_Alignof(max_align_t)`. Larger values may be unsupported; an
  unsupported alignment fails with NULL.
- On xrealloc and xfree the caller passes the same `align` as the call
  that allocated the block, so implementations never store alignment.

### 1.3 xalloc

- `size == 0`: must return NULL. This is success; no allocation was
  performed and nothing needs releasing. A zero-size request cannot fail,
  so `size != 0 && result == NULL` is the complete failure test.
- `size > 0`: returns a pointer to at least `size` bytes whose address is
  a multiple of the effective alignment, or NULL if and only if the
  request cannot be fulfilled (out of memory, unsupported alignment, or
  internally unrepresentable size; callers cannot distinguish these and
  must not need to).
- The memory is uninitialized. No zeroing is promised.
- Implementations must reject `size > PTRDIFF_MAX` with NULL.
- The block stays valid until passed to xfree or xrealloc of an equal
  allocator, or until the owner of the instance behind `ctx` resets or
  destroys it (for example arena reset), whichever comes first.
- On failure the allocator remains valid and the call returns normally.
  No abort, no longjmp, no exit.

### 1.4 xrealloc

Preconditions; violating any is a contract violation (see 4.4):

- `new_size != 0`. Freeing through realloc is forbidden; xfree exists.
  This deletes the realloc(p, 0) divergence that C23 declared UB
  (WG14 N2464).
- `align` equals the align of the call that allocated `ptr`.
- If `ptr == NULL` then `old_size == 0`, and the call behaves exactly
  like `xalloc(ctx, new_size, align)`.
- If `ptr != NULL` then `ptr` came from xalloc/xrealloc of an equal
  allocator, has not been freed, and `old_size` equals exactly the size
  established by the most recent allocating call for this block.

Behavior:

- Success returns a pointer (possibly equal to `ptr`, possibly moved) to
  at least `new_size` bytes at the same effective alignment. Bytes
  [0, min(old_size, new_size)) are preserved. Bytes beyond old_size are
  uninitialized. If the block moved, the old pointer is invalid.
- Failure returns NULL. The old block is untouched, still valid, still
  owned by the caller, and must still be released eventually.
- Shrinking may fail like any other call (the Lua 5.4 rule), but a failed
  shrink is always recoverable because the old block still holds all the
  data. The built-in backends never fail a shrink.
- Implementations should resize in place when they can; relocation is
  always permitted.

### 1.5 xfree

- `ptr == NULL`: must return with no effect, regardless of the other
  arguments. This keeps cleanup ladders unconditional.
- `ptr != NULL`: `ptr` came from xalloc/xrealloc of an equal allocator
  and has not been freed; `size` equals exactly the size established by
  the most recent allocating call; `align` equals the original align.
- xfree releases the caller's claim; it does not promise immediate reuse.
  An arena backend may make it a no-op, or roll the cursor back when the
  block is the most recent allocation.

### 1.6 Thread safety and reentrancy

- Thread safety is a property of the backend plus ctx pair, never of the
  struct. The struct value is immutable data, readable from any thread.
- The libc backend is thread-safe (as malloc is). An arena backend is
  single-owner and requires external synchronization if shared.
- A library that receives an allocator calls it only during the execution
  of its own API functions, on the calling thread, and must not assume
  the allocator synchronizes.
- Allocator implementations must not call back into the library that
  invoked them, and are not async-signal-safe unless a backend documents
  otherwise.

### 1.7 Backends and helpers

```c
alloc_t alloc_libc(void);
alloc_t alloc_null(void);
```

- `alloc_libc()`: malloc/realloc/free backed. Supports align up to
  `_Alignof(max_align_t)` only; larger alignments fail with NULL (MSVC
  has no aligned_alloc, and header-free routing of over-aligned frees is
  not portable; over-aligned callers use an arena). Ignores the size
  argument on xfree (debug builds may validate it where the platform
  offers a usable-size query).
- `alloc_null()`: every xalloc/xrealloc fails with NULL; xfree is a
  no-op. Used for heap-disabled runs and as the parent of fixed arenas.

Static inline call helpers route every call site through one place that
carries the debug assertions of the preconditions:

```c
static inline void *alloc_alloc(const alloc_t *a, size_t size, size_t align);
static inline void *alloc_realloc(const alloc_t *a, void *ptr,
                                  size_t old_size, size_t new_size,
                                  size_t align);
static inline void alloc_free(const alloc_t *a, void *ptr, size_t size,
                              size_t align);
static inline void *alloc_zeroed(const alloc_t *a, size_t size, size_t align);
```

`alloc_zeroed` is alloc plus memset; zeroing is deliberately outside the
vtable (P5 in RESEARCH.md).

Versioning: `alloc_t` is frozen. If the family ever needs an in-place
resize probe (the Zig resize/remap lesson), it ships as a new struct type
with a conversion helper, never as a mutation of this layout.

## 2. The arena (arena.h)

### 2.1 Model

An arena owns a chain of blocks obtained from a parent allocator and
serves allocations by bumping a cursor. Individual allocations are not
freed; lifetime ends in bulk at a temp-scope end, a reset, or deinit.
The fixed-buffer variant serves from caller memory and never touches a
parent.

Blocks are of two kinds: normal blocks, sized by the growth policy and
retained for reuse, and oversize blocks, created for single allocations
larger than the policy size, never retained.

Growth policy: the first normal block allocation requests `min_block`
bytes; each subsequent normal block doubles the previous size, saturating
at `max_block`. Defaults: `ARENA_MIN_BLOCK_DEFAULT` 4096,
`ARENA_MAX_BLOCK_DEFAULT` 65536. All sizes include the block header.
A request whose padded size plus header exceeds the next normal block
size gets a dedicated oversize block sized exactly for it.

Block retention: when the cursor leaves blocks behind (temp end, reset),
normal blocks move to a per-arena free list and are reused before the
parent is asked for new memory. Oversize blocks are returned to the
parent immediately. `arena_trim` returns the free list to the parent.
There is no cross-arena sharing of any kind (F9 in RESEARCH.md).

### 2.2 Types

```c
typedef enum {
    ARENA_OK = 0,
    ARENA_ERR_ARG = -1,
    ARENA_ERR_ALLOC = -2,
    ARENA_ERR_OVERFLOW = -3
} arena_status_t;

typedef struct arena {
    /* Caller-readable for stack allocation and diagnostics. Callers
       must not modify fields or copy the struct by assignment. All
       block bookkeeping lives behind the opaque block type. */
    alloc_t parent;
    struct arena_block *current;   /* newest block, NULL before first use */
    struct arena_block *free_blocks;
    size_t min_block;
    size_t max_block;
    size_t next_block;             /* next normal block size to request */
    size_t used;                   /* live bytes handed out */
    size_t committed;              /* bytes obtained from parent or buffer */
    size_t high_water;             /* max of used over the arena's life */
    unsigned temp_depth;           /* open temp scopes */
    unsigned generation;           /* bumped by reset and deinit */
    unsigned flags;                /* fixed-buffer, internal */
    arena_status_t status;         /* sticky */
} arena_t;

typedef struct arena_temp {
    arena_t *arena;
    struct arena_block *block;
    size_t block_used;
    size_t used;
    unsigned generation;
    unsigned depth;
} arena_temp_t;
```

`struct arena_block` is declared in arena.h and defined only in arena.c.

Invariants (checked by debug assertions, relied on everywhere):

- `used <= committed`, `high_water >= used`.
- Cursor arithmetic never forms a pointer outside a live block; all
  capacity checks happen on integers first (H2 in RESEARCH.md).
- Every size accepted is `<= PTRDIFF_MAX` (H3).
- A failed operation changes nothing except recording the sticky status.

### 2.3 Lifecycle

```c
void arena_init(arena_t *a, alloc_t parent);
arena_status_t arena_init_sized(arena_t *a, alloc_t parent,
                                size_t min_block, size_t max_block);
void arena_init_fixed(arena_t *a, void *buffer, size_t size);
void arena_deinit(arena_t *a);
void arena_reset(arena_t *a);
void arena_trim(arena_t *a);
void arena_clear_error(arena_t *a);
```

- `arena_init`: default block sizes. Never allocates; the first block is
  obtained lazily on the first allocation, so init cannot fail. NULL `a`
  is a no-op.
- `arena_init_sized`: validates `0 < min_block <= max_block <=
  PTRDIFF_MAX` and that min_block can hold the block header plus one
  default-aligned byte; returns and records ARENA_ERR_ARG otherwise.
- `arena_init_fixed`: the arena serves from `buffer` only and fails with
  ARENA_ERR_ALLOC when it is exhausted. Any `buffer` alignment is
  accepted; the first allocation aligns internally. A small bookkeeping
  region is carved from the front of the buffer;
  `ARENA_FIXED_OVERHEAD` names its worst-case size so tests and callers
  can size buffers. The intended buffer is a static or automatic
  `_Alignas(max_align_t) unsigned char` array or memory from an
  allocator; see the effective-type note in 4.5. The buffer must outlive
  the arena. `size == 0` or NULL buffer with nonzero size records
  ARENA_ERR_ARG.
- `arena_deinit`: returns every block including the free list to the
  parent (nothing for fixed arenas), then leaves the object in the state
  arena_init left it in, minus a bumped generation. Accepts NULL and
  already-deinitialized objects. Never fails.
- `arena_reset`: invalidates every outstanding allocation and open temp
  scope, rewinds to empty, retains normal blocks (first block stays
  current, others move to the free list), returns oversize blocks to the
  parent, clears the sticky status, zeroes `used`, keeps `high_water`,
  bumps `generation`, and sets `temp_depth` to 0. Usable while a sticky
  error is set.
- `arena_trim`: returns free-list blocks to the parent. Fixed arenas:
  no-op. Usable any time; never fails.
- `arena_clear_error`: clears the sticky status without touching content.

### 2.4 Allocation

```c
void *arena_alloc(arena_t *a, size_t size);
void *arena_alloc_zeroed(arena_t *a, size_t size);
void *arena_alloc_n(arena_t *a, size_t count, size_t size, size_t align);
void *arena_alloc_n_zeroed(arena_t *a, size_t count, size_t size,
                           size_t align);
void *arena_memdup(arena_t *a, const void *src, size_t size);
void *arena_realloc(arena_t *a, void *ptr, size_t old_size,
                    size_t new_size, size_t align);
```

Common rules:

- NULL arena returns NULL (nothing to record a status on).
- A sticky non-OK status makes every allocation call return NULL without
  changing the arena, until arena_clear_error or arena_reset. Chain
  allocations, check once; but note that using any returned pointer still
  requires it to be non-NULL.
- Zero total size returns NULL as success: no status is recorded, sticky
  state is unchanged. `arena_failed(a)` distinguishes this from failure.
- `align` follows the interface rules (1.2). The arena supports any
  power-of-two align up to `ARENA_MAX_ALIGN` (65536), including values
  above max_align_t; padding comes out of the current block.
- `count * size` is checked by division before multiplying; overflow
  records ARENA_ERR_OVERFLOW. Sizes above PTRDIFF_MAX record
  ARENA_ERR_OVERFLOW. Parent refusal records ARENA_ERR_ALLOC.
- Success returns uninitialized memory (zeroed variants memset), aligned
  as requested, disjoint from every other live allocation.
- `arena_alloc(a, size)` is `arena_alloc_n(a, 1, size, 0)`.
- `arena_memdup` is alloc (default align) plus memcpy; NULL `src` with
  nonzero size records ARENA_ERR_ARG.

`arena_realloc` follows the xrealloc contract (1.4) with one addition:
when `ptr` is the arena's most recent live allocation and the current
block can absorb the growth, it grows in place; shrinking the most recent
allocation returns the tail to the block. In all other cases it
allocates, copies min(old_size, new_size) bytes, and abandons the old
region (which stays part of the arena until reset). Failure returns NULL,
records the status, and leaves the old block valid.

Typed convenience macros (zeroing by default, per D-zero in RESEARCH.md):

```c
#define ARENA_NEW(a, T)        ((T *)arena_alloc_n_zeroed((a), 1, sizeof(T), _Alignof(T)))
#define ARENA_NEW_N(a, T, n)   ((T *)arena_alloc_n_zeroed((a), (n), sizeof(T), _Alignof(T)))
#define ARENA_NEW_N_UNINIT(a, T, n) ((T *)arena_alloc_n((a), (n), sizeof(T), _Alignof(T)))
```

Macro arguments are evaluated exactly once.

### 2.5 Temp scopes

```c
arena_temp_t arena_temp_begin(arena_t *a);
void arena_temp_end(arena_temp_t temp);
```

- `arena_temp_begin` snapshots the cursor and increments `temp_depth`.
  It allocates nothing and never fails. On a NULL arena it returns a temp
  whose `arena` is NULL; ending that temp is a no-op.
- `arena_temp_end` rewinds the arena to the snapshot: allocations made
  since begin are invalidated, normal blocks entered since then move to
  the free list, oversize blocks created since then return to the parent,
  `used` returns to its snapshot value, and `temp_depth` decrements.
  Sticky status is not cleared (a failure inside a temp scope stays
  observable).
- Scopes nest and must end in LIFO order. Ending a temp whose arena has
  since been reset, deinitialized, or already rewound past it is a
  contract violation; debug builds detect it through the generation and
  depth fields and abort, release builds have undefined behavior.
- After `arena_temp_end`, pointers into the rewound region are dangling.
  Checking builds fill and poison the region (section 3).

The scratch-arena pattern: a callee that needs temporary memory takes an
`arena_t *scratch` parameter distinct from the arena its results go to.
Never allocate results and scratch from the same arena inside one temp
scope (RESEARCH.md D-scratch, E2).

### 2.6 The arena as an allocator

```c
alloc_t arena_allocator(arena_t *a);
```

Returns an alloc_t whose ctx is `a`:

- xalloc bumps the arena (recording sticky status on failure as usual).
- xrealloc follows arena_realloc.
- xfree rolls the cursor back when given the most recent live allocation
  (making LIFO alloc/free pairs cost zero net memory) and is a no-op
  otherwise.
- The adapter inherits single-owner thread semantics from the arena. The
  allocator value dies with the arena: resetting or deinitializing the
  arena invalidates every block it handed out, without individual frees.

This is how agenc-ds and later libraries run entirely inside an arena.

### 2.7 Queries

```c
const char *arena_status_name(arena_status_t status);
arena_status_t arena_status(const arena_t *a);   /* NULL: ARENA_ERR_ARG */
bool arena_ok(const arena_t *a);                 /* non-NULL and ARENA_OK */
bool arena_failed(const arena_t *a);             /* NULL or not ARENA_OK */
size_t arena_used(const arena_t *a);             /* NULL: 0 */
size_t arena_committed(const arena_t *a);        /* NULL: 0 */
size_t arena_high_water(const arena_t *a);       /* NULL: 0 */
```

Statistics semantics: `used` counts bytes of live allocations including
alignment padding; `committed` counts bytes held from the parent or the
fixed buffer, including block headers and the free list; `high_water` is
the maximum `used` has reached since init (reset preserves it, deinit
does not). These exist because every surveyed production arena grew them
late and the region literature calls profiling the difference between
usable and unusable (RESEARCH.md F8, E2).

## 3. Checking builds: sanitizers and debug fills

Observable contract, all behind feature detection, zero cost otherwise:

- Under ASan (`__has_feature(address_sanitizer)` or
  `__SANITIZE_ADDRESS__`): block memory is poisoned when obtained,
  allocations unpoison exactly the user size, rewound and freed regions
  are re-poisoned, and a redzone of `ARENA_ASAN_REDZONE` (16) bytes
  separates allocations. Minimum effective alignment under ASan is 8 and
  the redzone is 8-aligned (shadow granularity, RESEARCH.md H6). ASan
  builds therefore fit fewer allocations per block; capacity-sensitive
  tests must account for it. In-place realloc growth unpoisons the
  extension; move paths poison the old region (the protobuf #20565
  regression shape).
- Under MSan: `__msan_allocated_memory` marks every allocation
  uninitialized with its own origin.
- Valgrind: opt-in with `ARENA_ENABLE_VALGRIND` and valgrind headers on
  the include path (not vendored; the no-third-party-dependencies rule).
  Maps each block to its own memcheck mempool: create on block
  acquisition, MEMPOOL_ALLOC per allocation, MEMPOOL_TRIM within the
  block on partial rewind, destroy when the block is recycled or
  released. Per-block anchoring is required because MEMPOOL_TRIM is
  range-based and one arena-wide pool would discard live chunks in
  sibling blocks.
- Debug fills (`!defined(NDEBUG)`, no sanitizer): fresh allocations are
  filled with 0xA5, rewound and freed regions with 0x5A (jemalloc's
  junk convention). Under ASan the fill is skipped; poison is stronger.
- `ARENA_DISABLE_SANITIZER_HOOKS` turns all of it off for exotic
  toolchains.

## 4. Cross-cutting rules

### 4.1 Status and sticky-error semantics

Follows agenc-str: the first failure records its status on the arena;
later allocation calls return NULL and change nothing until
arena_clear_error or arena_reset. Queries, temp_end, reset, trim, and
deinit remain available while an error is sticky. arena_status_name
returns a borrowed static string; unknown values map to
"ARENA_ERR_UNKNOWN".

### 4.2 Failure atomicity

A failed call leaves every live allocation, the cursor, the block chain,
the free list, and all statistics exactly as they were. The sticky status
is the only state change. If a growth attempt obtained a block from the
parent and then a later step failed, the block goes to the free list or
back to the parent; it never leaks and never becomes partially live.

### 4.3 Concurrency

Independent arenas may be used concurrently. Anything that touches one
arena (including through its alloc_t adapter or a temp scope) requires
external synchronization when threads share it and any of them may
allocate, rewind, reset, trim, or deinit. The library spawns no threads,
installs no signal handlers, and holds no global mutable state.

### 4.4 Contract violations

The following are programmer errors, not runtime conditions: passing a
wrong old_size or align to realloc or free, realloc to size 0, ending
temps out of order or after reset, using an allocation after its scope
ended, freeing through an unequal allocator. Debug builds abort with a
message where detection is affordable (generation and depth checks,
cursor validation). Release builds do not pay for detection and the
behavior is undefined. Malformed sizes (overflow, > PTRDIFF_MAX) are
runtime conditions, not violations: they fail with a recorded status,
because size arithmetic on untrusted input is expected caller behavior
in this family.

Align values are split: at the vtable boundary a non-power-of-two align
is a contract violation (backends stay lean; the inline helpers assert
it in debug builds). At the arena entry points an invalid align
(non-power-of-two, or a power of two above ARENA_MAX_ALIGN) is a defined
failure recording ARENA_ERR_ARG, matching the family's str-style
argument handling. The arena validates at its boundary and therefore
never forwards an invalid align to its parent.

### 4.5 Portability notes carried in the header

- Alignment padding is computed as an integer from the cursor address
  and applied by advancing the cursor pointer; the code never materializes
  a pointer from computed integers. This relies on `uintptr_t` existing
  and follows ISO/IEC TS 6010 provenance semantics; it is correct on
  CHERI (RESEARCH.md H1).
- Fixed-buffer arenas hand out typed objects from caller-provided
  storage. When that storage is a declared character array, ISO C11 has
  no explicit blessing for the retyping (unlike malloc'd memory); every
  mainstream compiler supports it and every shipping fixed-buffer
  allocator relies on it. The header documents this, recommends
  `_Alignas(max_align_t) unsigned char` buffers, and the implementation
  keeps a single internal launder point so a paranoid build can insert a
  compiler barrier (RESEARCH.md H5).

### 4.6 Function attributes

GNU/Clang builds annotate, behind version guards: the four bump entry
points (arena_alloc, arena_alloc_zeroed, arena_alloc_n,
arena_alloc_n_zeroed) get the malloc attribute plus alloc_size (usable
size equals requested size). arena_memdup and arena_realloc get
alloc_size only, never the malloc attribute: both return storage whose
contents were copied from caller bytes and can carry pointers to live
objects, which violates GCC's documented malloc-attribute semantics
(RESEARCH.md H7). Nothing gets returns_nonnull (NULL is the failure
signal) or nonnull on the arena (a NULL arena is defined behavior), and
alloc_align is not used (the align parameter may be 0 meaning default,
which the attribute does not model).

## 5. Constants

| Name | Value | Meaning |
| --- | --- | --- |
| ARENA_MIN_BLOCK_DEFAULT | 4096 | first normal block size, header included |
| ARENA_MAX_BLOCK_DEFAULT | 65536 | growth cap for normal blocks |
| ARENA_DEFAULT_ALIGN | _Alignof(max_align_t) | the meaning of align 0 |
| ARENA_MAX_ALIGN | 65536 | largest supported per-call align |
| ARENA_FIXED_OVERHEAD | impl, <= 64 | fixed-arena bookkeeping carve-out |
| ARENA_ASAN_REDZONE | 16 | inter-allocation redzone, ASan builds only |

Values above are the design targets; the header defines the final
numbers and the tests pin them.
