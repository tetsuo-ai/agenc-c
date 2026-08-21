/*
 * bench_arena.c: microbenchmarks for the arena hot paths.
 *
 * Reports nanoseconds per operation as the minimum over repetitions,
 * which is the standard microbenchmark estimator for the true cost
 * (larger samples only add scheduling noise). Every allocation is
 * written to and folded into a volatile sink so the optimizer cannot
 * delete the work. malloc rows are context, not competition: the
 * interesting comparisons are between arena paths and across arena
 * changes.
 */

#if defined(__unix__)
#define _POSIX_C_SOURCE 199309L
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "alloc.h"
#include "arena.h"

enum {
    BENCH_REPS = 5,
    BENCH_BATCH = 1024,
    BENCH_BATCHES = 2048,
    BENCH_TEMP_ALLOCS = 32,
    BENCH_TEMP_CYCLES = 200000,
    BENCH_PAIR_OPS = 2000000,
    BENCH_REALLOC_CHAINS = 200000
};

static volatile unsigned char bench_sink;
/* Escaping each malloc result stops clang from legally eliding whole
 * malloc/free pairs; the arena calls are in another translation unit
 * and cannot be elided. */
static unsigned char *volatile bench_escape;

static uint64_t bench_now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* Batches of allocations, then reset, so steady state runs from
 * retained blocks (or the fixed buffer). Returns operations performed. */
static uint64_t bench_alloc_batches(arena_t *a, size_t size, size_t batch_size)
{
    uint64_t ops = 0;
    size_t batch;
    size_t idx;

    for (batch = 0; batch < BENCH_BATCHES; batch++) {
        for (idx = 0; idx < batch_size; idx++) {
            unsigned char *ptr = arena_alloc(a, size);

            if (ptr == NULL) {
                fprintf(stderr, "bench: unexpected allocation failure\n");
                exit(1);
            }
            ptr[0] = (unsigned char)idx;
            bench_sink = ptr[0];
            ops++;
        }
        arena_reset(a);
    }
    return ops;
}

static double bench_alloc_ns(size_t size)
{
    uint64_t best = UINT64_MAX;
    uint64_t ops = 0;
    int rep;

    for (rep = 0; rep < BENCH_REPS; rep++) {
        arena_t a;
        uint64_t start;
        uint64_t elapsed;

        arena_init(&a, alloc_libc());
        start = bench_now_ns();
        ops = bench_alloc_batches(&a, size, BENCH_BATCH);
        elapsed = bench_now_ns() - start;
        arena_deinit(&a);
        if (elapsed < best) {
            best = elapsed;
        }
    }
    return (double)best / (double)ops;
}

static double bench_fixed_alloc_ns(size_t size)
{
    static _Alignas(max_align_t) unsigned char buffer[1 << 16];
    uint64_t best = UINT64_MAX;
    uint64_t ops = 0;
    int rep;

    for (rep = 0; rep < BENCH_REPS; rep++) {
        arena_t a;
        uint64_t start;
        uint64_t elapsed;

        arena_init_fixed(&a, buffer, sizeof(buffer));
        start = bench_now_ns();
        /* Half the buffer's worth per batch leaves room for padding. */
        ops = bench_alloc_batches(&a, size, sizeof(buffer) / size / 2);
        elapsed = bench_now_ns() - start;
        arena_deinit(&a);
        if (elapsed < best) {
            best = elapsed;
        }
    }
    return (double)best / (double)ops;
}

static double bench_temp_cycle_ns(void)
{
    uint64_t best = UINT64_MAX;
    int rep;

    for (rep = 0; rep < BENCH_REPS; rep++) {
        arena_t a;
        uint64_t start;
        uint64_t elapsed;
        size_t cycle;
        size_t idx;

        arena_init(&a, alloc_libc());
        start = bench_now_ns();
        for (cycle = 0; cycle < BENCH_TEMP_CYCLES; cycle++) {
            arena_temp_t temp = arena_temp_begin(&a);

            for (idx = 0; idx < BENCH_TEMP_ALLOCS; idx++) {
                unsigned char *ptr = arena_alloc(&a, 128);

                if (ptr == NULL) {
                    fprintf(stderr, "bench: unexpected allocation failure\n");
                    exit(1);
                }
                ptr[0] = (unsigned char)idx;
                bench_sink = ptr[0];
            }
            arena_temp_end(temp);
        }
        elapsed = bench_now_ns() - start;
        arena_deinit(&a);
        if (elapsed < best) {
            best = elapsed;
        }
    }
    return (double)best / ((double)BENCH_TEMP_CYCLES * (double)BENCH_TEMP_ALLOCS);
}

static double bench_adapter_pair_ns(void)
{
    uint64_t best = UINT64_MAX;
    int rep;

    for (rep = 0; rep < BENCH_REPS; rep++) {
        arena_t a;
        alloc_t adapter;
        uint64_t start;
        uint64_t elapsed;
        size_t idx;

        arena_init(&a, alloc_libc());
        adapter = arena_allocator(&a);
        start = bench_now_ns();
        for (idx = 0; idx < BENCH_PAIR_OPS; idx++) {
            unsigned char *ptr = alloc_alloc(&adapter, 64, 0);

            if (ptr == NULL) {
                fprintf(stderr, "bench: unexpected allocation failure\n");
                exit(1);
            }
            ptr[0] = (unsigned char)idx;
            bench_sink = ptr[0];
            alloc_free(&adapter, ptr, 64, 0);
        }
        elapsed = bench_now_ns() - start;
        arena_deinit(&a);
        if (elapsed < best) {
            best = elapsed;
        }
    }
    return (double)best / (double)BENCH_PAIR_OPS;
}

static double bench_realloc_chain_ns(void)
{
    uint64_t best = UINT64_MAX;
    uint64_t ops = 0;
    int rep;

    for (rep = 0; rep < BENCH_REPS; rep++) {
        arena_t a;
        uint64_t start;
        uint64_t elapsed;
        size_t chain;

        arena_init(&a, alloc_libc());
        start = bench_now_ns();
        ops = 0;
        for (chain = 0; chain < BENCH_REALLOC_CHAINS; chain++) {
            unsigned char *ptr = arena_alloc(&a, 16);
            size_t held = 16;
            size_t next;

            if (ptr == NULL) {
                fprintf(stderr, "bench: unexpected allocation failure\n");
                exit(1);
            }
            for (next = 32; next <= 4096; next *= 2) {
                ptr = arena_realloc(&a, ptr, held, next, 0);
                if (ptr == NULL) {
                    fprintf(stderr, "bench: unexpected realloc failure\n");
                    exit(1);
                }
                held = next;
                ops++;
            }
            ptr[0] = (unsigned char)chain;
            bench_sink = ptr[0];
            arena_reset(&a);
        }
        elapsed = bench_now_ns() - start;
        arena_deinit(&a);
        if (elapsed < best) {
            best = elapsed;
        }
    }
    return (double)best / (double)ops;
}

static double bench_malloc_pair_ns(size_t size)
{
    uint64_t best = UINT64_MAX;
    int rep;

    for (rep = 0; rep < BENCH_REPS; rep++) {
        uint64_t start = bench_now_ns();
        uint64_t elapsed;
        size_t idx;

        for (idx = 0; idx < BENCH_PAIR_OPS; idx++) {
            unsigned char *ptr = malloc(size);

            if (ptr == NULL) {
                fprintf(stderr, "bench: malloc failure\n");
                exit(1);
            }
            ptr[0] = (unsigned char)idx;
            bench_escape = ptr;
            bench_sink = ptr[0];
            free(ptr);
        }
        elapsed = bench_now_ns() - start;
        if (elapsed < best) {
            best = elapsed;
        }
    }
    return (double)best / (double)BENCH_PAIR_OPS;
}

int main(void)
{
    printf("%-28s %10s\n", "case", "ns/op");
    printf("%-28s %10.2f\n", "arena_alloc 16", bench_alloc_ns(16));
    printf("%-28s %10.2f\n", "arena_alloc 64", bench_alloc_ns(64));
    printf("%-28s %10.2f\n", "arena_alloc 256", bench_alloc_ns(256));
    printf("%-28s %10.2f\n", "fixed arena_alloc 64", bench_fixed_alloc_ns(64));
    printf("%-28s %10.2f\n", "temp cycle alloc 128", bench_temp_cycle_ns());
    printf("%-28s %10.2f\n", "adapter LIFO pair 64", bench_adapter_pair_ns());
    printf("%-28s %10.2f\n", "realloc grow chain", bench_realloc_chain_ns());
    printf("%-28s %10.2f\n", "malloc/free pair 64 (context)", bench_malloc_pair_ns(64));
    return 0;
}
