/*
 * fuzz_arena.c: libFuzzer op-sequence harness with a shadow model.
 *
 * Input bytes decode a sequence of arena operations. A shadow model
 * tracks every live allocation (address, size, align, fill seed, temp
 * level) and every open temp scope, performs only contract-legal calls,
 * and after every operation verifies that all live allocations still
 * hold their fill pattern, are aligned as requested, and that the
 * arena's statistics stay coherent. Runs under fuzzer,address,undefined
 * with arena poisoning active, so overlap and use-after-rewind are
 * directly observable. The first input byte selects the backend: fixed
 * buffer or growing with the libc parent.
 */

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "alloc.h"
#include "arena.h"

enum {
    FUZZ_MAX_LIVE = 64,
    FUZZ_MAX_TEMPS = 8,
    FUZZ_FIXED_BUFFER = 1 << 16,
    FUZZ_MAX_TOTAL_OPS = 512
};

struct fuzz_entry {
    unsigned char *ptr;
    size_t size;
    size_t align;
    unsigned char seed;
    unsigned level; /* temp depth at (re)allocation */
};

struct fuzz_model {
    struct fuzz_entry live[FUZZ_MAX_LIVE];
    size_t live_count;
    arena_temp_t temps[FUZZ_MAX_TEMPS];
    unsigned temp_depth;
    unsigned char next_seed;
};

struct fuzz_input {
    const uint8_t *data;
    size_t size;
    size_t pos;
};

static uint8_t fuzz_byte(struct fuzz_input *in)
{
    if (in->pos >= in->size) {
        return 0;
    }
    return in->data[in->pos++];
}

static size_t fuzz_pick_size(struct fuzz_input *in)
{
    uint8_t class_byte = fuzz_byte(in);
    uint8_t low = fuzz_byte(in);
    uint8_t high = fuzz_byte(in);
    size_t value = (size_t)low | ((size_t)high << 8);

    switch (class_byte % 4u) {
    case 0:
        return value % 65u; /* 0..64 */
    case 1:
        return value % 1025u;
    case 2:
        return value % 8193u;
    default:
        /* Rare huge sizes exercise the overflow guards. */
        return SIZE_MAX - (value % 3u);
    }
}

static size_t fuzz_pick_align(struct fuzz_input *in)
{
    uint8_t shift = fuzz_byte(in);

    if (shift % 4u == 0) {
        return 0; /* default */
    }
    return (size_t)1 << (shift % 14u); /* 1..8192 */
}

static void fuzz_fill(struct fuzz_entry *entry)
{
    size_t idx;

    for (idx = 0; idx < entry->size; idx++) {
        entry->ptr[idx] = (unsigned char)(entry->seed + (unsigned char)idx);
    }
}

static void fuzz_verify_entry(const struct fuzz_entry *entry)
{
    size_t idx;
    size_t effective = entry->align == 0 ? ARENA_DEFAULT_ALIGN : entry->align;

    assert(((uintptr_t)entry->ptr % (uintptr_t)effective) == 0);
    for (idx = 0; idx < entry->size; idx++) {
        assert(entry->ptr[idx] == (unsigned char)(entry->seed + (unsigned char)idx));
    }
}

static void fuzz_verify_all(const struct fuzz_model *m, const arena_t *a)
{
    size_t idx;

    for (idx = 0; idx < m->live_count; idx++) {
        fuzz_verify_entry(&m->live[idx]);
    }
    assert(arena_used(a) <= arena_committed(a) || arena_committed(a) == 0);
    assert(arena_high_water(a) >= arena_used(a));
}

static void fuzz_remove(struct fuzz_model *m, size_t idx)
{
    m->live[idx] = m->live[m->live_count - 1];
    m->live_count--;
}

/* Drops every model entry allocated at or above the ended depth. */
static void fuzz_drop_level(struct fuzz_model *m, unsigned depth)
{
    size_t idx = 0;

    while (idx < m->live_count) {
        if (m->live[idx].level >= depth) {
            fuzz_remove(m, idx);
        } else {
            idx++;
        }
    }
}

static void fuzz_record(struct fuzz_model *m, void *ptr, size_t size, size_t align)
{
    struct fuzz_entry *entry = &m->live[m->live_count];

    entry->ptr = ptr;
    entry->size = size;
    entry->align = align;
    entry->seed = m->next_seed++;
    entry->level = m->temp_depth;
    fuzz_fill(entry);
    m->live_count++;
}

static void fuzz_handle_failure(arena_t *a, size_t size)
{
    if (size != 0) {
        assert(arena_failed(a));
        arena_clear_error(a);
    } else {
        assert(!arena_failed(a));
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static _Alignas(max_align_t) unsigned char fixed_buffer[FUZZ_FIXED_BUFFER];
    struct fuzz_input in = {data, size, 0};
    struct fuzz_model m;
    arena_t a;
    alloc_t adapter;
    bool fixed;
    size_t ops;
    uint8_t op;
    size_t req_size;
    size_t req_align;
    void *ptr;
    size_t idx;
    struct fuzz_entry *entry;
    size_t new_size;

    memset(&m, 0, sizeof(m));
    fixed = (fuzz_byte(&in) & 1u) != 0;
    if (fixed) {
        arena_init_fixed(&a, fixed_buffer, sizeof(fixed_buffer));
    } else {
        assert(arena_init_sized(&a, alloc_libc(), 256, 8192) == ARENA_OK);
    }
    adapter = arena_allocator(&a);

    for (ops = 0; ops < FUZZ_MAX_TOTAL_OPS && in.pos < in.size; ops++) {
        op = fuzz_byte(&in);
        switch (op % 10u) {
        case 0: /* alloc */
        case 1:
            if (m.live_count >= FUZZ_MAX_LIVE) {
                break;
            }
            req_size = fuzz_pick_size(&in);
            req_align = fuzz_pick_align(&in);
            ptr = arena_alloc_n(&a, 1, req_size, req_align);
            if (ptr == NULL) {
                fuzz_handle_failure(&a, req_size);
                break;
            }
            fuzz_record(&m, ptr, req_size, req_align);
            break;
        case 2: /* zeroed alloc, verified zero before filling */
            if (m.live_count >= FUZZ_MAX_LIVE) {
                break;
            }
            req_size = fuzz_pick_size(&in) % 4096u;
            ptr = arena_alloc_zeroed(&a, req_size);
            if (ptr == NULL) {
                fuzz_handle_failure(&a, req_size);
                break;
            }
            for (idx = 0; idx < req_size; idx++) {
                assert(((unsigned char *)ptr)[idx] == 0);
            }
            fuzz_record(&m, ptr, req_size, 0);
            break;
        case 3: /* memdup of an existing entry's contents */
            if (m.live_count == 0 || m.live_count >= FUZZ_MAX_LIVE) {
                break;
            }
            idx = fuzz_byte(&in) % m.live_count;
            entry = &m.live[idx];
            ptr = arena_memdup(&a, entry->ptr, entry->size);
            if (ptr == NULL) {
                fuzz_handle_failure(&a, entry->size);
                break;
            }
            assert(entry->size == 0 || memcmp(ptr, entry->ptr, entry->size) == 0);
            fuzz_record(&m, ptr, entry->size, 0);
            /* fuzz_record refilled it; both entries verify independently
             * because record gave the copy its own seed. */
            break;
        case 4: /* realloc a live in-scope entry with exact old size and
                 * align; older entries are off limits while a temp scope
                 * is open (the header's in-scope-only rule) */
            if (m.live_count == 0) {
                break;
            }
            idx = fuzz_byte(&in) % m.live_count;
            entry = &m.live[idx];
            if (entry->level != m.temp_depth) {
                break;
            }
            new_size = fuzz_pick_size(&in);
            if (new_size == 0) {
                new_size = 1;
            }
            ptr = arena_realloc(&a, entry->ptr, entry->size, new_size, entry->align);
            if (ptr == NULL) {
                assert(arena_failed(&a));
                arena_clear_error(&a);
                fuzz_verify_entry(entry); /* the old block stayed valid */
                break;
            }
            entry->ptr = ptr;
            entry->size = new_size;
            entry->level = m.temp_depth; /* the result is a fresh allocation */
            entry->seed = m.next_seed++;
            fuzz_fill(entry);
            break;
        case 5: /* temp begin */
            if (m.temp_depth >= FUZZ_MAX_TEMPS) {
                break;
            }
            m.temps[m.temp_depth] = arena_temp_begin(&a);
            m.temp_depth++;
            break;
        case 6: /* temp end */
            if (m.temp_depth == 0) {
                break;
            }
            m.temp_depth--;
            fuzz_drop_level(&m, m.temp_depth + 1u);
            arena_temp_end(m.temps[m.temp_depth]);
            break;
        case 7: /* reset drops everything, including open temps */
            arena_reset(&a);
            m.live_count = 0;
            m.temp_depth = 0;
            break;
        case 8: /* trim, plus adapter alloc as an alternate entry path */
            arena_trim(&a);
            if (m.live_count >= FUZZ_MAX_LIVE) {
                break;
            }
            req_size = fuzz_byte(&in);
            ptr = alloc_alloc(&adapter, req_size, 0);
            if (ptr == NULL) {
                fuzz_handle_failure(&a, req_size);
                break;
            }
            fuzz_record(&m, ptr, req_size, 0);
            break;
        default: /* adapter free of the newest in-scope entry (LIFO
                  * rollback or no-op); older entries are off limits
                  * while a temp scope is open */
            if (m.live_count == 0) {
                break;
            }
            idx = m.live_count - 1;
            entry = &m.live[idx];
            if (entry->level != m.temp_depth) {
                break;
            }
            alloc_free(&adapter, entry->ptr, entry->size, entry->align);
            fuzz_remove(&m, idx);
            break;
        }
        fuzz_verify_all(&m, &a);
    }

    while (m.temp_depth > 0) {
        m.temp_depth--;
        fuzz_drop_level(&m, m.temp_depth + 1u);
        arena_temp_end(m.temps[m.temp_depth]);
        fuzz_verify_all(&m, &a);
    }
    arena_deinit(&a);
    return 0;
}
