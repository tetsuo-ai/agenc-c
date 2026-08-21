#ifndef AGENC_ALLOC_H_INCLUDED
#define AGENC_ALLOC_H_INCLUDED

#include <assert.h>
#include <stddef.h>
#include <string.h>

/*
 * alloc.h: the allocator interface for the agenc-* C library family.
 *
 * An alloc_t is a value: a context pointer plus three function pointers,
 * copied freely and stored by value inside any object that will allocate
 * later. Two allocators are the same allocator if and only if all four
 * members compare equal. Every block must be released through an
 * allocator equal to the one that allocated it. Consumers store the
 * allocator they were created with and use it for all later calls; that
 * stored copy is the identity.
 *
 * The member names carry an x prefix because C11 7.1.4 permits stdlib
 * functions to be masked by function-like macros, and a member named
 * "free" would break under such a macro. All three function pointers
 * must be non-NULL; there is no optional-member probing.
 *
 * Alignment rules shared by all three functions:
 * - align is 0 or a power of two. A non-power-of-two align is a contract
 *   violation; the inline call helpers below assert it in debug builds.
 * - 0 means the default alignment, _Alignof(max_align_t): the block is
 *   suitably aligned for any object of the requested size or smaller.
 * - Every implementation supports all powers of two up to and including
 *   _Alignof(max_align_t). Larger values may be unsupported; an
 *   unsupported alignment fails with NULL.
 * - On xrealloc and xfree the caller passes the same align as the call
 *   that allocated the block, so implementations never store alignment.
 *
 * Thread safety is a property of the backend plus ctx pair, never of
 * this struct: the struct value is immutable data, readable from any
 * thread. The libc backend is thread-safe; an arena backend is
 * single-owner and requires external synchronization if shared. A
 * library that receives an allocator calls it only during the execution
 * of its own API functions, on the calling thread, and must not assume
 * the allocator synchronizes. Allocator implementations must not call
 * back into the library that invoked them and are not async-signal-safe
 * unless a backend documents otherwise.
 *
 * This struct is frozen. If the family ever needs an in-place resize
 * probe, it ships as a new struct type with a conversion helper, never
 * as a mutation of this layout.
 */

/*
 * Allocates size bytes aligned to align.
 * size 0 must return NULL; that is success, no allocation was performed
 * and nothing needs releasing, so size != 0 with a NULL result is the
 * complete failure test. size > 0 returns a pointer to at least size
 * bytes whose address is a multiple of the effective alignment, or NULL
 * if and only if the request cannot be fulfilled (out of memory,
 * unsupported alignment, or internally unrepresentable size; callers
 * cannot distinguish these). The memory is uninitialized. Requests
 * larger than PTRDIFF_MAX must fail with NULL. The block stays valid
 * until passed to xfree or xrealloc of an equal allocator, or until the
 * owner of the instance behind ctx resets or destroys it, whichever
 * comes first. On failure the allocator remains valid and the call
 * returns normally: no abort, no longjmp, no exit.
 */
typedef void *(*alloc_xalloc_fn)(void *ctx, size_t size, size_t align);

/*
 * Changes the size of a block from old_size to new_size.
 * Preconditions, contract violations otherwise: new_size is not 0
 * (freeing through realloc is forbidden; xfree exists); align equals the
 * align of the call that allocated ptr; a NULL ptr requires old_size 0,
 * and the call then behaves exactly like xalloc(ctx, new_size, align); a
 * non-NULL ptr came from xalloc or xrealloc of an equal allocator, has
 * not been freed, and old_size equals exactly the size established by
 * the most recent allocating call for this block.
 * Success returns a pointer, possibly equal to ptr and possibly moved,
 * to at least new_size bytes at the same effective alignment; bytes
 * [0, min(old_size, new_size)) are preserved, bytes beyond old_size are
 * uninitialized, and a moved block invalidates the old pointer. Failure
 * returns NULL and the old block is untouched, still valid, still owned
 * by the caller, and must still be released eventually. Shrinking may
 * fail like any other call, but a failed shrink is always recoverable
 * because the old block still holds all the data; the built-in backends
 * never fail a shrink. Implementations should resize in place when they
 * can; relocation is always permitted.
 */
typedef void *(*alloc_xrealloc_fn)(void *ctx, void *ptr, size_t old_size, size_t new_size,
                                   size_t align);

/*
 * Releases a block.
 * A NULL ptr must return with no effect regardless of the other
 * arguments, keeping cleanup ladders unconditional. A non-NULL ptr came
 * from xalloc or xrealloc of an equal allocator and has not been freed;
 * size equals exactly the size established by the most recent allocating
 * call; align equals the original align. xfree releases the caller's
 * claim; it does not promise immediate reuse. An arena backend may make
 * it a no-op, or roll the cursor back when the block is the most recent
 * allocation.
 */
typedef void (*alloc_xfree_fn)(void *ctx, void *ptr, size_t size, size_t align);

typedef struct alloc {
    void *ctx;
    alloc_xalloc_fn xalloc;
    alloc_xrealloc_fn xrealloc;
    alloc_xfree_fn xfree;
} alloc_t;

/*
 * The malloc/realloc/free backend. Thread-safe as malloc is. Supports
 * align up to _Alignof(max_align_t) only; larger alignments fail with
 * NULL (over-aligned callers use an arena). Ignores the size argument on
 * xfree. Never fails a shrink.
 */
alloc_t alloc_libc(void);

/*
 * The always-failing backend: xalloc and xrealloc return NULL for every
 * request, xfree ignores its arguments entirely. Used for heap-disabled
 * runs and as the parent of fixed arenas.
 */
alloc_t alloc_null(void);

/*
 * Inline call helpers. Every call site in the family goes through these
 * so the interface preconditions are asserted in one place in debug
 * builds. They add no behavior of their own beyond the assertions and
 * alloc_zeroed's memset.
 */

static inline void *alloc_alloc(const alloc_t *a, size_t size, size_t align)
{
    assert(a != NULL && a->xalloc != NULL);
    assert((align & (align - 1)) == 0);
    return a->xalloc(a->ctx, size, align);
}

static inline void *alloc_realloc(const alloc_t *a, void *ptr, size_t old_size, size_t new_size,
                                  size_t align)
{
    assert(a != NULL && a->xrealloc != NULL);
    assert((align & (align - 1)) == 0);
    assert(new_size != 0);
    assert(ptr != NULL || old_size == 0);
    return a->xrealloc(a->ctx, ptr, old_size, new_size, align);
}

static inline void alloc_free(const alloc_t *a, void *ptr, size_t size, size_t align)
{
    assert(a != NULL && a->xfree != NULL);
    assert((align & (align - 1)) == 0);
    a->xfree(a->ctx, ptr, size, align);
}

/* alloc_alloc plus memset to zero on success. Zeroing is deliberately
 * outside the vtable; see SPEC.md section 1.7. */
static inline void *alloc_zeroed(const alloc_t *a, size_t size, size_t align)
{
    void *ptr = alloc_alloc(a, size, align);

    if (ptr != NULL) {
        memset(ptr, 0, size);
    }
    return ptr;
}

#endif /* AGENC_ALLOC_H_INCLUDED */
