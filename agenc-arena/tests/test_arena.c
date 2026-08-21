/* test_arena.c: driver for the alloc interface and arena modules. */

#if !defined(NDEBUG) && defined(__unix__)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alloc.h"
#include "arena.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define TEST_HAS_ASAN 1
#endif
#endif
#if !defined(TEST_HAS_ASAN) && defined(__SANITIZE_ADDRESS__)
#define TEST_HAS_ASAN 1
#endif

enum { TEST_SMALL = 16, TEST_MEDIUM = 64, TEST_LARGE = 128, TEST_FILL_BYTE = 0x5C };

/* The public constants are stable API; pin them. */
_Static_assert(ARENA_MIN_BLOCK_DEFAULT == 4096, "stable API");
_Static_assert(ARENA_MAX_BLOCK_DEFAULT == 65536, "stable API");
_Static_assert(ARENA_DEFAULT_ALIGN == _Alignof(max_align_t), "stable API");
_Static_assert(ARENA_MAX_ALIGN == 65536, "stable API");
_Static_assert(ARENA_FIXED_OVERHEAD == 64, "stable API");
_Static_assert(ARENA_ASAN_REDZONE == 16, "stable API");

static struct {
    int run;
} test_ctx;

static void test_expect(bool cond, const char *file, int line, const char *expr)
{
    test_ctx.run++;
    if (cond) {
        return;
    }
    fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
    abort();
}

#define EXPECT(cond) test_expect((bool)(cond), __FILE__, __LINE__, #cond)

static bool test_is_aligned(const void *ptr, size_t align)
{
    return ((uintptr_t)ptr % (uintptr_t)align) == 0;
}

/* T6: the always-failing backend. Heap-free, runs in every build. */
static void test_alloc_null_contract(void)
{
    alloc_t na = alloc_null();
    static char foreign[TEST_SMALL];

    EXPECT(alloc_alloc(&na, 0, 0) == NULL);
    EXPECT(alloc_alloc(&na, TEST_MEDIUM, 0) == NULL);
    EXPECT(alloc_alloc(&na, TEST_MEDIUM, TEST_SMALL) == NULL);
    EXPECT(alloc_realloc(&na, NULL, 0, TEST_SMALL, 0) == NULL);
    EXPECT(alloc_zeroed(&na, TEST_MEDIUM, 0) == NULL);
    alloc_free(&na, NULL, 0, 0);
    alloc_free(&na, foreign, sizeof(foreign), 0); /* documented no-op */
}

/* T1/T3: fixed-arena basics: alignment, disjointness, content survival,
 * exhaustion, recovery. Heap-free. */
static void test_fixed_basics(void)
{
    static _Alignas(max_align_t) unsigned char buffer[4096];
    arena_t a;
    unsigned char *first;
    unsigned char *second;
    int *number;
    size_t idx;
    void *big;
    void *small;

    arena_init_fixed(&a, buffer, sizeof(buffer));
    EXPECT(arena_ok(&a));
    EXPECT(arena_committed(&a) == sizeof(buffer));
    EXPECT(arena_used(&a) == 0);

    first = arena_alloc(&a, TEST_SMALL);
    EXPECT(first != NULL);
    EXPECT(test_is_aligned(first, ARENA_DEFAULT_ALIGN));
    memset(first, TEST_FILL_BYTE, TEST_SMALL);

    number = ARENA_NEW(&a, int);
    EXPECT(number != NULL);
    EXPECT(test_is_aligned(number, _Alignof(int)));
    EXPECT(*number == 0); /* macro zeroes */

    second = arena_alloc_n(&a, 4, TEST_SMALL, TEST_SMALL);
    EXPECT(second != NULL);
    EXPECT(test_is_aligned(second, TEST_SMALL));

    /* Disjoint from the first allocation. */
    EXPECT(second >= first + TEST_SMALL || second + 4 * TEST_SMALL <= first);

    /* Earlier contents survive later allocations. */
    for (idx = 0; idx < TEST_SMALL; idx++) {
        EXPECT(first[idx] == TEST_FILL_BYTE);
    }

    EXPECT(arena_used(&a) > 0);
    EXPECT(arena_used(&a) <= arena_committed(&a));
    EXPECT(arena_high_water(&a) == arena_used(&a));

    /* Exhaustion records ARENA_ERR_ALLOC and gates later calls. */
    big = arena_alloc(&a, sizeof(buffer));
    EXPECT(big == NULL);
    EXPECT(arena_failed(&a));
    EXPECT(arena_status(&a) == ARENA_ERR_ALLOC);
    small = arena_alloc(&a, 1);
    EXPECT(small == NULL); /* sticky gate */

    /* Recovery: clear the error, small allocations work again. */
    arena_clear_error(&a);
    EXPECT(arena_ok(&a));
    small = arena_alloc(&a, 1);
    EXPECT(small != NULL);

    arena_deinit(&a);
}

/* T1: deliberately misaligned buffer starts. */
static void test_fixed_misaligned_starts(void)
{
    static _Alignas(max_align_t) unsigned char storage[512 + 16];
    arena_t a;
    size_t offset;
    void *ptr;

    for (offset = 1; offset < 16; offset++) {
        arena_init_fixed(&a, storage + offset, 512);
        EXPECT(arena_ok(&a));
        ptr = arena_alloc(&a, TEST_SMALL);
        EXPECT(ptr != NULL);
        EXPECT(test_is_aligned(ptr, ARENA_DEFAULT_ALIGN));
        ptr = arena_alloc_n(&a, 1, TEST_SMALL, 64);
        EXPECT(ptr != NULL);
        EXPECT(test_is_aligned(ptr, 64));
        arena_deinit(&a);
    }
}

/* T1: tiny and boundary buffer sizes stay clean. */
static void test_fixed_tiny_buffers(void)
{
    static _Alignas(max_align_t) unsigned char storage[ARENA_FIXED_OVERHEAD + 64];
    volatile size_t fixed_over_cap = (size_t)PTRDIFF_MAX + 1;
    arena_t a;
    void *ptr;
    size_t size;

    /* Zero size and NULL buffer record ARENA_ERR_ARG. */
    arena_init_fixed(&a, storage, 0);
    EXPECT(arena_status(&a) == ARENA_ERR_ARG);
    arena_init_fixed(&a, NULL, 64);
    EXPECT(arena_status(&a) == ARENA_ERR_ARG);

    /* Sizes through the worst-case overhead boundary: always a valid
     * arena; the real header cost can be smaller, so the allocation
     * either succeeds cleanly or fails cleanly with ARENA_ERR_ALLOC. */
    for (size = 1; size <= ARENA_FIXED_OVERHEAD + 1; size++) {
        arena_init_fixed(&a, storage, size);
        EXPECT(arena_ok(&a));
        ptr = arena_alloc(&a, 1);
        if (ptr == NULL) {
            EXPECT(arena_status(&a) == ARENA_ERR_ALLOC);
        } else {
            EXPECT(arena_ok(&a));
        }
        arena_deinit(&a);
    }

    /* Overhead plus slack serves a small allocation. */
    arena_init_fixed(&a, storage, sizeof(storage));
    EXPECT(arena_ok(&a));
    ptr = arena_alloc(&a, 8);
    EXPECT(ptr != NULL);
    arena_deinit(&a);

    /* A size above PTRDIFF_MAX is refused before any buffer access. */
    arena_init_fixed(&a, storage, fixed_over_cap);
    EXPECT(arena_status(&a) == ARENA_ERR_OVERFLOW);
    arena_deinit(&a);
}

/* T3: the zero-size table: NULL as success, no status recorded. */
static void test_zero_size_table(void)
{
    static _Alignas(max_align_t) unsigned char buffer[256];
    arena_t a;

    arena_init_fixed(&a, buffer, sizeof(buffer));
    EXPECT(arena_alloc(&a, 0) == NULL);
    EXPECT(arena_alloc_zeroed(&a, 0) == NULL);
    EXPECT(arena_alloc_n(&a, 0, TEST_SMALL, 0) == NULL);
    EXPECT(arena_alloc_n(&a, TEST_SMALL, 0, 0) == NULL);
    EXPECT(arena_memdup(&a, buffer, 0) == NULL);
    EXPECT(arena_memdup(&a, NULL, 0) == NULL);
    EXPECT(arena_ok(&a));
    EXPECT(arena_used(&a) == 0);
    arena_deinit(&a);
}

/* T1: argument and overflow guards. The huge sizes pass through
 * volatiles so the alloc_size attributes cannot reject them at compile
 * time; the runtime guards are the subject under test. */
static void test_arg_and_overflow_guards(void)
{
    static _Alignas(max_align_t) unsigned char buffer[256];
    volatile size_t huge_size = SIZE_MAX;
    volatile size_t over_cap = (size_t)PTRDIFF_MAX + 1;
    volatile size_t huge_count = SIZE_MAX / 2;
    volatile size_t over_count = (size_t)PTRDIFF_MAX / TEST_SMALL + 1;
    volatile size_t guard_max = (size_t)PTRDIFF_MAX;
    volatile size_t near_cap = (size_t)PTRDIFF_MAX - 1;
    volatile size_t huge_align = SIZE_MAX / 2 + 1;
    arena_t a;
    arena_t growing;

    arena_init_fixed(&a, buffer, sizeof(buffer));

    /* Non-power-of-two align records ARENA_ERR_ARG. */
    EXPECT(arena_alloc_n(&a, 1, TEST_SMALL, 3) == NULL);
    EXPECT(arena_status(&a) == ARENA_ERR_ARG);
    arena_clear_error(&a);

    /* A power of two above ARENA_MAX_ALIGN records ARENA_ERR_ARG, up
     * to the extreme SIZE_MAX / 2 + 1. */
    EXPECT(arena_alloc_n(&a, 1, TEST_SMALL, ARENA_MAX_ALIGN * 2) == NULL);
    EXPECT(arena_status(&a) == ARENA_ERR_ARG);
    arena_clear_error(&a);
    EXPECT(arena_alloc_n(&a, 1, TEST_SMALL, huge_align) == NULL);
    EXPECT(arena_status(&a) == ARENA_ERR_ARG);
    arena_clear_error(&a);

    /* Invalid align is reported even for zero-size requests. */
    EXPECT(arena_alloc_n(&a, 0, 0, 6) == NULL);
    EXPECT(arena_status(&a) == ARENA_ERR_ARG);
    arena_clear_error(&a);

    /* The PTRDIFF_MAX cap. */
    EXPECT(arena_alloc(&a, huge_size) == NULL);
    EXPECT(arena_status(&a) == ARENA_ERR_OVERFLOW);
    arena_clear_error(&a);
    EXPECT(arena_alloc(&a, over_cap) == NULL);
    EXPECT(arena_status(&a) == ARENA_ERR_OVERFLOW);
    arena_clear_error(&a);

    /* count * size products that would exceed the cap. */
    EXPECT(arena_alloc_n(&a, huge_count, TEST_SMALL, 0) == NULL);
    EXPECT(arena_status(&a) == ARENA_ERR_OVERFLOW);
    arena_clear_error(&a);
    EXPECT(arena_alloc_n(&a, over_count, TEST_SMALL, 0) == NULL);
    EXPECT(arena_status(&a) == ARENA_ERR_OVERFLOW);
    arena_clear_error(&a);

    /* Sizes that pass the product guard but dwarf the buffer fail on
     * capacity, never on overflow, right up to the cap boundary. */
    EXPECT(arena_alloc_n(&a, 1, guard_max, 0) == NULL);
    EXPECT(arena_status(&a) == ARENA_ERR_ALLOC);
    arena_clear_error(&a);
    EXPECT(arena_alloc(&a, near_cap) == NULL);
    EXPECT(arena_status(&a) == ARENA_ERR_ALLOC);
    arena_clear_error(&a);

    /* The huge-size table holds at every allocating entry point. */
    EXPECT(arena_alloc_zeroed(&a, huge_size) == NULL);
    EXPECT(arena_status(&a) == ARENA_ERR_OVERFLOW);
    arena_clear_error(&a);
    EXPECT(arena_memdup(&a, buffer, over_cap) == NULL);
    EXPECT(arena_status(&a) == ARENA_ERR_OVERFLOW);
    arena_clear_error(&a);
    EXPECT(arena_realloc(&a, NULL, 0, huge_size, 0) == NULL);
    EXPECT(arena_status(&a) == ARENA_ERR_OVERFLOW);
    arena_clear_error(&a);

    /* A near-cap size combined with the largest align overflows the
     * block-sizing computation on a growing arena. */
    EXPECT(arena_init_sized(&growing, alloc_null(), 4096, 65536) == ARENA_OK);
    EXPECT(arena_alloc_n(&growing, 1, guard_max, ARENA_MAX_ALIGN) == NULL);
    EXPECT(arena_status(&growing) == ARENA_ERR_OVERFLOW);
    arena_deinit(&growing);

    /* An ordinary product inside the guard succeeds. */
    EXPECT(arena_alloc_n(&a, 2, TEST_SMALL, 0) != NULL);

    /* NULL memdup source with nonzero size. */
    EXPECT(arena_memdup(&a, NULL, TEST_SMALL) == NULL);
    EXPECT(arena_status(&a) == ARENA_ERR_ARG);
    arena_clear_error(&a);

    /* Failure atomicity: the guards above changed nothing but status. */
    EXPECT(arena_committed(&a) == sizeof(buffer));
    arena_deinit(&a);
}

/* T3: memdup copies content. */
static void test_memdup_content(void)
{
    static _Alignas(max_align_t) unsigned char buffer[256];
    arena_t a;
    unsigned char src[TEST_SMALL];
    unsigned char *copy;
    size_t idx;

    for (idx = 0; idx < TEST_SMALL; idx++) {
        src[idx] = (unsigned char)(idx * 3);
    }
    arena_init_fixed(&a, buffer, sizeof(buffer));
    copy = arena_memdup(&a, src, sizeof(src));
    EXPECT(copy != NULL);
    EXPECT(copy != src);
    for (idx = 0; idx < TEST_SMALL; idx++) {
        EXPECT(copy[idx] == src[idx]);
    }
    arena_deinit(&a);
}

/* T6/T7: NULL-arena queries and status names. */
static void test_queries_and_names(void)
{
    EXPECT(arena_status(NULL) == ARENA_ERR_ARG);
    EXPECT(!arena_ok(NULL));
    EXPECT(arena_failed(NULL));
    EXPECT(arena_used(NULL) == 0);
    EXPECT(arena_committed(NULL) == 0);
    EXPECT(arena_high_water(NULL) == 0);
    EXPECT(arena_alloc(NULL, TEST_SMALL) == NULL);
    EXPECT(arena_memdup(NULL, "x", 1) == NULL);
    arena_deinit(NULL);
    arena_reset(NULL);
    arena_trim(NULL);
    arena_clear_error(NULL);
    arena_temp_end(arena_temp_begin(NULL)); /* inert scope */
    EXPECT(arena_realloc(NULL, NULL, 0, TEST_SMALL, 0) == NULL);

    EXPECT(strcmp(arena_status_name(ARENA_OK), "ARENA_OK") == 0);
    EXPECT(strcmp(arena_status_name(ARENA_ERR_ARG), "ARENA_ERR_ARG") == 0);
    EXPECT(strcmp(arena_status_name(ARENA_ERR_ALLOC), "ARENA_ERR_ALLOC") == 0);
    EXPECT(strcmp(arena_status_name(ARENA_ERR_OVERFLOW), "ARENA_ERR_OVERFLOW") == 0);
    EXPECT(strcmp(arena_status_name((arena_status_t)99), "ARENA_ERR_UNKNOWN") == 0);
}

/* T1: init_sized validation. Uses the null parent, so it is heap-free. */
static void test_init_sized_validation(void)
{
    arena_t a;

    EXPECT(arena_init_sized(NULL, alloc_null(), 4096, 65536) == ARENA_ERR_ARG);
    EXPECT(arena_init_sized(&a, alloc_null(), 0, 65536) == ARENA_ERR_ARG);
    EXPECT(arena_status(&a) == ARENA_ERR_ARG);
    EXPECT(arena_init_sized(&a, alloc_null(), 65536, 4096) == ARENA_ERR_ARG);
    EXPECT(arena_init_sized(&a, alloc_null(), 4096, SIZE_MAX) == ARENA_ERR_ARG);
    EXPECT(arena_init_sized(&a, alloc_null(), ARENA_FIXED_OVERHEAD, ARENA_FIXED_OVERHEAD) ==
           ARENA_OK);
    EXPECT(arena_ok(&a));
    arena_deinit(&a);
}

/* T2: deinit and reinit cycles on fixed arenas. */
static void test_deinit_cycles(void)
{
    static _Alignas(max_align_t) unsigned char buffer[256];
    arena_t a;
    unsigned generation;

    arena_init_fixed(&a, buffer, sizeof(buffer));
    EXPECT(arena_alloc(&a, TEST_SMALL) != NULL);
    generation = a.generation;
    arena_deinit(&a);
    EXPECT(a.generation == generation + 1);
    EXPECT(arena_used(&a) == 0);
    EXPECT(arena_committed(&a) == 0);
    arena_deinit(&a); /* double deinit is accepted */

    /* The storage is reusable for a fresh arena. */
    arena_init_fixed(&a, buffer, sizeof(buffer));
    EXPECT(arena_ok(&a));
    EXPECT(arena_alloc(&a, TEST_SMALL) != NULL);
    arena_deinit(&a);
}

/* T2: reset on a fixed arena rewinds deterministically. Heap-free. */
static void test_fixed_reset(void)
{
    static _Alignas(max_align_t) unsigned char buffer[512];
    arena_t a;
    void *first;
    void *again;
    size_t high;
    unsigned generation;

    arena_init_fixed(&a, buffer, sizeof(buffer));
    first = arena_alloc(&a, TEST_MEDIUM);
    EXPECT(first != NULL);
    EXPECT(arena_used(&a) > 0);
    high = arena_high_water(&a);
    generation = a.generation;

    arena_reset(&a);
    EXPECT(arena_ok(&a));
    EXPECT(arena_used(&a) == 0);
    EXPECT(arena_committed(&a) == sizeof(buffer));
    EXPECT(arena_high_water(&a) == high);
    EXPECT(a.generation == generation + 1);

    /* The cursor really rewound: the same request lands at the same
     * address. */
    again = arena_alloc(&a, TEST_MEDIUM);
    EXPECT(again == first);

    /* Reset clears a sticky error too. */
    EXPECT(arena_alloc(&a, sizeof(buffer)) == NULL);
    EXPECT(arena_failed(&a));
    arena_reset(&a);
    EXPECT(arena_ok(&a));

    arena_trim(&a); /* no-op on fixed arenas */
    EXPECT(arena_committed(&a) == sizeof(buffer));
    arena_deinit(&a);
}

/* T2: temp scopes on a fixed arena: exact rewind, nesting, sticky
 * status surviving the scope. Heap-free. */
static void test_temp_scopes(void)
{
    static _Alignas(max_align_t) unsigned char buffer[1024];
    arena_t a;
    arena_temp_t outer;
    arena_temp_t inner;
    void *base;
    void *first_inner;
    void *second_inner;
    void *after;
    size_t used_base;

    arena_init_fixed(&a, buffer, sizeof(buffer));
    base = arena_alloc(&a, TEST_MEDIUM);
    EXPECT(base != NULL);
    used_base = arena_used(&a);

    outer = arena_temp_begin(&a);
    first_inner = arena_alloc(&a, TEST_MEDIUM);
    EXPECT(first_inner != NULL);
    inner = arena_temp_begin(&a);
    second_inner = arena_alloc(&a, TEST_MEDIUM);
    EXPECT(second_inner != NULL);
    EXPECT(a.temp_depth == 2);

    arena_temp_end(inner);
    EXPECT(a.temp_depth == 1);
    /* Exact rewind: the next allocation reuses the rewound spot. */
    after = arena_alloc(&a, TEST_MEDIUM);
    EXPECT(after == second_inner);

    arena_temp_end(outer);
    EXPECT(a.temp_depth == 0);
    EXPECT(arena_used(&a) == used_base);
    after = arena_alloc(&a, TEST_MEDIUM);
    EXPECT(after == first_inner);

    /* A failure inside a temp scope stays observable after it ends. */
    outer = arena_temp_begin(&a);
    EXPECT(arena_alloc(&a, sizeof(buffer)) == NULL);
    EXPECT(arena_failed(&a));
    arena_temp_end(outer);
    EXPECT(arena_failed(&a));
    EXPECT(arena_status(&a) == ARENA_ERR_ALLOC);
    arena_clear_error(&a);
    EXPECT(arena_ok(&a));

    arena_deinit(&a);
}

/* T3: realloc on a fixed arena: last-allocation in-place paths.
 * Heap-free. */
static void test_realloc_fixed(void)
{
    static _Alignas(max_align_t) unsigned char buffer[512];
    arena_t a;
    unsigned char *ptr;
    unsigned char *grown;
    size_t used_before;

    arena_init_fixed(&a, buffer, sizeof(buffer));

    /* NULL with old_size 0 behaves as alloc. */
    ptr = arena_realloc(&a, NULL, 0, TEST_MEDIUM, 0);
    EXPECT(ptr != NULL);
    memset(ptr, TEST_FILL_BYTE, TEST_MEDIUM);

    /* Grow in place: same pointer, contents kept. */
    grown = arena_realloc(&a, ptr, TEST_MEDIUM, TEST_LARGE, 0);
    EXPECT(grown == ptr);
    EXPECT(grown[TEST_MEDIUM - 1] == TEST_FILL_BYTE);

    /* Shrink in place returns the tail. */
    used_before = arena_used(&a);
    EXPECT(arena_realloc(&a, grown, TEST_LARGE, TEST_SMALL, 0) == grown);
    EXPECT(arena_used(&a) == used_before - (TEST_LARGE - TEST_SMALL));

    /* new_size 0 is refused with a recorded status. */
    EXPECT(arena_realloc(&a, grown, TEST_SMALL, 0, 0) == NULL);
    EXPECT(arena_status(&a) == ARENA_ERR_ARG);
    arena_clear_error(&a);

    arena_deinit(&a);
}

/*
 * T3: seeded randomized op sequences. Every live allocation carries a
 * fill pattern; after every op all live allocations verify their
 * pattern, their alignment, and pairwise disjointness. Failures print
 * the seed and the op index. Runs on a fixed arena in every build and
 * on a tracked growing arena where the heap is allowed.
 */

enum { RAND_LIVE_MAX = 32, RAND_TEMP_MAX = 4, RAND_OPS = 2000, RAND_SIZE_MAX = 600 };

struct rand_entry {
    unsigned char *ptr;
    size_t size;
    size_t align;
    unsigned char fill;
    unsigned level;
};

static uint32_t test_rng_next(uint32_t *state)
{
    uint32_t value = *state;

    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

static void rand_fail(uint32_t seed, size_t op, const char *what)
{
    fprintf(stderr, "FAIL random ops: seed=0x%08x op=%zu: %s\n", (unsigned)seed, op, what);
    abort();
}

static void rand_fill_entry(struct rand_entry *entry)
{
    size_t idx;

    for (idx = 0; idx < entry->size; idx++) {
        entry->ptr[idx] = (unsigned char)(entry->fill + (unsigned char)idx);
    }
}

static void rand_verify_all(const struct rand_entry *live, size_t live_count, uint32_t seed,
                            size_t op)
{
    size_t i;
    size_t j;
    size_t b;

    for (i = 0; i < live_count; i++) {
        size_t effective = live[i].align == 0 ? ARENA_DEFAULT_ALIGN : live[i].align;

        if (!test_is_aligned(live[i].ptr, effective)) {
            rand_fail(seed, op, "misaligned allocation");
        }
        for (b = 0; b < live[i].size; b++) {
            if (live[i].ptr[b] != (unsigned char)(live[i].fill + (unsigned char)b)) {
                rand_fail(seed, op, "fill pattern clobbered");
            }
        }
    }
    for (i = 0; i < live_count; i++) {
        uintptr_t a_lo = (uintptr_t)live[i].ptr;
        uintptr_t a_hi = a_lo + live[i].size;

        for (j = i + 1; j < live_count; j++) {
            uintptr_t b_lo = (uintptr_t)live[j].ptr;
            uintptr_t b_hi = b_lo + live[j].size;

            if (a_lo < b_hi && b_lo < a_hi) {
                rand_fail(seed, op, "live allocations overlap");
            }
        }
    }
}

static void rand_drop_level(struct rand_entry *live, size_t *live_count, unsigned depth)
{
    size_t idx = 0;

    while (idx < *live_count) {
        if (live[idx].level > depth) {
            live[idx] = live[*live_count - 1];
            *live_count -= 1;
        } else {
            idx++;
        }
    }
}

static void run_random_ops(arena_t *a, uint32_t seed)
{
    struct rand_entry live[RAND_LIVE_MAX];
    size_t live_count = 0;
    arena_temp_t temps[RAND_TEMP_MAX];
    unsigned depth = 0;
    unsigned char next_fill = 1;
    alloc_t adapter = arena_allocator(a);
    uint32_t rng = seed;
    size_t op;

    for (op = 0; op < RAND_OPS; op++) {
        uint32_t roll = test_rng_next(&rng);
        uint32_t detail = test_rng_next(&rng);
        size_t size = 1 + (size_t)(detail % RAND_SIZE_MAX);
        size_t align = (detail & 7u) == 0 ? 0 : (size_t)1 << ((detail >> 3) % 7u);

        switch (roll % 8u) {
        case 0:
        case 1: { /* alloc, uninitialized */
            unsigned char *ptr = arena_alloc_n(a, 1, size, align);

            if (ptr == NULL) {
                if (!arena_failed(a)) {
                    rand_fail(seed, op, "NULL without a recorded status");
                }
                arena_clear_error(a);
                break;
            }
            if (live_count < RAND_LIVE_MAX) {
                live[live_count].ptr = ptr;
                live[live_count].size = size;
                live[live_count].align = align;
                live[live_count].fill = next_fill++;
                live[live_count].level = depth;
                rand_fill_entry(&live[live_count]);
                live_count++;
            }
            break;
        }
        case 2: { /* zeroed alloc, verified zero before filling */
            unsigned char *ptr = arena_alloc_zeroed(a, size);
            size_t b;

            if (ptr == NULL) {
                if (!arena_failed(a)) {
                    rand_fail(seed, op, "NULL without a recorded status");
                }
                arena_clear_error(a);
                break;
            }
            for (b = 0; b < size; b++) {
                if (ptr[b] != 0) {
                    rand_fail(seed, op, "zeroed allocation not zero");
                }
            }
            if (live_count < RAND_LIVE_MAX) {
                live[live_count].ptr = ptr;
                live[live_count].size = size;
                live[live_count].align = 0;
                live[live_count].fill = next_fill++;
                live[live_count].level = depth;
                rand_fill_entry(&live[live_count]);
                live_count++;
            }
            break;
        }
        case 3:
        case 4: { /* realloc the newest in-scope entry */
            size_t pick = live_count;
            unsigned char *moved;
            size_t verify;
            size_t b;

            while (pick > 0 && live[pick - 1].level != depth) {
                pick--;
            }
            if (pick == 0) {
                break;
            }
            pick--;
            moved = arena_realloc(a, live[pick].ptr, live[pick].size, size, live[pick].align);
            if (moved == NULL) {
                if (!arena_failed(a)) {
                    rand_fail(seed, op, "NULL without a recorded status");
                }
                arena_clear_error(a);
                break; /* the old entry stays valid and verified */
            }
            verify = live[pick].size < size ? live[pick].size : size;
            for (b = 0; b < verify; b++) {
                if (moved[b] != (unsigned char)(live[pick].fill + (unsigned char)b)) {
                    rand_fail(seed, op, "realloc lost contents");
                }
            }
            live[pick].ptr = moved;
            live[pick].size = size;
            live[pick].fill = next_fill++;
            rand_fill_entry(&live[pick]);
            break;
        }
        case 5: /* temp begin */
            if (depth < RAND_TEMP_MAX) {
                temps[depth] = arena_temp_begin(a);
                depth++;
            }
            break;
        case 6: /* temp end */
            if (depth > 0) {
                depth--;
                rand_drop_level(live, &live_count, depth);
                arena_temp_end(temps[depth]);
            }
            break;
        default: /* adapter free of the newest entry when it is in scope */
            if (live_count > 0 && live[live_count - 1].level == depth) {
                struct rand_entry *entry = &live[live_count - 1];

                alloc_free(&adapter, entry->ptr, entry->size, entry->align);
                live_count--;
            }
            break;
        }
        rand_verify_all(live, live_count, seed, op);
        if (arena_committed(a) != 0 && arena_used(a) > arena_committed(a)) {
            rand_fail(seed, op, "used exceeds committed");
        }
        test_ctx.run++;
    }
    while (depth > 0) {
        depth--;
        rand_drop_level(live, &live_count, depth);
        arena_temp_end(temps[depth]);
        rand_verify_all(live, live_count, seed, op);
    }
}

/* T3: the seeded property loop on a fixed arena. Heap-free. */
static void test_random_ops_fixed(void)
{
    static _Alignas(max_align_t) unsigned char buffer[1 << 16];
    arena_t a;

    arena_init_fixed(&a, buffer, sizeof(buffer));
    run_random_ops(&a, 0xC0FFEE01u);
    arena_deinit(&a);
}

#if !defined(NDEBUG) && defined(__unix__)

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

/* T2: contract-violation death tests: the sequence must abort through
 * the debug detection promised by the header. */
static void expect_abort(void (*violation)(void))
{
    pid_t pid;
    int status;

    pid = fork();
    EXPECT(pid >= 0);
    if (pid == 0) {
        FILE *sink = freopen("/dev/null", "w", stderr);

        (void)sink;
        violation();
        _exit(0); /* no abort happened: reported as the wrong signal */
    }
    status = 0;
    EXPECT(waitpid(pid, &status, 0) == pid);
    EXPECT(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}

static void violate_temp_order(void)
{
    static _Alignas(max_align_t) unsigned char buffer[256];
    arena_t a;
    arena_temp_t outer;
    arena_temp_t inner;

    arena_init_fixed(&a, buffer, sizeof(buffer));
    outer = arena_temp_begin(&a);
    inner = arena_temp_begin(&a);
    (void)inner;
    arena_temp_end(outer); /* LIFO violated: inner is still open */
}

static void violate_temp_after_reset(void)
{
    static _Alignas(max_align_t) unsigned char buffer[256];
    arena_t a;
    arena_temp_t temp;

    arena_init_fixed(&a, buffer, sizeof(buffer));
    temp = arena_temp_begin(&a);
    arena_reset(&a);
    arena_temp_end(temp); /* stale: the arena was reset since begin */
}

static void violate_temp_rollback(void)
{
    static _Alignas(max_align_t) unsigned char buffer[512];
    arena_t a;
    alloc_t ad;
    arena_temp_t temp;
    void *before_scope;

    arena_init_fixed(&a, buffer, sizeof(buffer));
    ad = arena_allocator(&a);
    before_scope = alloc_alloc(&ad, TEST_MEDIUM, 0);
    temp = arena_temp_begin(&a);
    /* Releasing a pre-scope allocation inside the scope moves the
     * cursor below the snapshot: a contract violation the end-of-scope
     * checks must catch. */
    alloc_free(&ad, before_scope, TEST_MEDIUM, 0);
    arena_temp_end(temp);
}

static void test_temp_violation_death(void)
{
    expect_abort(violate_temp_order);
    expect_abort(violate_temp_after_reset);
    expect_abort(violate_temp_rollback);
}

#if defined(TEST_HAS_ASAN) && !defined(ARENA_TEST_FIXED_ONLY)

/* T2: an actual dereference of rewound memory dies under ASan. The
 * child exits nonzero (ASan's default) or dies by signal; either counts
 * as death. */
static void expect_nonzero_exit(void (*violation)(void))
{
    pid_t pid;
    int status;

    pid = fork();
    EXPECT(pid >= 0);
    if (pid == 0) {
        FILE *sink = freopen("/dev/null", "w", stderr);

        (void)sink;
        violation();
        _exit(0); /* surviving the violation is the failure */
    }
    status = 0;
    EXPECT(waitpid(pid, &status, 0) == pid);
    EXPECT(WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) != 0));
}

static void violate_use_after_rewind(void)
{
    arena_t a;
    arena_temp_t temp;
    volatile unsigned char *stale;

    arena_init(&a, alloc_libc());
    temp = arena_temp_begin(&a);
    stale = arena_alloc(&a, TEST_MEDIUM);
    if (stale == NULL) {
        return; /* no allocation, no violation: the parent sees exit 0 */
    }
    arena_temp_end(temp);
    stale[0] = 1; /* use after rewind: ASan must catch this */
}

static void violate_use_after_reset(void)
{
    arena_t a;
    volatile unsigned char *stale;

    arena_init(&a, alloc_libc());
    stale = arena_alloc(&a, TEST_MEDIUM);
    if (stale == NULL) {
        return; /* no allocation, no violation: the parent sees exit 0 */
    }
    arena_reset(&a);
    stale[0] = 1; /* use after reset: ASan must catch this */
}

static void test_use_after_rewind_death(void)
{
    expect_nonzero_exit(violate_use_after_rewind);
    expect_nonzero_exit(violate_use_after_reset);
}

#endif /* TEST_HAS_ASAN && !ARENA_TEST_FIXED_ONLY */

#endif /* !NDEBUG && __unix__ */

#ifndef ARENA_TEST_FIXED_ONLY

/*
 * The tracking allocator: counts allocating calls, verifies that xfree
 * and xrealloc receive the exact size and align of the allocating call,
 * detects leaks, and injects fail-Nth OOM (transient: exactly call N;
 * persistent: call N and all later ones).
 */

enum { TRACK_MAX = 512 };

struct track_entry {
    void *ptr;
    size_t size;
    size_t align;
};

struct track_ctx {
    struct track_entry live[TRACK_MAX];
    size_t live_count;
    unsigned long alloc_calls;
    unsigned long fail_at; /* 1-based; 0 = never fail */
    bool persistent;
    unsigned long injected;
};

static void *track_xalloc(void *ctx, size_t size, size_t align)
{
    struct track_ctx *t = ctx;
    void *ptr;

    if (size == 0) {
        return NULL;
    }
    /* The arena must honor the interface contract as a caller. */
    EXPECT(size <= (size_t)PTRDIFF_MAX);
    EXPECT(align == 0 || ((align & (align - 1)) == 0 && align <= _Alignof(max_align_t)));
    t->alloc_calls++;
    if (t->fail_at != 0 &&
        (t->persistent ? t->alloc_calls >= t->fail_at : t->alloc_calls == t->fail_at)) {
        t->injected++;
        return NULL;
    }
    ptr = malloc(size);
    EXPECT(ptr != NULL);
    EXPECT(t->live_count < TRACK_MAX);
    t->live[t->live_count].ptr = ptr;
    t->live[t->live_count].size = size;
    t->live[t->live_count].align = align;
    t->live_count++;
    return ptr;
}

static void *track_xrealloc(void *ctx, void *ptr, size_t old_size, size_t new_size, size_t align)
{
    (void)ctx;
    (void)ptr;
    (void)old_size;
    (void)new_size;
    (void)align;
    /* The arena never reallocs parent blocks. */
    EXPECT(0);
    return NULL;
}

static void track_xfree(void *ctx, void *ptr, size_t size, size_t align)
{
    struct track_ctx *t = ctx;
    size_t idx;

    if (ptr == NULL) {
        return;
    }
    for (idx = 0; idx < t->live_count; idx++) {
        if (t->live[idx].ptr == ptr) {
            break;
        }
    }
    EXPECT(idx < t->live_count); /* no double free, no foreign free */
    EXPECT(t->live[idx].size == size);
    EXPECT(t->live[idx].align == align);
    t->live[idx] = t->live[t->live_count - 1];
    t->live_count--;
    free(ptr);
}

static alloc_t track_allocator(struct track_ctx *t)
{
    alloc_t backend = {t, track_xalloc, track_xrealloc, track_xfree};

    return backend;
}

/* T1: growth policy: doubling to the cap, oversize dedicated blocks,
 * over-aligned requests, exact release on deinit. */
static void test_growth_and_blocks(void)
{
    static struct track_ctx t;
    arena_t a;
    void *ptr;
    size_t idx;
    bool saw_cap_block = false;
    bool saw_oversize = false;

    memset(&t, 0, sizeof(t));
    EXPECT(arena_init_sized(&a, track_allocator(&t), 128, 512) == ARENA_OK);
    EXPECT(arena_committed(&a) == 0); /* the first block is lazy */

    ptr = arena_alloc(&a, TEST_MEDIUM);
    EXPECT(ptr != NULL);
    EXPECT(arena_committed(&a) == 128);

    for (idx = 0; idx < 40; idx++) {
        ptr = arena_alloc(&a, TEST_MEDIUM);
        EXPECT(ptr != NULL);
        EXPECT(test_is_aligned(ptr, ARENA_DEFAULT_ALIGN));
        memset(ptr, TEST_FILL_BYTE, TEST_MEDIUM);
    }
    /* Every normal block the parent saw is one of the policy sizes. */
    for (idx = 0; idx < t.live_count; idx++) {
        EXPECT(t.live[idx].size == 128 || t.live[idx].size == 256 || t.live[idx].size == 512);
        if (t.live[idx].size == 512) {
            saw_cap_block = true;
        }
    }
    EXPECT(saw_cap_block); /* doubling reached and held the cap */

    /* A request beyond the cap gets a dedicated block and continues. */
    ptr = arena_alloc(&a, 2048);
    EXPECT(ptr != NULL);
    memset(ptr, TEST_FILL_BYTE, 2048);
    for (idx = 0; idx < t.live_count; idx++) {
        if (t.live[idx].size >= 2048) {
            saw_oversize = true;
        }
    }
    EXPECT(saw_oversize);
    EXPECT(arena_alloc(&a, TEST_MEDIUM) != NULL);

    /* Over-aligned requests work on growing arenas. */
    ptr = arena_alloc_n(&a, 1, 8, ARENA_MAX_ALIGN);
    EXPECT(ptr != NULL);
    EXPECT(test_is_aligned(ptr, ARENA_MAX_ALIGN));

    EXPECT(arena_used(&a) <= arena_committed(&a));
    arena_deinit(&a);
    EXPECT(t.live_count == 0); /* every block returned, exact args */
    EXPECT(arena_committed(&a) == 0);
}

/* T2: reset retains normal blocks, releases oversize blocks, and reuse
 * happens without parent traffic; trim releases the free list. */
static void test_reset_and_trim(void)
{
    static struct track_ctx t;
    arena_t a;
    void *ptr;
    size_t idx;
    size_t committed_full;
    size_t committed_reset;
    size_t high;
    size_t live_before;
    unsigned long calls_before;

    memset(&t, 0, sizeof(t));
    EXPECT(arena_init_sized(&a, track_allocator(&t), 256, 512) == ARENA_OK);
    for (idx = 0; idx < 12; idx++) {
        EXPECT(arena_alloc(&a, 100) != NULL);
    }
    ptr = arena_alloc(&a, 1000); /* oversize for max_block 512 */
    EXPECT(ptr != NULL);

    committed_full = arena_committed(&a);
    high = arena_high_water(&a);
    live_before = t.live_count;

    arena_reset(&a);
    EXPECT(arena_ok(&a));
    EXPECT(arena_used(&a) == 0);
    EXPECT(arena_high_water(&a) == high);
    EXPECT(arena_committed(&a) < committed_full);
    EXPECT(t.live_count == live_before - 1); /* exactly the oversize left */

    /* The same workload replays from retained blocks: zero parent
     * calls, committed flat. */
    committed_reset = arena_committed(&a);
    calls_before = t.alloc_calls;
    for (idx = 0; idx < 12; idx++) {
        EXPECT(arena_alloc(&a, 100) != NULL);
    }
    EXPECT(t.alloc_calls == calls_before);
    EXPECT(arena_committed(&a) == committed_reset);

    /* Trim returns the free list, keeping only the current block, which
     * is the oldest normal block: the min_block-sized first one. */
    arena_reset(&a);
    arena_trim(&a);
    EXPECT(t.live_count == 1);
    EXPECT(arena_committed(&a) == 256);

    arena_deinit(&a);
    EXPECT(t.live_count == 0);
    EXPECT(arena_committed(&a) == 0);
}

/* T4: after a recorded failure the sticky gate answers without parent
 * traffic; clear_error resumes. */
static void test_sticky_gating_parent_calls(void)
{
    static struct track_ctx t;
    arena_t a;
    unsigned long calls;

    memset(&t, 0, sizeof(t));
    t.fail_at = 1; /* transient: only the first parent call fails */
    EXPECT(arena_init_sized(&a, track_allocator(&t), 256, 512) == ARENA_OK);
    EXPECT(arena_alloc(&a, 100) == NULL);
    EXPECT(arena_status(&a) == ARENA_ERR_ALLOC);
    EXPECT(t.injected == 1);

    calls = t.alloc_calls;
    EXPECT(arena_alloc(&a, 8) == NULL);
    EXPECT(arena_alloc_zeroed(&a, 8) == NULL);
    EXPECT(arena_memdup(&a, &calls, sizeof(calls)) == NULL);
    EXPECT(t.alloc_calls == calls); /* gate short-circuits before the parent */

    arena_clear_error(&a);
    EXPECT(arena_alloc(&a, 8) != NULL);
    arena_deinit(&a);
    EXPECT(t.live_count == 0);
}

/* T4: SQLite-style OOM loops over a scenario table, transient and
 * persistent, with leak and consistency checks after every injection. */

typedef void (*oom_scenario_fn)(arena_t *a);

static void oom_scenario_small(arena_t *a)
{
    size_t idx;

    for (idx = 0; idx < 10; idx++) {
        (void)arena_alloc(a, 100);
    }
}

static void oom_scenario_growth(arena_t *a)
{
    size_t idx;

    for (idx = 0; idx < 30; idx++) {
        (void)arena_alloc(a, 200);
    }
}

static void oom_scenario_oversize(arena_t *a)
{
    (void)arena_alloc(a, 100);
    (void)arena_alloc(a, 5000);
    (void)arena_alloc(a, 100);
}

static void oom_scenario_mixed(arena_t *a)
{
    static const unsigned char blob[TEST_LARGE] = {1, 2, 3};

    (void)arena_alloc_zeroed(a, 300);
    (void)arena_memdup(a, blob, sizeof(blob));
    (void)arena_alloc_n(a, 5, TEST_MEDIUM, 32);
    (void)ARENA_NEW_N(a, long, 40);
}

static void oom_scenario_temp_cycles(arena_t *a)
{
    size_t cycle;

    for (cycle = 0; cycle < 4; cycle++) {
        arena_temp_t temp = arena_temp_begin(a);

        (void)arena_alloc(a, 400);
        (void)arena_alloc(a, 2000);
        arena_temp_end(temp);
    }
    (void)arena_alloc(a, TEST_MEDIUM);
}

static void oom_scenario_realloc_chain(arena_t *a)
{
    void *ptr = arena_alloc(a, TEST_MEDIUM);
    size_t sizes[] = {200, 900, 300, 4000};
    size_t step;
    size_t held = TEST_MEDIUM;

    for (step = 0; step < sizeof(sizes) / sizeof(sizes[0]); step++) {
        void *grown;

        if (ptr == NULL) {
            return;
        }
        grown = arena_realloc(a, ptr, held, sizes[step], 0);
        if (grown == NULL) {
            return; /* the old block stays valid; deinit reclaims it */
        }
        ptr = grown;
        held = sizes[step];
    }
}

static void oom_scenario_adapter(arena_t *a)
{
    alloc_t ad = arena_allocator(a);
    void *first = alloc_alloc(&ad, 200, 0);
    void *second = alloc_alloc(&ad, 100, 32);

    if (first != NULL) {
        first = alloc_realloc(&ad, first, 200, 800, 0);
    }
    if (second != NULL) {
        alloc_free(&ad, second, 100, 32);
    }
    (void)alloc_alloc(&ad, 5000, 0);
    if (first != NULL) {
        alloc_free(&ad, first, 800, 0);
    }
}

static void test_oom_loops(void)
{
    static const oom_scenario_fn scenarios[] = {
        oom_scenario_small,  oom_scenario_growth,      oom_scenario_oversize,
        oom_scenario_mixed,  oom_scenario_temp_cycles, oom_scenario_realloc_chain,
        oom_scenario_adapter};
    static struct track_ctx t;
    arena_t a;
    size_t s;
    int persistent;
    unsigned long n;
    bool completed;

    for (s = 0; s < sizeof(scenarios) / sizeof(scenarios[0]); s++) {
        for (persistent = 0; persistent < 2; persistent++) {
            completed = false;
            for (n = 1; n < 1000 && !completed; n++) {
                memset(&t, 0, sizeof(t));
                t.fail_at = n;
                t.persistent = persistent != 0;
                EXPECT(arena_init_sized(&a, track_allocator(&t), 256, 1024) == ARENA_OK);
                scenarios[s](&a);
                completed = t.injected == 0;
                if (!completed) {
                    EXPECT(arena_status(&a) == ARENA_ERR_ALLOC);
                    if (persistent == 0) {
                        arena_clear_error(&a);
                        EXPECT(arena_alloc(&a, 8) != NULL);
                    }
                }
                arena_deinit(&a);
                EXPECT(t.live_count == 0);
                EXPECT(arena_committed(&a) == 0);
            }
            EXPECT(completed);
        }
    }
}

/* T2: 1000 temp cycles run from retained blocks: committed and parent
 * traffic stay flat after warmup. */
static void test_temp_block_recycling(void)
{
    static struct track_ctx t;
    arena_t a;
    arena_temp_t temp;
    size_t cycle;
    size_t idx;
    size_t committed_warm = 0;
    unsigned long calls_warm = 0;

    memset(&t, 0, sizeof(t));
    EXPECT(arena_init_sized(&a, track_allocator(&t), 256, 1024) == ARENA_OK);
    for (cycle = 0; cycle < 1000; cycle++) {
        temp = arena_temp_begin(&a);
        for (idx = 0; idx < 20; idx++) {
            EXPECT(arena_alloc(&a, 150) != NULL);
        }
        arena_temp_end(temp);
        EXPECT(arena_used(&a) == 0);
        if (cycle == 0) {
            committed_warm = arena_committed(&a);
            calls_warm = t.alloc_calls;
        } else {
            EXPECT(arena_committed(&a) == committed_warm);
            EXPECT(t.alloc_calls == calls_warm);
        }
    }
    arena_deinit(&a);
    EXPECT(t.live_count == 0);
}

/* T2: oversize blocks created inside a temp scope return to the parent
 * at temp end instead of lingering on the free list. */
static void test_temp_oversize_release(void)
{
    static struct track_ctx t;
    arena_t a;
    arena_temp_t temp;
    size_t live_before;
    size_t committed_before;

    memset(&t, 0, sizeof(t));
    EXPECT(arena_init_sized(&a, track_allocator(&t), 256, 512) == ARENA_OK);
    EXPECT(arena_alloc(&a, 100) != NULL);
    live_before = t.live_count;
    committed_before = arena_committed(&a);

    temp = arena_temp_begin(&a);
    EXPECT(arena_alloc(&a, 5000) != NULL); /* oversize */
    EXPECT(t.live_count == live_before + 1);
    arena_temp_end(temp);
    EXPECT(t.live_count == live_before);
    EXPECT(arena_committed(&a) == committed_before);

    arena_deinit(&a);
    EXPECT(t.live_count == 0);
}

/* T3: the realloc matrix on a growing arena. */
static void test_realloc_matrix(void)
{
    static struct track_ctx t;
    arena_t a;
    unsigned char *ptr;
    unsigned char *other;
    unsigned char *moved;
    size_t committed_before;
    size_t used_before;
    size_t idx;

    memset(&t, 0, sizeof(t));
    EXPECT(arena_init_sized(&a, track_allocator(&t), 1024, 4096) == ARENA_OK);

    /* Grow in place: last allocation, capacity available. */
    ptr = arena_alloc(&a, 100);
    EXPECT(ptr != NULL);
    memset(ptr, TEST_FILL_BYTE, 100);
    committed_before = arena_committed(&a);
    moved = arena_realloc(&a, ptr, 100, 200, 0);
    EXPECT(moved == ptr);
    EXPECT(arena_committed(&a) == committed_before); /* no new block */
    for (idx = 0; idx < 100; idx++) {
        EXPECT(moved[idx] == TEST_FILL_BYTE);
    }

    /* Not the last allocation: the move path copies. */
    other = arena_alloc(&a, TEST_SMALL);
    EXPECT(other != NULL);
    moved = arena_realloc(&a, ptr, 200, 400, 0);
    EXPECT(moved != NULL);
    EXPECT(moved != ptr);
    for (idx = 0; idx < 100; idx++) {
        EXPECT(moved[idx] == TEST_FILL_BYTE);
    }

    /* Growth beyond the block moves and preserves contents. */
    memset(moved, 0x2A, 400);
    ptr = arena_realloc(&a, moved, 400, 3000, 0);
    EXPECT(ptr != NULL);
    EXPECT(ptr != moved);
    for (idx = 0; idx < 400; idx++) {
        EXPECT(ptr[idx] == 0x2A);
    }

    /* Shrink of the last allocation returns the tail. */
    used_before = arena_used(&a);
    EXPECT(arena_realloc(&a, ptr, 3000, 1000, 0) == ptr);
    EXPECT(arena_used(&a) == used_before - 2000);

    /* Parent refusal on the move path: NULL, status recorded, the old
     * region intact. */
    memset(ptr, 0x77, 1000);
    (void)arena_alloc(&a, TEST_SMALL); /* make ptr not last */
    t.fail_at = t.alloc_calls + 1;
    t.persistent = true;
    moved = arena_realloc(&a, ptr, 1000, 100000, 0);
    EXPECT(moved == NULL);
    EXPECT(arena_status(&a) == ARENA_ERR_ALLOC);
    for (idx = 0; idx < 1000; idx++) {
        EXPECT(ptr[idx] == 0x77);
    }
    t.fail_at = 0;
    t.persistent = false;
    arena_clear_error(&a);

    /* Shrink of a non-last allocation takes the move path and keeps
     * the surviving prefix. */
    moved = arena_realloc(&a, ptr, 1000, 100, 0);
    EXPECT(moved != NULL);
    EXPECT(moved != ptr);
    for (idx = 0; idx < 100; idx++) {
        EXPECT(moved[idx] == 0x77);
    }

    arena_deinit(&a);
    EXPECT(t.live_count == 0);
}

/* T6: the adapter presents the arena as a conforming alloc_t; LIFO
 * alloc/free pairs cost zero net memory. */
static void test_adapter_contract(void)
{
    static struct track_ctx t;
    volatile size_t huge = SIZE_MAX - 8;
    arena_t a;
    alloc_t ad;
    void *first;
    void *second;
    unsigned char *bytes;
    size_t used_before;
    size_t committed_before;
    size_t idx;

    memset(&t, 0, sizeof(t));
    EXPECT(arena_init_sized(&a, track_allocator(&t), 1024, 4096) == ARENA_OK);
    ad = arena_allocator(&a);

    /* Contract table. */
    EXPECT(alloc_alloc(&ad, 0, 0) == NULL);
    first = alloc_alloc(&ad, TEST_MEDIUM, 0);
    EXPECT(first != NULL);
    EXPECT(test_is_aligned(first, ARENA_DEFAULT_ALIGN));
    alloc_free(&ad, NULL, 0, 0);

    /* Richer than libc: over-aligned requests succeed. */
    second = alloc_alloc(&ad, TEST_SMALL, 4096);
    EXPECT(second != NULL);
    EXPECT(test_is_aligned(second, 4096));

    /* LIFO rollback: freeing the newest allocation returns its bytes. */
    used_before = arena_used(&a);
    first = alloc_alloc(&ad, TEST_LARGE, 0);
    EXPECT(first != NULL);
    alloc_free(&ad, first, TEST_LARGE, 0);
    EXPECT(arena_used(&a) == used_before);

    /* A long LIFO pair sequence is flat in both used and committed. */
    committed_before = arena_committed(&a);
    for (idx = 0; idx < 100; idx++) {
        first = alloc_alloc(&ad, 32, 0);
        EXPECT(first != NULL);
        alloc_free(&ad, first, 32, 0);
    }
    EXPECT(arena_used(&a) == used_before);
    EXPECT(arena_committed(&a) == committed_before);

    /* Freeing a non-last allocation is a harmless no-op. */
    first = alloc_alloc(&ad, TEST_SMALL, 0);
    second = alloc_alloc(&ad, TEST_SMALL, 0);
    EXPECT(first != NULL && second != NULL);
    used_before = arena_used(&a);
    alloc_free(&ad, first, TEST_SMALL, 0);
    EXPECT(arena_used(&a) == used_before);

    /* A garbage size stays a no-op instead of wrapping into a bogus
     * cursor rollback. The invalid free released nothing, so second is
     * still live below. */
    alloc_free(&ad, second, huge, 0);
    EXPECT(arena_used(&a) == used_before);

    /* Realloc through the adapter: last allocation grows in place. */
    bytes = alloc_realloc(&ad, second, TEST_SMALL, TEST_LARGE, 0);
    EXPECT((void *)bytes == second);

    /* The adapter stays usable after a reset; only memory handed out
     * before the reset is dead. */
    arena_reset(&a);
    first = alloc_alloc(&ad, TEST_SMALL, 0);
    EXPECT(first != NULL);

    /* NULL arena: the adapter is inert. */
    ad = arena_allocator(NULL);
    EXPECT(alloc_alloc(&ad, TEST_SMALL, 0) == NULL);
    alloc_free(&ad, NULL, 0, 0);

    arena_deinit(&a);
    EXPECT(t.live_count == 0);
}

/* T6: an arena backed by another arena through the vtable. The child's
 * deinit releases blocks newest-first, so the parent arena rolls every
 * one of them back. */
static void test_arena_on_arena(void)
{
    static struct track_ctx t;
    arena_t parent_arena;
    arena_t child;
    size_t parent_used_before;
    unsigned char *ptr;
    size_t idx;

    memset(&t, 0, sizeof(t));
    EXPECT(arena_init_sized(&parent_arena, track_allocator(&t), 4096, 65536) == ARENA_OK);
    EXPECT(arena_alloc(&parent_arena, TEST_SMALL) != NULL); /* preexisting content */
    parent_used_before = arena_used(&parent_arena);

    EXPECT(arena_init_sized(&child, arena_allocator(&parent_arena), 256, 512) == ARENA_OK);
    for (idx = 0; idx < 8; idx++) {
        ptr = arena_alloc(&child, 100);
        EXPECT(ptr != NULL);
        memset(ptr, TEST_FILL_BYTE, 100);
        EXPECT(test_is_aligned(ptr, ARENA_DEFAULT_ALIGN));
    }
    EXPECT(arena_used(&parent_arena) > parent_used_before);

    arena_deinit(&child);
    /* Perfect LIFO release: the parent arena recovered every byte. */
    EXPECT(arena_used(&parent_arena) == parent_used_before);

    arena_deinit(&parent_arena);
    EXPECT(t.live_count == 0);
}

/*
 * T7: a deliberately nonconforming parent that returns 8-misaligned
 * blocks. The arena never relies on block base alignment, so every
 * allocation must still come out aligned.
 */

static void *misalign_xalloc(void *ctx, size_t size, size_t align)
{
    unsigned char *raw;

    (void)ctx;
    (void)align;
    if (size == 0) {
        return NULL;
    }
    raw = malloc(size + 8);
    EXPECT(raw != NULL);
    return raw + 8;
}

static void *misalign_xrealloc(void *ctx, void *ptr, size_t old_size, size_t new_size, size_t align)
{
    (void)ctx;
    (void)ptr;
    (void)old_size;
    (void)new_size;
    (void)align;
    EXPECT(0); /* the arena never reallocs parent blocks */
    return NULL;
}

static void misalign_xfree(void *ctx, void *ptr, size_t size, size_t align)
{
    (void)ctx;
    (void)size;
    (void)align;
    if (ptr != NULL) {
        free((unsigned char *)ptr - 8);
    }
}

static void test_alignment_with_misaligning_parent(void)
{
    alloc_t parent = {NULL, misalign_xalloc, misalign_xrealloc, misalign_xfree};
    arena_t a;
    void *ptr;
    size_t align;

    EXPECT(arena_init_sized(&a, parent, 256, 1024) == ARENA_OK);
    for (align = 1; align <= ARENA_MAX_ALIGN; align *= 2) {
        ptr = arena_alloc_n(&a, 1, TEST_SMALL, align);
        EXPECT(ptr != NULL);
        EXPECT(test_is_aligned(ptr, align));
        memset(ptr, TEST_FILL_BYTE, TEST_SMALL);
    }
    ptr = arena_alloc(&a, TEST_MEDIUM);
    EXPECT(ptr != NULL);
    EXPECT(test_is_aligned(ptr, ARENA_DEFAULT_ALIGN));
    arena_deinit(&a);
}

/* T6: one representative workout, run against every parent the library
 * ships plus the arena's own adapter, proving parent-independence. */
static void run_workout(alloc_t parent)
{
    arena_t a;
    arena_temp_t temp;
    unsigned char *ptr;
    unsigned char *moved;
    size_t idx;
    size_t round;

    EXPECT(arena_init_sized(&a, parent, 256, 1024) == ARENA_OK);
    for (round = 0; round < 3; round++) {
        temp = arena_temp_begin(&a);
        for (idx = 0; idx < 25; idx++) {
            ptr = arena_alloc(&a, 90);
            EXPECT(ptr != NULL);
            memset(ptr, TEST_FILL_BYTE, 90);
        }
        moved = arena_realloc(&a, ptr, 90, 700, 0);
        EXPECT(moved != NULL);
        EXPECT(moved[89] == TEST_FILL_BYTE);
        EXPECT(arena_alloc(&a, 3000) != NULL); /* oversize */
        arena_temp_end(temp);
        EXPECT(arena_used(&a) == 0);
    }
    EXPECT(ARENA_NEW_N(&a, double, 12) != NULL);
    arena_reset(&a);
    EXPECT(arena_alloc(&a, TEST_MEDIUM) != NULL);
    arena_deinit(&a);
}

static void test_parent_matrix(void)
{
    static struct track_ctx t;
    arena_t outer;

    run_workout(alloc_libc());

    memset(&t, 0, sizeof(t));
    run_workout(track_allocator(&t));
    EXPECT(t.live_count == 0);

    memset(&t, 0, sizeof(t));
    EXPECT(arena_init_sized(&outer, track_allocator(&t), 16384, 65536) == ARENA_OK);
    run_workout(arena_allocator(&outer));
    arena_deinit(&outer);
    EXPECT(t.live_count == 0);
}

/* T3: the seeded property loop on a tracked growing arena. */
static void test_random_ops_growing(void)
{
    static struct track_ctx t;
    arena_t a;

    memset(&t, 0, sizeof(t));
    EXPECT(arena_init_sized(&a, track_allocator(&t), 256, 4096) == ARENA_OK);
    run_random_ops(&a, 0x9E3779B9u);
    arena_deinit(&a);
    EXPECT(t.live_count == 0);
}

/* T6: the libc backend contract table. */
static void test_alloc_libc_contract(void)
{
    alloc_t la = alloc_libc();
    alloc_t copy;
    unsigned char *bytes;
    void *ptr;
    void *grown;
    size_t align;
    size_t idx;

    /* Zero size is NULL-as-success. */
    EXPECT(alloc_alloc(&la, 0, 0) == NULL);

    /* Default alignment satisfies max_align_t. */
    ptr = alloc_alloc(&la, TEST_MEDIUM, 0);
    EXPECT(ptr != NULL);
    EXPECT(test_is_aligned(ptr, _Alignof(max_align_t)));
    memset(ptr, TEST_FILL_BYTE, TEST_MEDIUM);
    alloc_free(&la, ptr, TEST_MEDIUM, 0);

    /* Every supported power of two. */
    for (align = 1; align <= _Alignof(max_align_t); align *= 2) {
        ptr = alloc_alloc(&la, TEST_SMALL, align);
        EXPECT(ptr != NULL);
        EXPECT(test_is_aligned(ptr, align));
        alloc_free(&la, ptr, TEST_SMALL, align);
    }

    /* Over-aligned requests are refused, never mis-served. */
    EXPECT(alloc_alloc(&la, TEST_MEDIUM, (size_t) _Alignof(max_align_t) * 2) == NULL);

    /* The PTRDIFF_MAX cap. */
    EXPECT(alloc_alloc(&la, SIZE_MAX, 0) == NULL);
    EXPECT(alloc_alloc(&la, (size_t)PTRDIFF_MAX + 1, 0) == NULL);
    EXPECT(alloc_realloc(&la, NULL, 0, (size_t)PTRDIFF_MAX + 1, 0) == NULL);

    /* Realloc from NULL behaves as alloc. */
    ptr = alloc_realloc(&la, NULL, 0, TEST_SMALL, 0);
    EXPECT(ptr != NULL);
    bytes = ptr;
    for (idx = 0; idx < TEST_SMALL; idx++) {
        bytes[idx] = (unsigned char)idx;
    }

    /* Growth preserves the prefix. */
    grown = alloc_realloc(&la, ptr, TEST_SMALL, TEST_LARGE, 0);
    EXPECT(grown != NULL);
    bytes = grown;
    for (idx = 0; idx < TEST_SMALL; idx++) {
        EXPECT(bytes[idx] == (unsigned char)idx);
    }

    /* Shrink preserves the prefix and never fails in this backend. */
    ptr = alloc_realloc(&la, grown, TEST_LARGE, TEST_SMALL / 2, 0);
    EXPECT(ptr != NULL);
    bytes = ptr;
    for (idx = 0; idx < TEST_SMALL / 2; idx++) {
        EXPECT(bytes[idx] == (unsigned char)idx);
    }
    alloc_free(&la, ptr, TEST_SMALL / 2, 0);

    /* free(NULL) is a no-op. */
    alloc_free(&la, NULL, 0, 0);

    /* Zeroed convenience really zeroes. */
    ptr = alloc_zeroed(&la, TEST_LARGE, 0);
    EXPECT(ptr != NULL);
    bytes = ptr;
    for (idx = 0; idx < TEST_LARGE; idx++) {
        EXPECT(bytes[idx] == 0);
    }
    alloc_free(&la, ptr, TEST_LARGE, 0);

    /* The struct is a value: a copy is the same allocator. */
    copy = la;
    ptr = alloc_alloc(&copy, TEST_SMALL, 0);
    EXPECT(ptr != NULL);
    alloc_free(&la, ptr, TEST_SMALL, 0);
}

#endif /* ARENA_TEST_FIXED_ONLY */

#if defined(TEST_HAS_ASAN) && !defined(ARENA_TEST_FIXED_ONLY)

#include <sanitizer/asan_interface.h>

/* T2/T3: the poisoning contract holds exactly where the headers
 * promises it. */
static void test_asan_poisoning(void)
{
    arena_t a;
    arena_temp_t temp;
    unsigned char *probe;
    unsigned char *rewound;
    unsigned char *grown;
    alloc_t ad;

    EXPECT(arena_init_sized(&a, alloc_libc(), 1024, 4096) == ARENA_OK);

    /* Exact user size unpoisoned, redzone right behind it. */
    probe = arena_alloc(&a, 40);
    EXPECT(probe != NULL);
    EXPECT(!__asan_address_is_poisoned(probe));
    EXPECT(!__asan_address_is_poisoned(probe + 39));
    EXPECT(__asan_address_is_poisoned(probe + 40));

    /* Rewound regions are poisoned. */
    temp = arena_temp_begin(&a);
    rewound = arena_alloc(&a, TEST_MEDIUM);
    EXPECT(rewound != NULL);
    memset(rewound, 1, TEST_MEDIUM);
    arena_temp_end(temp);
    EXPECT(__asan_address_is_poisoned(rewound));

    /* In-place growth unpoisons the extension (the protobuf #20565
     * regression shape) and the extension is really writable. */
    probe = arena_alloc(&a, 32);
    EXPECT(probe != NULL);
    grown = arena_realloc(&a, probe, 32, 200, 0);
    EXPECT(grown == probe);
    EXPECT(!__asan_address_is_poisoned(grown + 100));
    memset(grown, 2, 200);

    /* In-place shrink re-poisons the tail. */
    EXPECT(arena_realloc(&a, grown, 200, 50, 0) == grown);
    EXPECT(__asan_address_is_poisoned(grown + 64));

    /* Adapter rollback poisons the freed allocation. */
    ad = arena_allocator(&a);
    probe = alloc_alloc(&ad, TEST_MEDIUM, 0);
    EXPECT(probe != NULL);
    alloc_free(&ad, probe, TEST_MEDIUM, 0);
    EXPECT(__asan_address_is_poisoned(probe));

    /* Reset poisons every rewound region too. */
    probe = arena_alloc(&a, TEST_MEDIUM);
    EXPECT(probe != NULL);
    memset(probe, 3, TEST_MEDIUM);
    arena_reset(&a);
    EXPECT(__asan_address_is_poisoned(probe));

    arena_deinit(&a);
}

/* T2: a fixed arena hands the caller's storage back fully addressable. */
static void test_asan_fixed_buffer_returns_clean(void)
{
    static _Alignas(max_align_t) unsigned char buffer[256];
    arena_t a;
    size_t idx;

    arena_init_fixed(&a, buffer, sizeof(buffer));
    EXPECT(arena_alloc(&a, TEST_SMALL) != NULL);
    arena_deinit(&a);
    for (idx = 0; idx < sizeof(buffer); idx++) {
        EXPECT(!__asan_address_is_poisoned(buffer + idx));
    }
    memset(buffer, 0, sizeof(buffer));
}

#endif /* TEST_HAS_ASAN && !ARENA_TEST_FIXED_ONLY */

int main(void)
{
    test_alloc_null_contract();
    test_fixed_basics();
    test_fixed_misaligned_starts();
    test_fixed_tiny_buffers();
    test_zero_size_table();
    test_arg_and_overflow_guards();
    test_memdup_content();
    test_queries_and_names();
    test_init_sized_validation();
    test_deinit_cycles();
    test_fixed_reset();
    test_temp_scopes();
    test_realloc_fixed();
    test_random_ops_fixed();
#if !defined(NDEBUG) && defined(__unix__)
    test_temp_violation_death();
#if defined(TEST_HAS_ASAN) && !defined(ARENA_TEST_FIXED_ONLY)
    test_use_after_rewind_death();
#endif
#endif
#ifndef ARENA_TEST_FIXED_ONLY
    test_alloc_libc_contract();
    test_growth_and_blocks();
    test_reset_and_trim();
    test_sticky_gating_parent_calls();
    test_oom_loops();
    test_temp_block_recycling();
    test_temp_oversize_release();
    test_realloc_matrix();
    test_adapter_contract();
    test_arena_on_arena();
    test_alignment_with_misaligning_parent();
    test_parent_matrix();
    test_random_ops_growing();
#if defined(TEST_HAS_ASAN)
    test_asan_poisoning();
    test_asan_fixed_buffer_returns_clean();
#endif
#endif

    if (test_ctx.run == 0) {
        fprintf(stderr, "FAIL: test runner discovered zero tests\n");
        return 1;
    }
    printf("OK: %d checks\n", test_ctx.run);
    return 0;
}
