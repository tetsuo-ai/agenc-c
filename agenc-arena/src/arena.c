/* arena.c: allocator interface backends and the arena region allocator. */

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "alloc.h"
#include "arena.h"

/*
 * Checking-build instrumentation. ASan and MSan hooks engage under
 * feature detection; Valgrind mempool annotation is opt-in with
 * ARENA_ENABLE_VALGRIND (valgrind headers come from the include path,
 * never vendored); ARENA_DISABLE_SANITIZER_HOOKS turns everything off.
 * Debug fills run only when no tool tracks the memory, because writing
 * a fill pattern into poisoned or unaddressable bytes would itself be a
 * report. Fills always happen while the bytes are still addressable,
 * then the region is marked dead.
 */

#if !defined(ARENA_DISABLE_SANITIZER_HOOKS)
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define ARENA_HAS_ASAN 1
#endif
#if __has_feature(memory_sanitizer)
#define ARENA_HAS_MSAN 1
#endif
#endif
#if !defined(ARENA_HAS_ASAN) && defined(__SANITIZE_ADDRESS__)
#define ARENA_HAS_ASAN 1
#endif
#if defined(ARENA_ENABLE_VALGRIND)
#define ARENA_HAS_VALGRIND 1
#endif
#endif

#if defined(ARENA_HAS_ASAN)
#include <sanitizer/asan_interface.h>
#define ARENA_POISON(ptr, len) ASAN_POISON_MEMORY_REGION((ptr), (len))
#define ARENA_UNPOISON(ptr, len) ASAN_UNPOISON_MEMORY_REGION((ptr), (len))
#else
#define ARENA_POISON(ptr, len) ((void)(ptr), (void)(len))
#define ARENA_UNPOISON(ptr, len) ((void)(ptr), (void)(len))
#endif

#if defined(ARENA_HAS_MSAN)
#include <sanitizer/msan_interface.h>
#define ARENA_MSAN_ALLOCATED(ptr, len) __msan_allocated_memory((ptr), (len))
#else
#define ARENA_MSAN_ALLOCATED(ptr, len) ((void)(ptr), (void)(len))
#endif

#if defined(ARENA_HAS_VALGRIND)
#include <valgrind/memcheck.h>
#define ARENA_VG_CREATE(block) VALGRIND_CREATE_MEMPOOL((block), 0, 0)
#define ARENA_VG_DESTROY(block) VALGRIND_DESTROY_MEMPOOL((block))
#define ARENA_VG_ALLOC(block, ptr, len) VALGRIND_MEMPOOL_ALLOC((block), (ptr), (len))
#define ARENA_VG_FREE(block, ptr) VALGRIND_MEMPOOL_FREE((block), (ptr))
#define ARENA_VG_CHANGE(block, ptr, len) VALGRIND_MEMPOOL_CHANGE((block), (ptr), (ptr), (len))
#define ARENA_VG_TRIM(block, ptr, len) VALGRIND_MEMPOOL_TRIM((block), (ptr), (len))
#define ARENA_VG_NOACCESS(ptr, len) ((void)VALGRIND_MAKE_MEM_NOACCESS((ptr), (len)))
#define ARENA_VG_UNDEFINED(ptr, len) ((void)VALGRIND_MAKE_MEM_UNDEFINED((ptr), (len)))
#else
#define ARENA_VG_CREATE(block) ((void)(block))
#define ARENA_VG_DESTROY(block) ((void)(block))
#define ARENA_VG_ALLOC(block, ptr, len) ((void)(block), (void)(ptr), (void)(len))
#define ARENA_VG_FREE(block, ptr) ((void)(block), (void)(ptr))
#define ARENA_VG_CHANGE(block, ptr, len) ((void)(block), (void)(ptr), (void)(len))
#define ARENA_VG_TRIM(block, ptr, len) ((void)(block), (void)(ptr), (void)(len))
#define ARENA_VG_NOACCESS(ptr, len) ((void)(ptr), (void)(len))
#define ARENA_VG_UNDEFINED(ptr, len) ((void)(ptr), (void)(len))
#endif

#if !defined(NDEBUG) && !defined(ARENA_HAS_ASAN) && !defined(ARENA_HAS_MSAN) &&                    \
    !defined(ARENA_HAS_VALGRIND)
#define ARENA_FILL_ALLOC(ptr, len) memset((ptr), 0xA5, (len))
#define ARENA_FILL_DEAD(ptr, len) memset((ptr), 0x5A, (len))
#else
#define ARENA_FILL_ALLOC(ptr, len) ((void)(ptr), (void)(len))
#define ARENA_FILL_DEAD(ptr, len) ((void)(ptr), (void)(len))
#endif

/* Under ASan every allocation carries a poisoned redzone and the shadow
 * granularity forces a minimum alignment of 8. */
#if defined(ARENA_HAS_ASAN)
#define ARENA_REDZONE ARENA_ASAN_REDZONE
#else
#define ARENA_REDZONE ((size_t)0)
#endif

/*
 * Blocks chain newest-first through the link field: the allocation chain
 * reads it as "previous (older) block", the free list reads it as "next
 * free block". Capacity checks run on integers before any pointer is
 * formed, and alignment padding is computed as an integer and applied by
 * advancing the cursor pointer, never by materializing a pointer from a
 * computed integer.
 */

enum { ARENA_FLAG_FIXED = 1 };

struct arena_block {
    struct arena_block *link;
    size_t cap;  /* usable data bytes after the header */
    size_t used; /* data bytes consumed */
    unsigned oversize;
};

/* Data begins at the header size rounded up to max_align_t, so a block
 * from a conforming parent starts its data at the default alignment. */
#define ARENA_BLOCK_HDR                                                                            \
    ((sizeof(struct arena_block) + _Alignof(max_align_t) - 1) / _Alignof(max_align_t) *            \
     _Alignof(max_align_t))

_Static_assert(ARENA_BLOCK_HDR + _Alignof(struct arena_block) - 1 <= ARENA_FIXED_OVERHEAD,
               "fixed-arena bookkeeping exceeds ARENA_FIXED_OVERHEAD");

/* The libc backend. Over-aligned requests are refused, never mis-served. */
static void *alloc_libc_xalloc(void *ctx, size_t size, size_t align);
static void *alloc_libc_xrealloc(void *ctx, void *ptr, size_t old_size, size_t new_size,
                                 size_t align);
static void alloc_libc_xfree(void *ctx, void *ptr, size_t size, size_t align);

/* The always-failing backend. */
static void *alloc_null_xalloc(void *ctx, size_t size, size_t align);
static void *alloc_null_xrealloc(void *ctx, void *ptr, size_t old_size, size_t new_size,
                                 size_t align);
static void alloc_null_xfree(void *ctx, void *ptr, size_t size, size_t align);

/* Returns the data area that follows block's header. block is a live
 * block header. */
static unsigned char *arena_block_data(struct arena_block *block);

/* True for 0 and for powers of two up to ARENA_MAX_ALIGN. */
static bool arena_align_valid(size_t align);

/* align passed arena_align_valid. Resolves 0 and the ASan floor. */
static size_t arena_align_effective(size_t align);

/* Records the first failure; an existing sticky status is preserved. */
static void arena_fail(arena_t *a, arena_status_t status);

/* False for a NULL arena (nothing to record a status on) and while a
 * failure is sticky; allocation entry points then answer NULL without
 * side effects. */
static bool arena_can_serve(const arena_t *a);

/* [ptr, ptr + size) was just carved from block: exact-size unpoison,
 * debug fill, MSan mark, mempool chunk registration, in that order. */
static void arena_mark_live(struct arena_block *block, unsigned char *ptr, size_t size);

/* Marks [keep, dead_end) of block's data dead: debug fill while still
 * addressable, then poison, then the Valgrind no-access mark. */
static void arena_mark_dead(struct arena_block *block, size_t keep, size_t dead_end);

/* Rewinds block to empty, dead-marking its whole used range. */
static void arena_rewind_block(struct arena_block *block);

/* Serves size bytes at align_eff from the current block, or returns
 * NULL without side effects when it does not fit. */
static void *arena_try_bump(arena_t *a, size_t size, size_t align_eff);

/* Unlinks and returns the first free-list block with cap >= data_need,
 * or NULL. The caller chains the block. */
static struct arena_block *arena_take_free_block(arena_t *a, size_t data_need);

/* Obtains a block holding data_need bytes from the parent: a normal
 * block of next_block bytes (doubling to max_block) when it fits, else
 * a dedicated exactly-sized oversize block. Records ARENA_ERR_ALLOC and
 * returns NULL on refusal. The caller chains the block. */
static struct arena_block *arena_new_block(arena_t *a, size_t data_need);

/* The slow path behind arena_alloc_impl: acquires capacity and bumps.
 * On failure nothing changes except the sticky status. */
static void *arena_grow_and_bump(arena_t *a, size_t size, size_t align_eff);

/* The shared front of every allocation entry point: sticky gate,
 * argument validation, overflow guards, bump, grow. */
static void *arena_alloc_impl(arena_t *a, size_t count, size_t size, size_t align);

/* Moves a dead normal block onto the free list. Its mempool anchor
 * stays alive for the block's whole lifetime; only the chunks drop. */
static void arena_retire_block(arena_t *a, struct arena_block *block);

/* Returns one block to the parent with the exact size and align of its
 * acquisition. Never called for fixed arenas. */
static void arena_release_block(arena_t *a, struct arena_block *block);

/* Releases every block on a chain to the parent. */
static void arena_release_chain(arena_t *a, struct arena_block *first);

/* Pops blocks entered since stop became current: oversize blocks return
 * to the parent, normal blocks retire to the free list. stop must still
 * be on the chain; a missing stop is a stale-temp contract violation. */
static void arena_pop_to_block(arena_t *a, struct arena_block *stop);

/* True when [ptr, ptr + size) is the most recent live allocation, in
 * the current block right at the cursor; out_offset then receives ptr's
 * block offset and is untouched otherwise. A size larger than the
 * block's consumption is answered false before any sum can wrap, so
 * garbage sizes stay a no-op for the callers. The membership test runs
 * in integer space: a relational pointer comparison against another
 * block's data would be undefined. */
static bool arena_is_last_alloc(const arena_t *a, const void *ptr, size_t size, size_t *out_offset);

/* Resizes ptr in place when it is the last allocation and the block can
 * absorb the change. Returns NULL, with nothing changed, when the move
 * path must run instead. */
static void *arena_try_resize_last(arena_t *a, void *ptr, size_t old_size, size_t new_size);

/* The realloc move path: allocate, copy min(old_size, new_size) bytes,
 * abandon the old region. Failure records the status and leaves the old
 * block valid. */
static void *arena_realloc_move(arena_t *a, void *ptr, size_t old_size, size_t new_size,
                                size_t align);

/* The alloc_t adapter over an arena. xfree rolls the cursor back for
 * the most recent live allocation and is a harmless no-op otherwise;
 * the alignment padding consumed in front of a rolled-back allocation
 * stays consumed. */
static void *arena_adapter_xalloc(void *ctx, size_t size, size_t align);
static void *arena_adapter_xrealloc(void *ctx, void *ptr, size_t old_size, size_t new_size,
                                    size_t align);
static void arena_adapter_xfree(void *ctx, void *ptr, size_t size, size_t align);

alloc_t alloc_libc(void)
{
    alloc_t backend = {NULL, alloc_libc_xalloc, alloc_libc_xrealloc, alloc_libc_xfree};

    return backend;
}

alloc_t alloc_null(void)
{
    alloc_t backend = {NULL, alloc_null_xalloc, alloc_null_xrealloc, alloc_null_xfree};

    return backend;
}

void arena_init(arena_t *a, alloc_t parent)
{
    if (a == NULL) {
        return;
    }
    a->parent = parent;
    a->current = NULL;
    a->free_blocks = NULL;
    a->min_block = ARENA_MIN_BLOCK_DEFAULT;
    a->max_block = ARENA_MAX_BLOCK_DEFAULT;
    a->next_block = ARENA_MIN_BLOCK_DEFAULT;
    a->used = 0;
    a->committed = 0;
    a->high_water = 0;
    a->temp_depth = 0;
    a->generation = 0;
    a->flags = 0;
    a->status = ARENA_OK;
}

arena_status_t arena_init_sized(arena_t *a, alloc_t parent, size_t min_block, size_t max_block)
{
    if (a == NULL) {
        return ARENA_ERR_ARG;
    }
    arena_init(a, parent);
    if (min_block < ARENA_FIXED_OVERHEAD || min_block > max_block ||
        max_block > (size_t)PTRDIFF_MAX) {
        arena_fail(a, ARENA_ERR_ARG);
        return ARENA_ERR_ARG;
    }
    a->min_block = min_block;
    a->max_block = max_block;
    a->next_block = min_block;
    return ARENA_OK;
}

void arena_init_fixed(arena_t *a, void *buffer, size_t size)
{
    if (a == NULL) {
        return;
    }
    arena_init(a, alloc_null());
    a->flags = ARENA_FLAG_FIXED;
    a->min_block = 0;
    a->max_block = 0;
    a->next_block = 0;
    if (buffer == NULL || size == 0) {
        arena_fail(a, ARENA_ERR_ARG);
        return;
    }
    if (size > (size_t)PTRDIFF_MAX) {
        /* A larger buffer would let block offsets exceed PTRDIFF_MAX,
         * reopening the wrap hazards every other entry point caps. */
        arena_fail(a, ARENA_ERR_OVERFLOW);
        return;
    }
    a->committed = size;
    unsigned char *base = buffer;
#if defined(ARENA_STRICT_ISO) && (defined(__GNUC__) || defined(__clang__))
    /* Severs the compiler's view of the buffer's declared type at the
     * one point where caller storage becomes arena storage. */
    __asm__ volatile("" : "+r"(base));
#endif
    size_t pad = (size_t)(-(uintptr_t)base & (uintptr_t)(_Alignof(struct arena_block) - 1));
    if (size < pad || size - pad < ARENA_BLOCK_HDR) {
        /* A valid arena with zero capacity; allocations fail with
         * ARENA_ERR_ALLOC. */
        return;
    }
    struct arena_block *block = (struct arena_block *)(void *)(base + pad);
    block->link = NULL;
    block->cap = size - pad - ARENA_BLOCK_HDR;
    block->used = 0;
    block->oversize = 0;
    a->current = block;
    ARENA_VG_CREATE(block);
    ARENA_POISON(arena_block_data(block), block->cap);
    ARENA_VG_NOACCESS(arena_block_data(block), block->cap);
}

void arena_deinit(arena_t *a)
{
    if (a == NULL) {
        return;
    }
    alloc_t parent = a->parent;
    unsigned generation = a->generation + 1u;
    if ((a->flags & ARENA_FLAG_FIXED) == 0) {
        arena_release_chain(a, a->current);
        arena_release_chain(a, a->free_blocks);
    } else if (a->current != NULL) {
        /* The buffer returns to its owner: fully addressable again, with
         * unspecified contents. */
        ARENA_UNPOISON(arena_block_data(a->current), a->current->cap);
        ARENA_VG_DESTROY(a->current);
        ARENA_VG_UNDEFINED(arena_block_data(a->current), a->current->cap);
    }
    arena_init(a, parent);
    a->generation = generation;
}

void arena_reset(arena_t *a)
{
    if (a == NULL) {
        return;
    }
    if ((a->flags & ARENA_FLAG_FIXED) != 0) {
        if (a->current != NULL) {
            arena_rewind_block(a->current);
        }
    } else {
        /* The oldest normal block stays current; other normal blocks
         * move to the free list; oversize blocks return to the parent. */
        struct arena_block *oldest_normal = NULL;
        struct arena_block *next;
        for (struct arena_block *block = a->current; block != NULL; block = block->link) {
            if (block->oversize == 0) {
                oldest_normal = block;
            }
        }
        for (struct arena_block *block = a->current; block != NULL; block = next) {
            next = block->link;
            if (block == oldest_normal) {
                continue;
            }
            if (block->oversize != 0) {
                arena_release_block(a, block);
            } else {
                arena_retire_block(a, block);
            }
        }
        if (oldest_normal != NULL) {
            arena_rewind_block(oldest_normal);
            oldest_normal->link = NULL;
        }
        a->current = oldest_normal;
    }
    a->used = 0;
    a->temp_depth = 0;
    a->generation += 1u;
    a->status = ARENA_OK;
}

void arena_trim(arena_t *a)
{
    if (a == NULL || (a->flags & ARENA_FLAG_FIXED) != 0) {
        return;
    }
    arena_release_chain(a, a->free_blocks);
    a->free_blocks = NULL;
}

void arena_clear_error(arena_t *a)
{
    if (a != NULL) {
        a->status = ARENA_OK;
    }
}

void *arena_alloc(arena_t *a, size_t size)
{
    return arena_alloc_impl(a, 1, size, 0);
}

void *arena_alloc_zeroed(arena_t *a, size_t size)
{
    void *ptr = arena_alloc_impl(a, 1, size, 0);

    if (ptr != NULL) {
        memset(ptr, 0, size);
    }
    return ptr;
}

void *arena_alloc_n(arena_t *a, size_t count, size_t size, size_t align)
{
    return arena_alloc_impl(a, count, size, align);
}

void *arena_alloc_n_zeroed(arena_t *a, size_t count, size_t size, size_t align)
{
    void *ptr = arena_alloc_impl(a, count, size, align);

    if (ptr != NULL) {
        memset(ptr, 0, count * size);
    }
    return ptr;
}

void *arena_memdup(arena_t *a, const void *src, size_t size)
{
    if (!arena_can_serve(a)) {
        return NULL;
    }
    if (src == NULL && size != 0) {
        arena_fail(a, ARENA_ERR_ARG);
        return NULL;
    }
    void *ptr = arena_alloc_impl(a, 1, size, 0);
    if (ptr != NULL) {
        memcpy(ptr, src, size);
    }
    return ptr;
}

void *arena_realloc(arena_t *a, void *ptr, size_t old_size, size_t new_size, size_t align)
{
    if (!arena_can_serve(a)) {
        return NULL;
    }
    if (!arena_align_valid(align)) {
        arena_fail(a, ARENA_ERR_ARG);
        return NULL;
    }
    if (new_size == 0) {
        /* Freeing through realloc is forbidden by the interface; refuse
         * with a recorded status instead of inheriting realloc(p, 0)
         * divergence. */
        arena_fail(a, ARENA_ERR_ARG);
        return NULL;
    }
    assert(ptr != NULL || old_size == 0);
    if (ptr == NULL) {
        return arena_alloc_impl(a, 1, new_size, align);
    }
    if (new_size > (size_t)PTRDIFF_MAX) {
        arena_fail(a, ARENA_ERR_OVERFLOW);
        return NULL;
    }
    void *resized = arena_try_resize_last(a, ptr, old_size, new_size);
    if (resized != NULL) {
        return resized;
    }
    return arena_realloc_move(a, ptr, old_size, new_size, align);
}

arena_temp_t arena_temp_begin(arena_t *a)
{
    arena_temp_t temp;

    temp.arena = a;
    temp.block = NULL;
    temp.block_used = 0;
    temp.used = 0;
    temp.generation = 0;
    temp.depth = 0;
    if (a == NULL) {
        return temp;
    }
    a->temp_depth += 1u;
    temp.block = a->current;
    temp.block_used = a->current != NULL ? a->current->used : 0;
    temp.used = a->used;
    temp.generation = a->generation;
    temp.depth = a->temp_depth;
    return temp;
}

void arena_temp_end(arena_temp_t temp)
{
    arena_t *a = temp.arena;

    if (a == NULL) {
        return;
    }
    /* Stale temps, and in-scope releases or resizes of allocations made
     * before the scope began, are contract violations; these checks are
     * the debug detection the header promises. Release builds never
     * raise a cursor above its rewound position, so freed bytes are not
     * resurrected. */
    assert(temp.generation == a->generation);
    assert(temp.depth == a->temp_depth);
    assert(a->used >= temp.used);
    arena_pop_to_block(a, temp.block);
    struct arena_block *block = a->current;
    if (block != NULL) {
        assert(block->used >= temp.block_used);
        if (block->used > temp.block_used) {
            arena_mark_dead(block, temp.block_used, block->used);
            ARENA_VG_TRIM(block, arena_block_data(block), temp.block_used);
            block->used = temp.block_used;
        }
    }
    if (a->used > temp.used) {
        a->used = temp.used;
    }
    if (a->temp_depth > 0) {
        a->temp_depth -= 1u;
    }
}

alloc_t arena_allocator(arena_t *a)
{
    alloc_t backend = {a, arena_adapter_xalloc, arena_adapter_xrealloc, arena_adapter_xfree};

    if (a == NULL) {
        return alloc_null();
    }
    return backend;
}

const char *arena_status_name(arena_status_t status)
{
    switch (status) {
    case ARENA_OK:
        return "ARENA_OK";
    case ARENA_ERR_ARG:
        return "ARENA_ERR_ARG";
    case ARENA_ERR_ALLOC:
        return "ARENA_ERR_ALLOC";
    case ARENA_ERR_OVERFLOW:
        return "ARENA_ERR_OVERFLOW";
    default:
        return "ARENA_ERR_UNKNOWN";
    }
}

arena_status_t arena_status(const arena_t *a)
{
    return a == NULL ? ARENA_ERR_ARG : a->status;
}

bool arena_ok(const arena_t *a)
{
    return a != NULL && a->status == ARENA_OK;
}

bool arena_failed(const arena_t *a)
{
    return a == NULL || a->status != ARENA_OK;
}

size_t arena_used(const arena_t *a)
{
    return a == NULL ? 0 : a->used;
}

size_t arena_committed(const arena_t *a)
{
    return a == NULL ? 0 : a->committed;
}

size_t arena_high_water(const arena_t *a)
{
    return a == NULL ? 0 : a->high_water;
}

static void *alloc_libc_xalloc(void *ctx, size_t size, size_t align)
{
    (void)ctx;
    if (size == 0 || size > (size_t)PTRDIFF_MAX) {
        return NULL;
    }
    if (align > _Alignof(max_align_t)) {
        return NULL;
    }
    return malloc(size);
}

static void *alloc_libc_xrealloc(void *ctx, void *ptr, size_t old_size, size_t new_size,
                                 size_t align)
{
    (void)ctx;
    if (new_size == 0 || new_size > (size_t)PTRDIFF_MAX) {
        return NULL;
    }
    if (align > _Alignof(max_align_t)) {
        return NULL;
    }
    if (ptr == NULL) {
        return malloc(new_size);
    }
    void *grown = realloc(ptr, new_size);
    if (grown == NULL && new_size <= old_size) {
        /* ISO C lets realloc fail even when shrinking. The old block
         * already satisfies any smaller request, which keeps the
         * header's shrinks-never-fail promise. */
        return ptr;
    }
    return grown;
}

static void alloc_libc_xfree(void *ctx, void *ptr, size_t size, size_t align)
{
    (void)ctx;
    (void)size;
    (void)align;
    free(ptr);
}

static void *alloc_null_xalloc(void *ctx, size_t size, size_t align)
{
    (void)ctx;
    (void)size;
    (void)align;
    return NULL;
}

static void *alloc_null_xrealloc(void *ctx, void *ptr, size_t old_size, size_t new_size,
                                 size_t align)
{
    (void)ctx;
    (void)ptr;
    (void)old_size;
    (void)new_size;
    (void)align;
    return NULL;
}

static void alloc_null_xfree(void *ctx, void *ptr, size_t size, size_t align)
{
    (void)ctx;
    (void)ptr;
    (void)size;
    (void)align;
}

static unsigned char *arena_block_data(struct arena_block *block)
{
    assert(block != NULL);
    return (unsigned char *)(void *)block + ARENA_BLOCK_HDR;
}

static bool arena_align_valid(size_t align)
{
    return (align & (align - 1)) == 0 && align <= ARENA_MAX_ALIGN;
}

static size_t arena_align_effective(size_t align)
{
    size_t effective = align == 0 ? ARENA_DEFAULT_ALIGN : align;

#if defined(ARENA_HAS_ASAN)
    if (effective < 8) {
        effective = 8;
    }
#endif
    return effective;
}

static void arena_fail(arena_t *a, arena_status_t status)
{
    assert(a != NULL);
    if (a->status == ARENA_OK) {
        a->status = status;
    }
}

static bool arena_can_serve(const arena_t *a)
{
    return a != NULL && a->status == ARENA_OK;
}

static void arena_mark_live(struct arena_block *block, unsigned char *ptr, size_t size)
{
    assert(block != NULL && ptr != NULL);
    ARENA_UNPOISON(ptr, size); /* exact user size, never padded */
    ARENA_FILL_ALLOC(ptr, size);
    ARENA_MSAN_ALLOCATED(ptr, size);
    ARENA_VG_ALLOC(block, ptr, size);
}

static void arena_mark_dead(struct arena_block *block, size_t keep, size_t dead_end)
{
    unsigned char *data = arena_block_data(block);

    if (dead_end <= keep) {
        return;
    }
    ARENA_FILL_DEAD(data + keep, dead_end - keep);
    ARENA_POISON(data + keep, dead_end - keep);
    ARENA_VG_NOACCESS(data + keep, dead_end - keep);
}

static void arena_rewind_block(struct arena_block *block)
{
    arena_mark_dead(block, 0, block->used);
    ARENA_VG_TRIM(block, arena_block_data(block), 0);
    block->used = 0;
}

static void *arena_try_bump(arena_t *a, size_t size, size_t align_eff)
{
    struct arena_block *block;

    assert(a != NULL);
    block = a->current;
    if (block == NULL) {
        return NULL;
    }
    unsigned char *cursor = arena_block_data(block) + block->used;
    size_t pad = (size_t)(-(uintptr_t)cursor & (uintptr_t)(align_eff - 1));
    size_t avail = block->cap - block->used;
    size_t total = size + ARENA_REDZONE;
    if (pad > avail || total > avail - pad) {
        return NULL;
    }
    block->used += pad + total;
    a->used += pad + total;
    if (a->used > a->high_water) {
        a->high_water = a->used;
    }
    arena_mark_live(block, cursor + pad, size);
    return cursor + pad;
}

static struct arena_block *arena_take_free_block(arena_t *a, size_t data_need)
{
    for (struct arena_block **linkp = &a->free_blocks; *linkp != NULL; linkp = &(*linkp)->link) {
        if ((*linkp)->cap >= data_need) {
            struct arena_block *block = *linkp;
            *linkp = block->link;
            return block;
        }
    }
    return NULL;
}

static struct arena_block *arena_new_block(arena_t *a, size_t data_need)
{
    size_t target = a->next_block;
    size_t block_size;

    if (ARENA_BLOCK_HDR + data_need <= target) {
        block_size = target;
    } else {
        block_size = ARENA_BLOCK_HDR + data_need;
    }
    struct arena_block *block = alloc_alloc(&a->parent, block_size, 0);
    if (block == NULL) {
        arena_fail(a, ARENA_ERR_ALLOC);
        return NULL;
    }
    block->cap = block_size - ARENA_BLOCK_HDR;
    block->used = 0;
    block->oversize = (unsigned)(block_size != target);
    if (block->oversize == 0) {
        a->next_block = target * 2 > a->max_block ? a->max_block : target * 2;
    }
    a->committed += block_size;
    ARENA_VG_CREATE(block);
    ARENA_POISON(arena_block_data(block), block->cap);
    ARENA_VG_NOACCESS(arena_block_data(block), block->cap);
    return block;
}

static void *arena_grow_and_bump(arena_t *a, size_t size, size_t align_eff)
{
    if ((a->flags & ARENA_FLAG_FIXED) != 0) {
        arena_fail(a, ARENA_ERR_ALLOC);
        return NULL;
    }
    /* Worst-case data bytes: request plus alignment slack plus redzone.
     * size is capped at PTRDIFF_MAX and align_eff at ARENA_MAX_ALIGN, so
     * this sum cannot wrap size_t. */
    size_t data_need = size + (align_eff - 1) + ARENA_REDZONE;
    if (data_need > (size_t)PTRDIFF_MAX - ARENA_BLOCK_HDR) {
        arena_fail(a, ARENA_ERR_OVERFLOW);
        return NULL;
    }
    /* First fit over retained blocks before asking the parent. Retained
     * data is already dead-marked; only the pool anchor returns. */
    struct arena_block *block = arena_take_free_block(a, data_need);
    if (block == NULL) {
        block = arena_new_block(a, data_need);
        if (block == NULL) {
            return NULL;
        }
    }
    block->link = a->current;
    a->current = block;
    void *ptr = arena_try_bump(a, size, align_eff);
    assert(ptr != NULL); /* cap covers worst-case padding */
    return ptr;
}

static void *arena_alloc_impl(arena_t *a, size_t count, size_t size, size_t align)
{
    if (!arena_can_serve(a)) {
        return NULL;
    }
    if (!arena_align_valid(align)) {
        arena_fail(a, ARENA_ERR_ARG);
        return NULL;
    }
    if (count == 0 || size == 0) {
        return NULL;
    }
    if (count > (size_t)PTRDIFF_MAX / size) {
        arena_fail(a, ARENA_ERR_OVERFLOW);
        return NULL;
    }
    size_t total = count * size;
    size_t align_eff = arena_align_effective(align);
    void *ptr = arena_try_bump(a, total, align_eff);
    if (ptr != NULL) {
        return ptr;
    }
    return arena_grow_and_bump(a, total, align_eff);
}

static void arena_retire_block(arena_t *a, struct arena_block *block)
{
    arena_rewind_block(block);
    block->link = a->free_blocks;
    a->free_blocks = block;
}

static void arena_release_block(arena_t *a, struct arena_block *block)
{
    size_t block_size = ARENA_BLOCK_HDR + block->cap;

    ARENA_FILL_DEAD(arena_block_data(block), block->used);
    ARENA_VG_DESTROY(block);
    a->committed -= block_size;
    alloc_free(&a->parent, block, block_size, 0);
}

static void arena_release_chain(arena_t *a, struct arena_block *first)
{
    struct arena_block *next;

    for (struct arena_block *block = first; block != NULL; block = next) {
        next = block->link;
        arena_release_block(a, block);
    }
}

static void arena_pop_to_block(arena_t *a, struct arena_block *stop)
{
    struct arena_block *block = a->current;

    while (block != NULL && block != stop) {
        struct arena_block *next = block->link;
        if (block->oversize != 0) {
            arena_release_block(a, block);
        } else {
            arena_retire_block(a, block);
        }
        block = next;
    }
    assert(block == stop);
    a->current = block;
}

static bool arena_is_last_alloc(const arena_t *a, const void *ptr, size_t size, size_t *out_offset)
{
    struct arena_block *block;

    assert(a != NULL && ptr != NULL && out_offset != NULL);
    block = a->current;
    if (block == NULL || size > block->used) {
        return false;
    }
    uintptr_t ptr_addr = (uintptr_t)ptr;
    uintptr_t data_addr = (uintptr_t)arena_block_data(block);
    if (ptr_addr < data_addr || ptr_addr - data_addr > block->cap) {
        return false;
    }
    size_t offset = (size_t)(ptr_addr - data_addr);
    if (offset + size + ARENA_REDZONE != block->used) {
        return false;
    }
    *out_offset = offset;
    return true;
}

static void *arena_try_resize_last(arena_t *a, void *ptr, size_t old_size, size_t new_size)
{
    struct arena_block *block = a->current;
    size_t offset;

    if (!arena_is_last_alloc(a, ptr, old_size, &offset)) {
        return NULL;
    }
    if (new_size <= old_size) {
        block->used -= old_size - new_size;
        a->used -= old_size - new_size;
        ARENA_VG_CHANGE(block, ptr, new_size);
        arena_mark_dead(block, offset + new_size, offset + old_size);
        return ptr;
    }
    size_t growth = new_size - old_size;
    if (growth > block->cap - block->used) {
        return NULL; /* does not fit in place; the move path runs */
    }
    block->used += growth;
    a->used += growth;
    if (a->used > a->high_water) {
        a->high_water = a->used;
    }
    /* The extension must become addressable on this path too, not only
     * on the move path. */
    unsigned char *extension = (unsigned char *)ptr + old_size;
    ARENA_UNPOISON(extension, growth);
    ARENA_FILL_ALLOC(extension, growth);
    ARENA_MSAN_ALLOCATED(extension, growth);
    ARENA_VG_CHANGE(block, ptr, new_size);
    ARENA_VG_UNDEFINED(extension, growth);
    return ptr;
}

static void *arena_realloc_move(arena_t *a, void *ptr, size_t old_size, size_t new_size,
                                size_t align)
{
    void *fresh = arena_alloc_impl(a, 1, new_size, align);

    if (fresh == NULL) {
        return NULL; /* status recorded; the old block stays valid */
    }
    memcpy(fresh, ptr, old_size < new_size ? old_size : new_size);
    /* The abandoned source is dead; its padding and redzone stay
     * counted until rewind. */
    ARENA_FILL_DEAD(ptr, old_size);
    ARENA_POISON(ptr, old_size);
    ARENA_VG_NOACCESS(ptr, old_size);
    a->used -= old_size;
    return fresh;
}

static void *arena_adapter_xalloc(void *ctx, size_t size, size_t align)
{
    return arena_alloc_n(ctx, 1, size, align);
}

static void *arena_adapter_xrealloc(void *ctx, void *ptr, size_t old_size, size_t new_size,
                                    size_t align)
{
    return arena_realloc(ctx, ptr, old_size, new_size, align);
}

static void arena_adapter_xfree(void *ctx, void *ptr, size_t size, size_t align)
{
    arena_t *a = ctx;
    size_t offset;

    (void)align;
    if (a == NULL || ptr == NULL) {
        return;
    }
    if (!arena_is_last_alloc(a, ptr, size, &offset)) {
        return;
    }
    ARENA_VG_FREE(a->current, ptr);
    arena_mark_dead(a->current, offset, a->current->used);
    a->current->used = offset;
    a->used -= size + ARENA_REDZONE;
}
