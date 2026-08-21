#ifndef AGENC_ARENA_H_INCLUDED
#define AGENC_ARENA_H_INCLUDED

#include <stdbool.h>
#include <stddef.h>

#include "alloc.h"

/*
 * arena.h: a region allocator with reset semantics.
 *
 * An arena owns a chain of blocks obtained from a parent allocator and
 * serves allocations by bumping a cursor. Individual allocations are not
 * freed; lifetime ends in bulk at a temp-scope end, arena_reset, or
 * arena_deinit. The fixed-buffer variant serves from caller memory and
 * never touches a parent. "Arena" here means a region/bump allocator;
 * jemalloc and glibc use the same word for a sharded general-purpose
 * heap, which is unrelated.
 *
 * Blocks are of two kinds: normal blocks, sized by the growth policy
 * (min_block doubling to max_block) and retained for reuse on a
 * per-arena free list, and oversize blocks, created for single
 * allocations larger than the policy size and returned to the parent as
 * soon as they die. There is no cross-arena sharing of any kind. Under
 * AddressSanitizer, blocks return to the parent with their interior
 * bytes still poisoned; a custom parent that inspects or fills memory
 * inside xfree must account for that.
 *
 * Independent arenas may be used concurrently. Anything that touches one
 * arena, including through its alloc_t adapter or a temp scope, requires
 * external synchronization when threads share it. The library spawns no
 * threads, installs no signal handlers, and holds no global mutable
 * state.
 *
 * Portability notes. Alignment padding is computed as an integer from
 * the cursor address and applied by advancing the cursor pointer; the
 * code never materializes a pointer from a computed integer, which is
 * sound under ISO/IEC TS 6010 provenance semantics and on CHERI. Fixed
 * arenas hand out typed objects from caller storage; when that storage
 * is a declared character array, ISO C11 has no explicit blessing for
 * the retyping, every mainstream compiler supports it, and every
 * shipping fixed-buffer allocator relies on it. Use an
 * _Alignas(max_align_t) unsigned char array, or memory obtained from an
 * allocator. Building with ARENA_STRICT_ISO inserts a compiler barrier
 * at the buffer attach point on GNU-compatible compilers.
 */

/* Public operation results. Error values remain stable API. */
typedef enum {
    /* Operation completed successfully. */
    ARENA_OK = 0,
    /* A pointer, object, alignment, or other argument is invalid. */
    ARENA_ERR_ARG = -1,
    /* The parent allocator refused, or a fixed buffer is exhausted. */
    ARENA_ERR_ALLOC = -2,
    /* A size computation exceeds PTRDIFF_MAX or wraps. */
    ARENA_ERR_OVERFLOW = -3
} arena_status_t;

/* First normal block size, block header included. */
#define ARENA_MIN_BLOCK_DEFAULT ((size_t)4096)
/* Growth cap for normal blocks. */
#define ARENA_MAX_BLOCK_DEFAULT ((size_t)65536)
/* The meaning of align 0 at every allocation entry point. */
#define ARENA_DEFAULT_ALIGN _Alignof(max_align_t)
/* Largest per-call align the arena serves. */
#define ARENA_MAX_ALIGN (((size_t)1) << 16)
/* Worst-case bytes of a fixed buffer consumed by bookkeeping; also the
 * floor for min_block, since every block must hold the same header. */
#define ARENA_FIXED_OVERHEAD ((size_t)64)
/* Bytes of poisoned redzone between allocations, ASan builds only. */
#define ARENA_ASAN_REDZONE ((size_t)16)

struct arena_block;

/*
 * The arena. Fields are caller-readable for stack allocation and
 * diagnostics; callers must not modify them or copy the struct by
 * assignment. Initialize with exactly one arena_init, arena_init_sized,
 * or arena_init_fixed call before first use.
 *
 * A failed operation changes nothing except recording a sticky status:
 * every live allocation, the cursor, the block chain, the free list, and
 * all statistics stay exactly as they were. After a failure is recorded,
 * allocation calls return NULL without changing the arena until
 * arena_clear_error or arena_reset. Queries, temp scopes (begin and
 * end), the adapter's xfree, reset, trim, and deinit remain available
 * while an error is sticky.
 */
typedef struct arena {
    alloc_t parent;
    struct arena_block *current; /* newest block, NULL before first use */
    struct arena_block *free_blocks;
    size_t min_block;
    size_t max_block;
    size_t next_block; /* next normal block size to request */
    size_t used;       /* live bytes handed out, padding included */
    size_t committed;  /* bytes held from the parent or the buffer */
    size_t high_water; /* max of used since init */
    unsigned temp_depth;
    unsigned generation; /* bumped by reset and deinit */
    unsigned flags;      /* internal */
    arena_status_t status;
} arena_t;

/*
 * A temp-scope snapshot. A value type; treat it as opaque. Valid until
 * its arena is rewound past it, reset, or deinitialized.
 */
typedef struct arena_temp {
    arena_t *arena;
    struct arena_block *block;
    size_t block_used;
    size_t used;
    unsigned generation;
    unsigned depth;
} arena_temp_t;

#if defined(__GNUC__) || defined(__clang__)
/*
 * malloc is sound on the bump entry points: they return storage no live
 * pointer aliases, holding no pointer values a caller may legally read.
 * It is deliberately absent from arena_memdup and arena_realloc, whose
 * results carry caller bytes. alloc_size states the usable size.
 * returns_nonnull is never used: NULL is the failure signal.
 */
#define ARENA_ATTR_MALLOC __attribute__((malloc))
#define ARENA_ATTR_ALLOC_SIZE(size_index) __attribute__((alloc_size(size_index)))
#define ARENA_ATTR_ALLOC_SIZE2(count_index, size_index)                                            \
    __attribute__((alloc_size(count_index, size_index)))
#else
#define ARENA_ATTR_MALLOC
#define ARENA_ATTR_ALLOC_SIZE(size_index)
#define ARENA_ATTR_ALLOC_SIZE2(count_index, size_index)
#endif

/*
 * Initializes previously uninitialized caller-owned storage as a growing
 * arena with default block sizes. Stores parent by value. Allocates
 * nothing; the first block is obtained lazily, so init cannot fail. All
 * three parent function pointers must be non-NULL. NULL arena is a
 * no-op.
 */
void arena_init(arena_t *a, alloc_t parent);

/*
 * arena_init with explicit block sizing. Requires
 * ARENA_FIXED_OVERHEAD <= min_block <= max_block <= PTRDIFF_MAX; other
 * values initialize the arena, record and return ARENA_ERR_ARG. NULL
 * arena returns ARENA_ERR_ARG with no object to record it on.
 */
arena_status_t arena_init_sized(arena_t *a, alloc_t parent, size_t min_block, size_t max_block);

/*
 * Initializes a fixed arena serving from size bytes at buffer. The arena
 * fails allocations with ARENA_ERR_ALLOC when the buffer is exhausted
 * and never touches a parent allocator. Any buffer alignment is
 * accepted; up to ARENA_FIXED_OVERHEAD bytes are consumed by
 * bookkeeping. The buffer must outlive the arena and is never freed by
 * it. Under checking builds the buffer stays poisoned while the arena
 * lives; call arena_deinit before reusing or discarding the storage. A
 * NULL buffer or zero size records ARENA_ERR_ARG; a size above
 * PTRDIFF_MAX records ARENA_ERR_OVERFLOW before any buffer access.
 * NULL arena is a no-op.
 */
void arena_init_fixed(arena_t *a, void *buffer, size_t size);

/*
 * Returns every block, including the free list, to the parent (nothing
 * for fixed arenas), invalidating all outstanding allocations and temp
 * scopes, then restores the state arena_init left, with the generation
 * advanced. NULL and already-deinitialized arenas are accepted. Never
 * fails.
 */
void arena_deinit(arena_t *a);

/*
 * Invalidates every outstanding allocation and open temp scope and
 * rewinds to empty. Normal blocks are retained (the oldest stays
 * current, the rest move to the free list); oversize blocks return to
 * the parent. Clears the sticky status, zeroes used, keeps high_water,
 * advances the generation, and sets temp_depth to 0. Usable while a
 * sticky error is set. NULL is a no-op.
 */
void arena_reset(arena_t *a);

/*
 * Returns free-list blocks to the parent. Live allocations are
 * unaffected. A no-op on fixed arenas and NULL. Never fails.
 */
void arena_trim(arena_t *a);

/* Clears the sticky status without changing content. NULL is a no-op. */
void arena_clear_error(arena_t *a);

/*
 * Allocation entry points. Common rules: a NULL arena returns NULL
 * (nothing to record a status on). A sticky non-OK status returns NULL
 * without changing the arena. A zero total size returns NULL as success,
 * recording nothing; arena_failed distinguishes that from failure. align
 * is 0 (meaning ARENA_DEFAULT_ALIGN) or a power of two up to
 * ARENA_MAX_ALIGN; other align values record ARENA_ERR_ARG, and align is
 * validated before the zero-size early return, so an invalid align is
 * reported even for zero-size requests. A size
 * above PTRDIFF_MAX, or a count * size product that would exceed it,
 * records ARENA_ERR_OVERFLOW. Parent refusal or fixed-buffer exhaustion
 * records ARENA_ERR_ALLOC. Success returns uninitialized memory (zeroed
 * variants zero it) aligned as requested, disjoint from every live
 * allocation, valid until the enclosing temp scope ends, arena_reset, or
 * arena_deinit. Under AddressSanitizer every allocation additionally
 * consumes ARENA_ASAN_REDZONE poisoned bytes and the effective minimum
 * alignment is 8, so fewer bytes fit per block than the same code sees
 * elsewhere.
 */

/* size bytes at the default alignment. */
void *arena_alloc(arena_t *a, size_t size) ARENA_ATTR_MALLOC ARENA_ATTR_ALLOC_SIZE(2);

/* size zeroed bytes at the default alignment. */
void *arena_alloc_zeroed(arena_t *a, size_t size) ARENA_ATTR_MALLOC ARENA_ATTR_ALLOC_SIZE(2);

/* count elements of size bytes at align. */
void *arena_alloc_n(arena_t *a, size_t count, size_t size, size_t align) ARENA_ATTR_MALLOC
    ARENA_ATTR_ALLOC_SIZE2(2, 3);

/* count elements of size zeroed bytes at align. */
void *arena_alloc_n_zeroed(arena_t *a, size_t count, size_t size, size_t align) ARENA_ATTR_MALLOC
    ARENA_ATTR_ALLOC_SIZE2(2, 3);

/*
 * Copies size bytes from src into the arena at the default alignment.
 * A NULL src with nonzero size records ARENA_ERR_ARG.
 */
void *arena_memdup(arena_t *a, const void *src, size_t size) ARENA_ATTR_ALLOC_SIZE(3);

/*
 * Follows the alloc.h xrealloc contract. A NULL ptr with old_size 0
 * behaves as arena_alloc_n(a, 1, new_size, align). When ptr is the
 * arena's most recent live allocation it grows or shrinks in place when
 * the current block allows; otherwise the call allocates, copies
 * min(old_size, new_size) bytes, and abandons the old region, which
 * stays part of the arena until rewind. Failure returns NULL, records
 * the status, and leaves the old block valid.
 * The contract's violations get three different fates here: an invalid
 * align or a zero new_size is detected in every build and records
 * ARENA_ERR_ARG; a NULL ptr with nonzero old_size aborts in debug
 * builds and is undefined in release builds; a wrong old_size or align
 * for ptr is undefined in every build.
 */
void *arena_realloc(arena_t *a, void *ptr, size_t old_size, size_t new_size, size_t align)
    ARENA_ATTR_ALLOC_SIZE(4);

/*
 * Opens a temp scope: snapshots the cursor and increments temp_depth.
 * Allocates nothing and never fails. On a NULL arena the returned scope
 * is inert and ending it is a no-op.
 */
arena_temp_t arena_temp_begin(arena_t *a);

/*
 * Rewinds the arena to the snapshot: allocations made since begin are
 * invalidated, normal blocks entered since then move to the free list,
 * oversize blocks created since then return to the parent, used returns
 * to its snapshot value, and temp_depth decrements. The sticky status is
 * not cleared; a failure inside the scope stays observable. Scopes nest
 * and must end in LIFO order. While a scope is open, arena_realloc and
 * the adapter's xfree may be applied only to allocations made inside
 * the current scope; releasing or resizing an older allocation moves
 * the cursor the snapshot describes and is a contract violation. Ending
 * a temp whose arena has since been reset, deinitialized, or already
 * rewound past it is a violation as well. Debug builds abort where the
 * generation, depth, and cursor checks can detect these; release builds
 * have undefined behavior, though the rewind never raises a cursor, so
 * released bytes are not resurrected.
 */
void arena_temp_end(arena_temp_t temp);

/*
 * Presents the arena as an alloc_t whose ctx is a. xalloc bumps the
 * arena, xrealloc follows arena_realloc, and xfree rolls the cursor back
 * when given the most recent live allocation and is a no-op otherwise,
 * so LIFO alloc/free pairs cost zero net memory apart from alignment
 * padding, which stays consumed. Failures stick like every arena
 * failure: later requests through the adapter return NULL until the
 * arena's owner clears or resets, and a generic alloc_t consumer cannot
 * do that itself, so probe-and-retry does not work through the adapter.
 * The adapter inherits the arena's single-owner thread contract and
 * dies with it: reset or deinit invalidates every block it handed out.
 * Returns an inert always-failing value for a NULL arena.
 */
alloc_t arena_allocator(arena_t *a);

/*
 * Returns a borrowed static status name. Unknown values map to
 * "ARENA_ERR_UNKNOWN". Never fails.
 */
const char *arena_status_name(arena_status_t status);

/* Returns the sticky status. NULL arena returns ARENA_ERR_ARG. */
arena_status_t arena_status(const arena_t *a);

/* True when the arena is non-NULL with status ARENA_OK. */
bool arena_ok(const arena_t *a);

/* True when the arena is NULL or its status is not ARENA_OK. */
bool arena_failed(const arena_t *a);

/*
 * Statistics. used counts live bytes handed out, alignment padding and
 * ASan redzones included; the padding and redzone of an abandoned
 * realloc source stay counted until rewind. committed counts bytes held
 * from the parent or
 * the fixed buffer, block headers and the free list included.
 * high_water is the maximum used has reached since init; reset preserves
 * it, deinit does not. NULL arena observes as zero.
 */
size_t arena_used(const arena_t *a);
size_t arena_committed(const arena_t *a);
size_t arena_high_water(const arena_t *a);

/*
 * Typed allocation macros. Zeroing by default; the _UNINIT variant is
 * the escape for hot paths. Arguments are evaluated exactly once.
 */
#define ARENA_NEW(a, T) ((T *)arena_alloc_n_zeroed((a), 1, sizeof(T), _Alignof(T)))
#define ARENA_NEW_N(a, T, n) ((T *)arena_alloc_n_zeroed((a), (n), sizeof(T), _Alignof(T)))
#define ARENA_NEW_N_UNINIT(a, T, n) ((T *)arena_alloc_n((a), (n), sizeof(T), _Alignof(T)))

#undef ARENA_ATTR_MALLOC
#undef ARENA_ATTR_ALLOC_SIZE
#undef ARENA_ATTR_ALLOC_SIZE2

#endif /* AGENC_ARENA_H_INCLUDED */
