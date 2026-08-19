/* str.c: owns growable byte strings, including their heap buffers. */

#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "str.h"

enum {
    STR_GROW_MIN = 16,
    STR_GROW_FACTOR = 2,
    STR_NUL_BYTES = 1,
    STR_FMT_STACK_BYTES = 256,
    STR_GROW_MAX_STEPS = (int)(sizeof(size_t) * (size_t)CHAR_BIT),
    STR_COMPARE_LESS = -1,
    STR_COMPARE_EQUAL = 0,
    STR_COMPARE_GREATER = 1
};

/* Propagates non-OK status. Permitted only in functions that acquire nothing. */
#define STR_TRY(expr)                                                                              \
    do {                                                                                           \
        str_status_t str_try_s_ = (expr);                                                          \
        if (str_try_s_ != STR_OK)                                                                  \
            return str_try_s_;                                                                     \
    } while (0)

typedef struct {
    bool inside;
    size_t idx;
} str_overlap_t;

typedef struct {
    const char *src;
    size_t len;
    size_t base_len;
} str_write_req_t;

typedef struct {
    str_view_t view;
    char *owned;
} str_staged_view_t;

typedef struct {
    size_t idx;
    size_t remove_len;
    size_t new_len;
    str_view_t replacement;
} str_replace_req_t;

typedef struct {
    char *buf;
    size_t space;
} str_fmt_dest_t;

typedef struct {
    str_fmt_dest_t stack;
    size_t rendered_len;
    const char *format;
} str_fmt_payload_t;

typedef enum { STR_SEARCH_FIRST, STR_SEARCH_LAST } str_search_mode_t;

typedef struct {
    size_t cut;
    size_t period;
} str_factor_t;

typedef struct {
    size_t idx;
    size_t memory;
} str_search_state_t;

typedef struct {
    str_view_t hay;
    str_view_t needle;
    str_factor_t factor;
    size_t full_shift;
    size_t saved_memory;
    size_t last_idx;
    str_search_mode_t mode;
} str_search_ctx_t;

typedef struct {
    str_factor_t factor;
    size_t candidate;
    size_t offset;
} str_suffix_walk_t;

#ifdef STR_TEST
/* Deviation: test-only mutable counter injects deterministic allocation failure. */
static size_t str_test_alloc_budget = SIZE_MAX;
#endif

static const char str_empty_buf[STR_NUL_BYTES] = {'\0'};

/* Returns STR_ERR_ARG. Sole producer of that status. */
static str_status_t str_arg_error(void);

/* Returns STR_ERR_RANGE. Sole producer of that status. */
static str_status_t str_range_error(void);

/* Returns STR_ERR_ALLOC. Sole producer of that status. */
static str_status_t str_alloc_error(void);

/* Returns STR_ERR_OVERFLOW. Sole producer of that status. */
static str_status_t str_overflow_error(void);

/* Returns STR_ERR_FMT. Sole producer of that status. */
static str_status_t str_fmt_error(void);

/* Sets fields to the canonical non-owning empty state. Does not free. */
static void str_reset_empty(str_t *s);

/* True when cap > 0, meaning buf is heap memory. */
static bool str_is_owned(const str_t *s);

/* Empties the string in place. Keeps any allocation. */
static void str_zero_content(str_t *s);

#ifdef STR_TEST
/* True when the current test budget requires an allocation failure. */
static bool str_test_should_fail_alloc(void);
#endif

/* True when the heap adapters may call libc. */
static bool str_heap_allowed(void);

/* Adapter over malloc. */
static void *str_heap_alloc(size_t bytes);

/* Adapter over realloc. */
static void *str_heap_resize(void *buf, size_t bytes);

/* Adapter over free. */
static void str_heap_release(void *buf);

/* Allocates fresh storage or resizes an owned buffer. */
static void *str_heap_expand(void *buf, size_t bytes, bool owned);

/* Rejects a NULL object. Passes a sticky non-OK status through. */
static str_status_t str_validate_mut(const str_t *s);

/* Rejects a NULL src when len > 0. */
static str_status_t str_validate_src(const char *src, size_t len);

/* Rejects a nonempty view with a NULL pointer. */
static str_status_t str_validate_view(str_view_t view);

/* Rejects a NULL s, a sticky error, or a NULL src when len > 0. */
static str_status_t str_begin_mut(str_t *s, const char *src, size_t len);

/* Rejects a span that does not lie inside [0, len]. */
static str_status_t str_validate_span(size_t len, size_t idx, size_t span);

/* Writes the C-string length of src inside a bounded initialized window. */
static str_status_t str_bounded_cstr_len(size_t *out_len, const char *src, size_t available);

/* Rejects invalid C-string input. Bounds self-source scanning to initialized bytes. */
static str_status_t str_require_cstr(str_t *s, size_t *out_len, const char *src);

/* Stores status on s. Leaves bytes unchanged. */
static str_status_t str_fail(str_t *s, str_status_t status);

/* Writes a + b. Sole arithmetic producer of STR_ERR_OVERFLOW. */
static str_status_t str_size_add(size_t *out, size_t a, size_t b);

/* Converts a nonnegative int to size_t without truncation. */
static str_status_t str_size_from_int(size_t *out, int value);

/* Writes len + add + NUL, the malloc size needed to hold the result. */
static str_status_t str_need_total(size_t *out_need, size_t len, size_t add);

/* Adapter over strlen. src is not NULL. */
static size_t str_c_len(const char *src);

/* Adapter over memcmp. True when both sides hold the same len bytes. */
static bool str_bytes_equal(const char *a, const char *b, size_t len);

/* Adapter over memmove. len == 0 is a no-op. */
static void str_move_bytes(char *dest, const char *src, size_t len);

/* Unsigned byte at idx. */
static unsigned char str_view_byte(str_view_t view, size_t idx);

/* Readable pointer for a valid view. NULL empty views use the static empty buffer. */
static const char *str_view_bytes(str_view_t view);

/* True when doubling cap would wrap size_t. */
static bool str_cap_would_wrap(size_t cap);

/* Doubles cap until it covers need. Bounded by STR_GROW_MAX_STEPS. */
static str_status_t str_grow_cap(size_t *out_cap, size_t cap, size_t need);

/* Chooses a geometric cap at least need. Bounded by STR_GROW_MAX_STEPS. */
static str_status_t str_choose_cap(size_t *out_cap, size_t current_cap, size_t need);

/* Replaces the heap buffer with one of new_cap bytes. */
static str_status_t str_replace_heap(str_t *s, size_t new_cap);

/* Grows s so cap >= need. Does not shrink. */
static str_status_t str_ensure_cap(str_t *s, size_t need);

/* True when src lies inside s->buf. Writes the byte index. */
static bool str_ptr_in_buf(const str_t *s, size_t *out_idx, const char *src);

/* Detects src inside s->buf. Rejects spans beyond initialized bytes plus NUL. */
static str_status_t str_probe_overlap(const str_t *s, str_overlap_t *out, const char *src,
                                      size_t len);

/* Grows s for an insert of len bytes. Writes overlap for the original src. */
static str_status_t str_insert_grow(str_t *s, str_overlap_t *out, const char *src, size_t len);

/* Grows s for a write of request.len after request.base_len. Rewrites overlapping src. */
static str_status_t str_prep_write(str_t *s, const char **out_src, str_write_req_t request);

/* Writes a NUL at s->len. */
static void str_term(str_t *s);

/* Replaces the body with len bytes. Capacity must already be enough. */
static void str_fill_n(str_t *s, const char *src, size_t len);

/* Appends len bytes. Capacity must already be enough. */
static void str_put_n(str_t *s, const char *src, size_t len);

/* Opens a hole of len bytes at idx. Capacity must already be enough. */
static void str_open_gap(str_t *s, size_t idx, size_t len);

/* Source index after opening a gap of len bytes at idx. */
static size_t str_shifted_idx(str_overlap_t overlap, size_t idx, size_t len);

/* Writes len bytes at idx without changing s->len. */
static void str_write_at(str_t *s, size_t idx, const char *src, size_t len);

/* Places src in a hole at idx. */
static void str_insert_apply(str_t *s, size_t idx, const char *src, size_t len);

/* Closes len bytes at idx. */
static void str_close_gap(str_t *s, size_t idx, size_t len);

/* Extends s to len, filling new bytes. Capacity must already be enough. */
static void str_extend_fill(str_t *s, size_t len, char fill);

/* Extends s to len with fill bytes. */
static str_status_t str_resize_grow(str_t *s, size_t len, char fill);

/* Releases an owned empty buffer back to the non-owning empty state. */
static str_status_t str_discard_heap(str_t *s);

/* Shrinks an owned buffer to exact_cap bytes. */
static str_status_t str_shrink_heap(str_t *s, size_t exact_cap);

/* Stages src only when it borrows from s. */
static str_status_t str_stage_view(const str_t *s, str_staged_view_t *out, str_view_t src);

/* Releases storage owned by a staged view. */
static void str_release_staged(str_staged_view_t *staged);

/* Replaces a validated span. Capacity must already cover new_len plus NUL. */
static void str_replace_apply(str_t *s, size_t idx, size_t remove_len, str_view_t replacement);

/* Replaces an equal-size span without allocation. */
static str_status_t str_replace_equal(str_t *s, size_t idx, str_view_t replacement);

/* Writes the result length of a replacement. */
static str_status_t str_replace_len(size_t *out_len, size_t old_len, size_t remove_len,
                                    size_t add_len);

/* Commits a size-changing replacement through staged source storage. */
static str_status_t str_replace_sized(str_t *s, str_replace_req_t request);

/* Applies a validated replacement that may change length. */
static str_status_t str_replace_commit(str_t *s, size_t idx, size_t remove_len,
                                       str_view_t replacement);

/* Writes one bounded printf result. Writes the untruncated length. */
static str_status_t str_fmt_write(size_t *out_len, str_fmt_dest_t dest, const char *format,
                                  va_list args);

/* Renders format into dest. Fails when the length is not expected_len. */
static str_status_t str_fmt_fill(str_fmt_dest_t dest, size_t expected_len, const char *format,
                                 va_list args);

/* Builds a heap buffer holding one formatted result of expected_len. */
static str_status_t str_fmt_build(char **out_buf, size_t expected_len, const char *format,
                                  va_list args);

/* Appends a staged printf result, heap-rendering when the stack buffer was short. */
static str_status_t str_append_fmt_payload(str_t *s, str_fmt_payload_t payload, va_list args);

/* View over s without a NULL check. */
static str_view_t str_view_of(const str_t *s);

/* Writes an empty view. */
static str_view_t str_view_empty(void);

/* Rejects NULL s, out_idx, or needle. */
static str_status_t str_validate_find(const str_t *s, const size_t *out_idx, const char *needle);

/* Writes the first or last needle index. Empty needle matches at 0 or hay.len. */
static str_status_t str_view_locate(str_view_t hay, size_t *out_idx, str_view_t needle,
                                    str_search_mode_t mode);

/* True when left precedes right under the selected byte order. */
static bool str_byte_is_before(unsigned char left, unsigned char right, bool reverse_order);

/* Advances the maximal-suffix walk by one compared byte pair. */
static void str_suffix_walk_step(str_suffix_walk_t *walk, unsigned char candidate_byte,
                                 unsigned char suffix_byte, bool reverse_order);

/* Returns the ordered maximal-suffix factorization. needle is nonempty. */
static str_factor_t str_maximal_suffix(str_view_t needle, bool reverse_order);

/* Returns the critical factorization across both byte orders. */
static str_factor_t str_critical_factor(str_view_t needle);

/* True when the candidate period covers the full needle. */
static bool str_factor_is_periodic(str_view_t needle, str_factor_t factor);

/* Returns the full-window shift for a nonperiodic needle. */
static size_t str_nonperiodic_shift(str_view_t needle, str_factor_t factor);

/* Returns the first right-half mismatch, or needle.len after a match. */
static size_t str_scan_right(str_view_t hay, str_view_t needle, str_factor_t factor,
                             str_search_state_t state);

/* True when the unchecked left half matches. */
static bool str_candidate_has_left_match(str_view_t hay, str_view_t needle, str_factor_t factor,
                                         str_search_state_t state);

/* Advances state without passing last_idx. False means the scan is complete. */
static bool str_search_advance(str_search_state_t *state, size_t last_idx, size_t shift);

/* Returns the requested occurrence of nonempty needle. */
static size_t str_view_search_mode(str_view_t hay, str_view_t needle, str_search_mode_t mode);

/* Builds immutable state for one Two-Way scan. */
static str_search_ctx_t str_search_context(str_view_t hay, str_view_t needle,
                                           str_search_mode_t mode);

/* Shifts past a right-half mismatch. True when another candidate remains. */
static bool str_search_on_mismatch(const str_search_ctx_t *ctx, str_search_state_t *state,
                                   size_t mismatch);

/* Records a full match when the left half agrees, then shifts. */
static bool str_search_after_right_match(const str_search_ctx_t *ctx, str_search_state_t *state,
                                         size_t *found);

/* Examines one candidate. True when another candidate remains. */
static bool str_search_step(const str_search_ctx_t *ctx, str_search_state_t *state, size_t *found);

/* First index of byte, or STR_NPOS. */
static size_t str_view_find_byte(str_view_t hay, unsigned char byte);

/* Last index of byte, or STR_NPOS. */
static size_t str_view_rfind_byte(str_view_t hay, unsigned char byte);

/* Returns the requested occurrence of one byte. */
static size_t str_view_search_byte(str_view_t hay, unsigned char byte, str_search_mode_t mode);

/* Normalizes the sign of memcmp for unsigned-byte lexical order. */
static int str_compare_bytes(const char *a, const char *b, size_t len);

/* Length tie-break for equal common prefixes. */
static int str_order_from_len(size_t a_len, size_t b_len);

/* True for ASCII space, tab, CR, LF, FF, VT. */
static bool str_is_ascii_whitespace(char c);

/* Drops leading ASCII whitespace. */
static str_view_t str_view_trim_left(str_view_t view);

/* Drops trailing ASCII whitespace. */
static str_view_t str_view_trim_right(str_view_t view);

/* Rejects a NULL out, a missing parts array, or a NULL src with len > 0. */
static str_status_t str_validate_split(str_view_t src, const str_split_out_t *out);

/* Stores one split part when count is inside out->cap. */
static void str_split_put(str_split_out_t *out, size_t count, const char *ptr, size_t len);

/* Next separator index at or after start, or STR_NPOS. */
static size_t str_find_separator(str_view_t src, size_t start, char separator);

/* Counts split parts without modifying caller-owned output storage. */
static str_status_t str_split_count(size_t *out_count, str_view_t src, char separator);

/* Writes split parts into caller-owned storage, truncating at out->cap. */
static void str_split_fill(str_split_out_t *out, str_view_t src, char separator);

void str_init(str_t *s)
{
    if (s == NULL)
        return;
    str_reset_empty(s);
}

void str_deinit(str_t *s)
{
    if (s == NULL)
        return;
    if (str_is_owned(s))
        str_heap_release(s->buf);
    str_reset_empty(s);
}

void str_clear(str_t *s)
{
    if (s == NULL)
        return;
    s->status = STR_OK;
    str_zero_content(s);
}

void str_clear_error(str_t *s)
{
    if (s == NULL)
        return;
    s->status = STR_OK;
}

void str_move(str_t *out, str_t *s)
{
    if (out == NULL || s == NULL)
        return;
    if (out == s)
        return;
    str_deinit(out);
    /* Ownership transfer. Callers still must not assign str_t. */
    *out = *s;
    str_reset_empty(s);
}

str_status_t str_copy(str_t *out, const str_t *s)
{
    if (out == NULL)
        return str_arg_error();

    str_status_t status = str_validate_mut(out);
    if (status != STR_OK)
        return status;
    if (s == NULL)
        return str_fail(out, str_arg_error());
    if (out == s)
        return STR_OK;

    str_t scratch = STR_EMPTY;
    status = str_set_n(&scratch, str_cstr(s), str_len(s));
    if (status != STR_OK) {
        str_deinit(&scratch);
        return str_fail(out, status);
    }
    str_move(out, &scratch);
    return STR_OK;
}

char *str_detach(str_t *s)
{
    if (s == NULL)
        return NULL;
    if (str_is_owned(s)) {
        char *owned = s->buf;
        str_reset_empty(s);
        return owned;
    }

    char *owned = str_heap_alloc((size_t)STR_NUL_BYTES);
    if (owned == NULL) {
        s->status = str_alloc_error();
        return NULL;
    }
    owned[0] = '\0';
    str_reset_empty(s);
    return owned;
}

const char *str_status_name(str_status_t status)
{
    switch (status) {
    case STR_OK:
        return "STR_OK";
    case STR_ERR_ARG:
        return "STR_ERR_ARG";
    case STR_ERR_ALLOC:
        return "STR_ERR_ALLOC";
    case STR_ERR_RANGE:
        return "STR_ERR_RANGE";
    case STR_ERR_OVERFLOW:
        return "STR_ERR_OVERFLOW";
    case STR_ERR_FMT:
        return "STR_ERR_FMT";
    default:
        return "STR_ERR_UNKNOWN";
    }
}

str_status_t str_set(str_t *s, const char *src)
{
    size_t len = 0;
    str_status_t status = str_require_cstr(s, &len, src);

    if (status != STR_OK)
        return status;
    return str_set_n(s, src, len);
}

str_status_t str_set_n(str_t *s, const char *src, size_t len)
{
    str_status_t status = str_begin_mut(s, src, len);

    if (status != STR_OK)
        return status;
    if (len == 0) {
        str_zero_content(s);
        return STR_OK;
    }

    const char *ready = NULL;
    str_write_req_t request = {.src = src, .len = len, .base_len = 0};
    status = str_prep_write(s, &ready, request);
    if (status != STR_OK)
        return str_fail(s, status);

    str_fill_n(s, ready, len);
    return STR_OK;
}

str_status_t str_set_view(str_t *s, str_view_t src)
{
    return str_set_n(s, src.ptr, src.len);
}

str_status_t str_append(str_t *s, const char *src)
{
    size_t len = 0;
    str_status_t status = str_require_cstr(s, &len, src);

    if (status != STR_OK)
        return status;
    return str_append_n(s, src, len);
}

str_status_t str_append_n(str_t *s, const char *src, size_t len)
{
    str_status_t status = str_begin_mut(s, src, len);

    if (status != STR_OK)
        return status;
    if (len == 0)
        return STR_OK;

    const char *ready = NULL;
    str_write_req_t request = {.src = src, .len = len, .base_len = s->len};
    status = str_prep_write(s, &ready, request);
    if (status != STR_OK)
        return str_fail(s, status);

    str_put_n(s, ready, len);
    return STR_OK;
}

str_status_t str_append_view(str_t *s, str_view_t src)
{
    return str_append_n(s, src.ptr, src.len);
}

str_status_t str_append_char(str_t *s, char c)
{
    char byte[1] = {c};

    return str_append_n(s, byte, 1);
}

str_status_t str_append_vfmt(str_t *s, const char *format, va_list args)
{
    size_t format_len = 0;
    str_status_t status = str_require_cstr(s, &format_len, format);

    if (status != STR_OK)
        return status;

    char stack_buf[STR_FMT_STACK_BYTES];
    size_t rendered_len = 0;
    str_fmt_dest_t stack_dest = {.buf = stack_buf, .space = sizeof(stack_buf)};
    status = str_fmt_write(&rendered_len, stack_dest, format, args);
    if (status != STR_OK)
        return str_fail(s, status);

    str_fmt_payload_t payload = {
        .stack = stack_dest,
        .rendered_len = rendered_len,
        .format = format,
    };
    return str_append_fmt_payload(s, payload, args);
}

str_status_t str_append_fmt(str_t *s, const char *format, ...)
{
    va_list args;

    va_start(args, format);
    str_status_t status = str_append_vfmt(s, format, args);
    va_end(args);
    return status;
}

str_status_t str_reserve(str_t *s, size_t len)
{
    str_status_t status = str_validate_mut(s);

    if (status != STR_OK)
        return status;
    if (len == 0)
        return STR_OK;

    size_t need = 0;
    status = str_need_total(&need, 0, len);
    if (status != STR_OK)
        return str_fail(s, status);
    status = str_ensure_cap(s, need);
    if (status != STR_OK)
        return str_fail(s, status);
    return STR_OK;
}

str_status_t str_shrink_to_fit(str_t *s)
{
    str_status_t status = str_validate_mut(s);

    if (status != STR_OK)
        return status;
    if (s->cap == 0)
        return STR_OK;
    if (s->len == 0)
        return str_discard_heap(s);

    size_t exact_cap = 0;
    status = str_size_add(&exact_cap, s->len, (size_t)STR_NUL_BYTES);
    if (status != STR_OK)
        return str_fail(s, status);
    status = str_shrink_heap(s, exact_cap);
    if (status != STR_OK)
        return str_fail(s, status);
    return STR_OK;
}

str_status_t str_resize(str_t *s, size_t len, char fill)
{
    str_status_t status = str_validate_mut(s);

    if (status != STR_OK)
        return status;
    if (len == s->len)
        return STR_OK;
    if (len < s->len) {
        str_close_gap(s, len, s->len - len);
        return STR_OK;
    }

    status = str_resize_grow(s, len, fill);
    if (status != STR_OK)
        return str_fail(s, status);
    return STR_OK;
}

str_status_t str_insert_n(str_t *s, size_t idx, const char *src, size_t len)
{
    str_status_t status = str_begin_mut(s, src, len);

    if (status != STR_OK)
        return status;
    status = str_validate_span(s->len, idx, 0);
    if (status != STR_OK)
        return str_fail(s, status);
    if (len == 0)
        return STR_OK;

    str_overlap_t overlap = {0};
    status = str_insert_grow(s, &overlap, src, len);
    if (status != STR_OK)
        return str_fail(s, status);

    const char *ready = src;
    if (overlap.inside)
        ready = s->buf + str_shifted_idx(overlap, idx, len);
    str_insert_apply(s, idx, ready, len);
    return STR_OK;
}

str_status_t str_insert_view(str_t *s, size_t idx, str_view_t src)
{
    return str_insert_n(s, idx, src.ptr, src.len);
}

str_status_t str_remove(str_t *s, size_t idx, size_t len)
{
    str_status_t status = str_validate_mut(s);

    if (status != STR_OK)
        return status;
    status = str_validate_span(s->len, idx, len);
    if (status != STR_OK)
        return str_fail(s, status);
    if (len == 0)
        return STR_OK;

    str_close_gap(s, idx, len);
    return STR_OK;
}

str_status_t str_replace_view(str_t *s, size_t idx, size_t remove_len, str_view_t replacement)
{
    str_status_t status = str_begin_mut(s, replacement.ptr, replacement.len);

    if (status != STR_OK)
        return status;
    status = str_validate_span(s->len, idx, remove_len);
    if (status != STR_OK)
        return str_fail(s, status);
    if (remove_len == 0 && replacement.len == 0)
        return STR_OK;

    status = str_replace_commit(s, idx, remove_len, replacement);
    if (status != STR_OK)
        return str_fail(s, status);
    return STR_OK;
}

const char *str_cstr(const str_t *s)
{
    if (s == NULL || s->buf == NULL)
        return str_empty_buf;
    return s->buf;
}

size_t str_len(const str_t *s)
{
    if (s == NULL)
        return 0;
    return s->len;
}

size_t str_capacity(const str_t *s)
{
    if (s == NULL || s->cap == 0)
        return 0;
    return s->cap - (size_t)STR_NUL_BYTES;
}

bool str_is_empty(const str_t *s)
{
    return str_len(s) == 0;
}

str_status_t str_status(const str_t *s)
{
    if (s == NULL)
        return str_arg_error();
    return s->status;
}

bool str_ok(const str_t *s)
{
    return s != NULL && s->status == STR_OK;
}

bool str_failed(const str_t *s)
{
    return !str_ok(s);
}

bool str_equals(const str_t *s, const str_t *other)
{
    if (s == NULL || other == NULL)
        return false;
    return str_view_equals(str_view_of(s), str_view_of(other));
}

bool str_equals_cstr(const str_t *s, const char *other)
{
    if (s == NULL || other == NULL)
        return false;
    return str_view_equals(str_view_of(s), str_view_from_cstr(other));
}

bool str_starts_with(const str_t *s, const char *prefix)
{
    if (s == NULL || prefix == NULL)
        return false;
    return str_view_starts_with(str_view_of(s), str_view_from_cstr(prefix));
}

bool str_ends_with(const str_t *s, const char *suffix)
{
    if (s == NULL || suffix == NULL)
        return false;
    return str_view_ends_with(str_view_of(s), str_view_from_cstr(suffix));
}

str_status_t str_find(const str_t *s, size_t *out_idx, const char *needle)
{
    STR_TRY(str_validate_find(s, out_idx, needle));
    return str_view_find(str_view_of(s), out_idx, str_view_from_cstr(needle));
}

str_status_t str_find_char(const str_t *s, size_t *out_idx, char c)
{
    if (s == NULL || out_idx == NULL)
        return str_arg_error();

    char byte[1] = {c};
    return str_view_find(str_view_of(s), out_idx, str_view_from_n(byte, 1));
}

str_view_t str_view(const str_t *s)
{
    if (s == NULL)
        return str_view_empty();
    return str_view_of(s);
}

str_status_t str_slice(const str_t *s, str_view_t *out, size_t off, size_t len)
{
    if (s == NULL || out == NULL)
        return str_arg_error();
    STR_TRY(str_validate_span(s->len, off, len));
    out->ptr = str_cstr(s) + off;
    out->len = len;
    return STR_OK;
}

str_view_t str_view_from_cstr(const char *src)
{
    if (src == NULL)
        return str_view_empty();
    return str_view_from_n(src, str_c_len(src));
}

str_view_t str_view_from_n(const char *src, size_t len)
{
    if (src == NULL && len == 0)
        return str_view_empty();
    return (str_view_t){.ptr = src, .len = len};
}

bool str_view_is_valid(str_view_t view)
{
    return view.ptr != NULL || view.len == 0;
}

bool str_view_equals(str_view_t a, str_view_t b)
{
    if (!str_view_is_valid(a) || !str_view_is_valid(b))
        return false;
    if (a.len != b.len)
        return false;
    return str_bytes_equal(a.ptr, b.ptr, a.len);
}

str_status_t str_view_compare(int *out_order, str_view_t a, str_view_t b)
{
    if (out_order == NULL)
        return str_arg_error();
    STR_TRY(str_validate_view(a));
    STR_TRY(str_validate_view(b));

    size_t common_len = a.len < b.len ? a.len : b.len;
    int order = str_compare_bytes(a.ptr, b.ptr, common_len);
    if (order == STR_COMPARE_EQUAL)
        order = str_order_from_len(a.len, b.len);
    *out_order = order;
    return STR_OK;
}

bool str_view_starts_with(str_view_t value, str_view_t prefix)
{
    if (!str_view_is_valid(value) || !str_view_is_valid(prefix))
        return false;
    if (prefix.len > value.len)
        return false;
    return str_bytes_equal(value.ptr, prefix.ptr, prefix.len);
}

bool str_view_ends_with(str_view_t value, str_view_t suffix)
{
    if (!str_view_is_valid(value) || !str_view_is_valid(suffix))
        return false;
    if (suffix.len > value.len)
        return false;
    if (suffix.len == 0)
        return true;
    return str_bytes_equal(value.ptr + (value.len - suffix.len), suffix.ptr, suffix.len);
}

str_status_t str_view_find(str_view_t hay, size_t *out_idx, str_view_t needle)
{
    return str_view_locate(hay, out_idx, needle, STR_SEARCH_FIRST);
}

str_status_t str_view_rfind(str_view_t hay, size_t *out_idx, str_view_t needle)
{
    return str_view_locate(hay, out_idx, needle, STR_SEARCH_LAST);
}

str_view_t str_view_trim(str_view_t view)
{
    if (!str_view_is_valid(view) || view.len == 0)
        return str_view_empty();
    return str_view_trim_right(str_view_trim_left(view));
}

str_status_t str_split_view(str_view_t src, str_split_out_t *out, char separator)
{
    size_t count = 0;

    STR_TRY(str_validate_split(src, out));
    STR_TRY(str_split_count(&count, src, separator));
    str_split_fill(out, src, separator);
    out->count = count;
    return STR_OK;
}

#ifdef STR_TEST
void str_test_fail_alloc_after(size_t success_count)
{
    str_test_alloc_budget = success_count;
}

void str_test_reset_alloc_failures(void)
{
    str_test_alloc_budget = SIZE_MAX;
}
#endif

static str_status_t str_arg_error(void)
{
    return STR_ERR_ARG;
}

static str_status_t str_range_error(void)
{
    return STR_ERR_RANGE;
}

static str_status_t str_alloc_error(void)
{
    return STR_ERR_ALLOC;
}

static str_status_t str_overflow_error(void)
{
    return STR_ERR_OVERFLOW;
}

static str_status_t str_fmt_error(void)
{
    return STR_ERR_FMT;
}

static void str_reset_empty(str_t *s)
{
    assert(s != NULL);
    s->buf = NULL;
    s->len = 0;
    s->cap = 0;
    s->status = STR_OK;
}

static bool str_is_owned(const str_t *s)
{
    assert(s != NULL);
    return s->cap > 0;
}

static void str_zero_content(str_t *s)
{
    assert(s != NULL);
    s->len = 0;
    if (s->cap == 0)
        return;
    assert(s->buf != NULL);
    s->buf[0] = '\0';
}

#ifdef STR_TEST
static bool str_test_should_fail_alloc(void)
{
    if (str_test_alloc_budget == SIZE_MAX)
        return false;
    if (str_test_alloc_budget == 0)
        return true;
    str_test_alloc_budget--;
    return false;
}
#endif

static bool str_heap_allowed(void)
{
#ifdef STR_TEST
    return !str_test_should_fail_alloc();
#else
    return true;
#endif
}

static void *str_heap_alloc(size_t bytes)
{
    if (!str_heap_allowed())
        return NULL;
    return malloc(bytes);
}

static void *str_heap_resize(void *buf, size_t bytes)
{
    if (!str_heap_allowed())
        return NULL;
    return realloc(buf, bytes);
}

static void str_heap_release(void *buf)
{
    free(buf);
}

static void *str_heap_expand(void *buf, size_t bytes, bool owned)
{
    if (owned)
        return str_heap_resize(buf, bytes);
    return str_heap_alloc(bytes);
}

static str_status_t str_validate_mut(const str_t *s)
{
    if (s == NULL)
        return str_arg_error();
    if (s->status != STR_OK)
        return s->status;
    return STR_OK;
}

static str_status_t str_validate_src(const char *src, size_t len)
{
    if (len == 0)
        return STR_OK;
    if (src == NULL)
        return str_arg_error();
    return STR_OK;
}

static str_status_t str_validate_view(str_view_t view)
{
    if (!str_view_is_valid(view))
        return str_arg_error();
    return STR_OK;
}

static str_status_t str_begin_mut(str_t *s, const char *src, size_t len)
{
    STR_TRY(str_validate_mut(s));

    str_status_t status = str_validate_src(src, len);
    if (status != STR_OK)
        return str_fail(s, status);
    return STR_OK;
}

static str_status_t str_validate_span(size_t len, size_t idx, size_t span)
{
    if (idx > len)
        return str_range_error();

    size_t end = 0;
    if (str_size_add(&end, idx, span) != STR_OK)
        return str_range_error();
    if (end > len)
        return str_range_error();
    return STR_OK;
}

static str_status_t str_bounded_cstr_len(size_t *out_len, const char *src, size_t available)
{
    assert(out_len != NULL);
    assert(src != NULL);

    const char *terminator = memchr(src, '\0', available);
    if (terminator == NULL)
        return str_arg_error();
    *out_len = (size_t)(terminator - src);
    return STR_OK;
}

static str_status_t str_require_cstr(str_t *s, size_t *out_len, const char *src)
{
    assert(out_len != NULL);
    STR_TRY(str_validate_mut(s));
    if (src == NULL)
        return str_fail(s, str_arg_error());

    size_t idx = 0;
    if (s->buf == NULL || !str_ptr_in_buf(s, &idx, src)) {
        *out_len = str_c_len(src);
        return STR_OK;
    }
    if (idx > s->len)
        return str_fail(s, str_arg_error());

    size_t available = (s->len - idx) + (size_t)STR_NUL_BYTES;
    str_status_t status = str_bounded_cstr_len(out_len, src, available);
    if (status != STR_OK)
        return str_fail(s, status);
    return STR_OK;
}

static str_status_t str_fail(str_t *s, str_status_t status)
{
    assert(s != NULL);
    assert(status != STR_OK);
    s->status = status;
    return status;
}

static str_status_t str_size_add(size_t *out, size_t a, size_t b)
{
    assert(out != NULL);
    if (a > SIZE_MAX - b)
        return str_overflow_error();
    *out = a + b;
    return STR_OK;
}

static str_status_t str_size_from_int(size_t *out, int value)
{
    assert(out != NULL);
    if (value < 0)
        return str_fmt_error();
    if ((uintmax_t)value > (uintmax_t)SIZE_MAX)
        return str_overflow_error();
    *out = (size_t)value;
    return STR_OK;
}

static str_status_t str_need_total(size_t *out_need, size_t len, size_t add)
{
    size_t bytes = 0;

    assert(out_need != NULL);
    STR_TRY(str_size_add(&bytes, len, add));
    return str_size_add(out_need, bytes, (size_t)STR_NUL_BYTES);
}

static size_t str_c_len(const char *src)
{
    assert(src != NULL);
    return strlen(src);
}

static bool str_bytes_equal(const char *a, const char *b, size_t len)
{
    if (len == 0)
        return true;
    assert(a != NULL);
    assert(b != NULL);
    return memcmp(a, b, len) == 0;
}

static int str_compare_bytes(const char *a, const char *b, size_t len)
{
    if (len == 0)
        return STR_COMPARE_EQUAL;
    assert(a != NULL);
    assert(b != NULL);

    int order = memcmp(a, b, len);
    if (order < 0)
        return STR_COMPARE_LESS;
    if (order > 0)
        return STR_COMPARE_GREATER;
    return STR_COMPARE_EQUAL;
}

static int str_order_from_len(size_t a_len, size_t b_len)
{
    if (a_len < b_len)
        return STR_COMPARE_LESS;
    if (a_len > b_len)
        return STR_COMPARE_GREATER;
    return STR_COMPARE_EQUAL;
}

static void str_move_bytes(char *dest, const char *src, size_t len)
{
    if (len == 0)
        return;
    assert(dest != NULL);
    assert(src != NULL);
    memmove(dest, src, len);
}

static unsigned char str_view_byte(str_view_t view, size_t idx)
{
    assert(view.ptr != NULL);
    assert(idx < view.len);
    return (unsigned char)view.ptr[idx];
}

static const char *str_view_bytes(str_view_t view)
{
    assert(str_view_is_valid(view));
    if (view.ptr == NULL)
        return str_empty_buf;
    return view.ptr;
}

static bool str_cap_would_wrap(size_t cap)
{
    return cap > SIZE_MAX / (size_t)STR_GROW_FACTOR;
}

static str_status_t str_grow_cap(size_t *out_cap, size_t cap, size_t need)
{
    assert(out_cap != NULL);
    for (size_t step = 0; step < (size_t)STR_GROW_MAX_STEPS; step++) {
        if (cap >= need) {
            *out_cap = cap;
            return STR_OK;
        }
        if (str_cap_would_wrap(cap)) {
            *out_cap = need;
            return STR_OK;
        }
        cap *= (size_t)STR_GROW_FACTOR;
    }
    if (cap >= need) {
        *out_cap = cap;
        return STR_OK;
    }
    return str_overflow_error();
}

static str_status_t str_choose_cap(size_t *out_cap, size_t current_cap, size_t need)
{
    assert(out_cap != NULL);
    if (need <= current_cap) {
        *out_cap = current_cap;
        return STR_OK;
    }

    size_t cap = current_cap;
    if (cap < (size_t)STR_GROW_MIN)
        cap = (size_t)STR_GROW_MIN;
    return str_grow_cap(out_cap, cap, need);
}

static str_status_t str_replace_heap(str_t *s, size_t new_cap)
{
    assert(s != NULL);
    assert(new_cap > s->cap);

    bool owned = str_is_owned(s);
    char *new_buf = str_heap_expand(s->buf, new_cap, owned);
    if (new_buf == NULL)
        return str_alloc_error();
    if (!owned)
        new_buf[0] = '\0';
    s->buf = new_buf;
    s->cap = new_cap;
    return STR_OK;
}

static str_status_t str_ensure_cap(str_t *s, size_t need)
{
    assert(s != NULL);
    if (need <= s->cap)
        return STR_OK;

    size_t new_cap = 0;
    str_status_t status = str_choose_cap(&new_cap, s->cap, need);
    if (status != STR_OK)
        return status;
    return str_replace_heap(s, new_cap);
}

static bool str_ptr_in_buf(const str_t *s, size_t *out_idx, const char *src)
{
    assert(s != NULL);
    assert(out_idx != NULL);
    assert(src != NULL);
    assert(s->buf != NULL);

#if defined(UINTPTR_MAX)
    /*
     * Deviation: uintptr_t ordering is implementation-defined. Mainstream flat-address
     * targets provide the constant-time path; other C11 targets use exact equality below.
     */
    uintptr_t src_addr = (uintptr_t)src;
    uintptr_t buf_addr = (uintptr_t)s->buf;
    if (src_addr < buf_addr)
        return false;

    uintptr_t offset = src_addr - buf_addr;
    if (offset > (uintptr_t)s->cap)
        return false;
    *out_idx = (size_t)offset;
    return true;
#else
    for (size_t idx = 0; idx <= s->cap; idx++) {
        if (src == s->buf + idx) {
            *out_idx = idx;
            return true;
        }
        if (idx == s->cap)
            return false;
    }
    return false;
#endif
}

static str_status_t str_probe_overlap(const str_t *s, str_overlap_t *out, const char *src,
                                      size_t len)
{
    assert(s != NULL);
    assert(out != NULL);

    out->inside = false;
    out->idx = 0;
    if (len == 0 || src == NULL || s->buf == NULL || s->cap == 0)
        return STR_OK;

    size_t idx = 0;
    if (!str_ptr_in_buf(s, &idx, src))
        return STR_OK;
    if (idx > s->len)
        return str_arg_error();

    size_t remain = (s->len - idx) + (size_t)STR_NUL_BYTES;
    if (len > remain)
        return str_arg_error();

    out->inside = true;
    out->idx = idx;
    return STR_OK;
}

static str_status_t str_insert_grow(str_t *s, str_overlap_t *out, const char *src, size_t len)
{
    assert(s != NULL);
    assert(out != NULL);

    size_t need = 0;
    str_status_t status = str_need_total(&need, s->len, len);
    if (status != STR_OK)
        return status;
    status = str_probe_overlap(s, out, src, len);
    if (status != STR_OK)
        return status;
    return str_ensure_cap(s, need);
}

static str_status_t str_prep_write(str_t *s, const char **out_src, str_write_req_t request)
{
    assert(s != NULL);
    assert(out_src != NULL);

    size_t need = 0;
    str_status_t status = str_need_total(&need, request.base_len, request.len);
    if (status != STR_OK)
        return status;

    str_overlap_t overlap = {0};
    status = str_probe_overlap(s, &overlap, request.src, request.len);
    if (status != STR_OK)
        return status;

    status = str_ensure_cap(s, need);
    if (status != STR_OK)
        return status;

    if (overlap.inside)
        *out_src = s->buf + overlap.idx;
    else
        *out_src = request.src;
    return STR_OK;
}

static void str_term(str_t *s)
{
    assert(s != NULL);
    assert(s->buf != NULL);
    if (s->cap == 0)
        return;
    assert(s->len < s->cap);
    s->buf[s->len] = '\0';
}

static void str_fill_n(str_t *s, const char *src, size_t len)
{
    assert(s != NULL);
    assert(s->buf != NULL);
    assert(len < s->cap);
    str_move_bytes(s->buf, src, len);
    s->len = len;
    str_term(s);
}

static void str_put_n(str_t *s, const char *src, size_t len)
{
    assert(s != NULL);
    assert(s->buf != NULL);
    assert(s->len < s->cap);
    assert(len <= s->cap - s->len - (size_t)STR_NUL_BYTES);
    str_move_bytes(s->buf + s->len, src, len);
    s->len += len;
    str_term(s);
}

static void str_open_gap(str_t *s, size_t idx, size_t len)
{
    assert(s != NULL);
    assert(s->buf != NULL);
    assert(idx <= s->len);
    assert(s->len < s->cap);
    assert(len <= s->cap - s->len - (size_t)STR_NUL_BYTES);

    size_t tail_len = (s->len - idx) + (size_t)STR_NUL_BYTES;
    str_move_bytes(s->buf + idx + len, s->buf + idx, tail_len);
}

static size_t str_shifted_idx(str_overlap_t overlap, size_t idx, size_t len)
{
    if (overlap.idx >= idx)
        return overlap.idx + len;
    return overlap.idx;
}

static void str_write_at(str_t *s, size_t idx, const char *src, size_t len)
{
    assert(s != NULL);
    assert(s->buf != NULL);
    assert(idx <= s->cap);
    assert(len <= s->cap - idx);
    str_move_bytes(s->buf + idx, src, len);
}

static void str_insert_apply(str_t *s, size_t idx, const char *src, size_t len)
{
    assert(s != NULL);
    assert(idx <= s->len);
    str_open_gap(s, idx, len);
    str_write_at(s, idx, src, len);
    s->len += len;
    str_term(s);
}

static void str_close_gap(str_t *s, size_t idx, size_t len)
{
    assert(s != NULL);
    assert(s->buf != NULL);
    assert(idx <= s->len);
    assert(len <= s->len - idx);

    size_t tail_len = (s->len - idx - len) + (size_t)STR_NUL_BYTES;
    str_move_bytes(s->buf + idx, s->buf + idx + len, tail_len);
    s->len -= len;
    str_term(s);
}

static void str_extend_fill(str_t *s, size_t len, char fill)
{
    assert(s != NULL);
    assert(s->buf != NULL);
    assert(s->len < len);
    assert(len < s->cap);
    memset(s->buf + s->len, (unsigned char)fill, len - s->len);
    s->len = len;
    str_term(s);
}

static str_status_t str_resize_grow(str_t *s, size_t len, char fill)
{
    assert(s != NULL);
    assert(len > s->len);

    size_t need = 0;
    str_status_t status = str_need_total(&need, 0, len);
    if (status != STR_OK)
        return status;
    status = str_ensure_cap(s, need);
    if (status != STR_OK)
        return status;
    str_extend_fill(s, len, fill);
    return STR_OK;
}

static str_status_t str_discard_heap(str_t *s)
{
    assert(s != NULL);
    assert(s->len == 0);
    assert(str_is_owned(s));
    str_heap_release(s->buf);
    str_reset_empty(s);
    return STR_OK;
}

static str_status_t str_shrink_heap(str_t *s, size_t exact_cap)
{
    assert(s != NULL);
    assert(s->buf != NULL);
    assert(exact_cap > 0);
    if (exact_cap == s->cap)
        return STR_OK;

    char *new_buf = str_heap_resize(s->buf, exact_cap);
    if (new_buf == NULL)
        return str_alloc_error();
    s->buf = new_buf;
    s->cap = exact_cap;
    return STR_OK;
}

static str_status_t str_stage_view(const str_t *s, str_staged_view_t *out, str_view_t src)
{
    assert(s != NULL);
    assert(out != NULL);
    out->view = src;
    out->owned = NULL;
    if (src.len == 0)
        return STR_OK;

    str_overlap_t overlap = {0};
    str_status_t status = str_probe_overlap(s, &overlap, src.ptr, src.len);
    if (status != STR_OK)
        return status;
    if (!overlap.inside)
        return STR_OK;

    char *owned = str_heap_alloc(src.len);
    if (owned == NULL)
        return str_alloc_error();
    str_move_bytes(owned, src.ptr, src.len);
    out->view.ptr = owned;
    out->owned = owned;
    return STR_OK;
}

static void str_release_staged(str_staged_view_t *staged)
{
    assert(staged != NULL);
    str_heap_release(staged->owned);
    staged->view = str_view_empty();
    staged->owned = NULL;
}

static void str_replace_apply(str_t *s, size_t idx, size_t remove_len, str_view_t replacement)
{
    assert(s != NULL);
    assert(str_view_is_valid(replacement));
    assert(idx <= s->len);
    assert(remove_len <= s->len - idx);

    size_t suffix_idx = idx + remove_len;
    size_t suffix_len = (s->len - suffix_idx) + (size_t)STR_NUL_BYTES;
    size_t new_len = (s->len - remove_len) + replacement.len;
    assert(new_len < s->cap);

    str_move_bytes(s->buf + idx + replacement.len, s->buf + suffix_idx, suffix_len);
    str_move_bytes(s->buf + idx, replacement.ptr, replacement.len);
    s->len = new_len;
    str_term(s);
}

static str_status_t str_replace_equal(str_t *s, size_t idx, str_view_t replacement)
{
    assert(s != NULL);
    assert(idx <= s->len);
    assert(replacement.len <= s->len - idx);

    str_overlap_t overlap = {0};
    STR_TRY(str_probe_overlap(s, &overlap, replacement.ptr, replacement.len));
    str_write_at(s, idx, replacement.ptr, replacement.len);
    return STR_OK;
}

static str_status_t str_replace_len(size_t *out_len, size_t old_len, size_t remove_len,
                                    size_t add_len)
{
    assert(out_len != NULL);
    assert(remove_len <= old_len);
    return str_size_add(out_len, old_len - remove_len, add_len);
}

static str_status_t str_replace_sized(str_t *s, str_replace_req_t request)
{
    assert(s != NULL);

    size_t need = 0;
    str_status_t status = str_need_total(&need, 0, request.new_len);
    if (status != STR_OK)
        return status;

    str_staged_view_t staged = {.view = request.replacement, .owned = NULL};
    status = str_stage_view(s, &staged, request.replacement);
    if (status != STR_OK)
        return status;

    status = str_ensure_cap(s, need);
    if (status != STR_OK) {
        str_release_staged(&staged);
        return status;
    }

    str_replace_apply(s, request.idx, request.remove_len, staged.view);
    str_release_staged(&staged);
    return STR_OK;
}

static str_status_t str_replace_commit(str_t *s, size_t idx, size_t remove_len,
                                       str_view_t replacement)
{
    assert(s != NULL);
    assert(idx <= s->len);
    assert(remove_len <= s->len - idx);
    if (remove_len == replacement.len)
        return str_replace_equal(s, idx, replacement);

    size_t new_len = 0;
    str_status_t status = str_replace_len(&new_len, s->len, remove_len, replacement.len);
    if (status != STR_OK)
        return status;

    str_replace_req_t request = {
        .idx = idx,
        .remove_len = remove_len,
        .new_len = new_len,
        .replacement = replacement,
    };
    return str_replace_sized(s, request);
}

static str_status_t str_fmt_write(size_t *out_len, str_fmt_dest_t dest, const char *format,
                                  va_list args)
{
    assert(out_len != NULL);
    assert(dest.buf != NULL);
    assert(dest.space > 0);
    assert(format != NULL);

    va_list copy;
    va_copy(copy, args);
    int printed = vsnprintf(dest.buf, dest.space, format, copy);
    va_end(copy);
    if (printed < 0)
        return str_fmt_error();
    return str_size_from_int(out_len, printed);
}

static str_status_t str_fmt_fill(str_fmt_dest_t dest, size_t expected_len, const char *format,
                                 va_list args)
{
    size_t rendered_len = 0;
    str_status_t status = str_fmt_write(&rendered_len, dest, format, args);

    if (status != STR_OK)
        return status;
    if (rendered_len != expected_len)
        return str_fmt_error();
    return STR_OK;
}

static str_status_t str_fmt_build(char **out_buf, size_t expected_len, const char *format,
                                  va_list args)
{
    assert(out_buf != NULL);
    assert(format != NULL);

    size_t space = 0;
    str_status_t status = str_need_total(&space, 0, expected_len);
    if (status != STR_OK)
        return status;

    char *buf = str_heap_alloc(space);
    if (buf == NULL)
        return str_alloc_error();

    str_fmt_dest_t dest = {.buf = buf, .space = space};
    status = str_fmt_fill(dest, expected_len, format, args);
    if (status != STR_OK) {
        str_heap_release(buf);
        return status;
    }

    *out_buf = buf;
    return STR_OK;
}

static str_status_t str_append_fmt_payload(str_t *s, str_fmt_payload_t payload, va_list args)
{
    assert(s != NULL);
    assert(payload.format != NULL);
    assert(payload.stack.buf != NULL);
    if (payload.rendered_len < payload.stack.space)
        return str_append_n(s, payload.stack.buf, payload.rendered_len);

    char *rendered = NULL;
    str_status_t status = str_fmt_build(&rendered, payload.rendered_len, payload.format, args);
    if (status != STR_OK)
        return str_fail(s, status);

    status = str_append_n(s, rendered, payload.rendered_len);
    str_heap_release(rendered);
    return status;
}

static str_view_t str_view_of(const str_t *s)
{
    assert(s != NULL);
    return (str_view_t){.ptr = str_cstr(s), .len = s->len};
}

static str_view_t str_view_empty(void)
{
    return (str_view_t){.ptr = str_empty_buf, .len = 0};
}

static str_status_t str_validate_find(const str_t *s, const size_t *out_idx, const char *needle)
{
    if (s == NULL || out_idx == NULL || needle == NULL)
        return str_arg_error();
    return STR_OK;
}

static str_status_t str_view_locate(str_view_t hay, size_t *out_idx, str_view_t needle,
                                    str_search_mode_t mode)
{
    if (out_idx == NULL)
        return str_arg_error();
    STR_TRY(str_validate_view(hay));
    STR_TRY(str_validate_view(needle));
    if (needle.len == 0) {
        *out_idx = (mode == STR_SEARCH_FIRST) ? 0 : hay.len;
        return STR_OK;
    }
    *out_idx = str_view_search_mode(hay, needle, mode);
    return STR_OK;
}

static bool str_byte_is_before(unsigned char left, unsigned char right, bool reverse_order)
{
    if (reverse_order)
        return left > right;
    return left < right;
}

static void str_suffix_walk_step(str_suffix_walk_t *walk, unsigned char candidate_byte,
                                 unsigned char suffix_byte, bool reverse_order)
{
    assert(walk != NULL);
    if (candidate_byte == suffix_byte) {
        if (walk->offset == walk->factor.period) {
            walk->candidate += walk->factor.period;
            walk->offset = 1;
            return;
        }
        walk->offset++;
        return;
    }
    if (str_byte_is_before(candidate_byte, suffix_byte, reverse_order)) {
        walk->candidate += walk->offset;
        walk->offset = 1;
        walk->factor.period = walk->candidate - walk->factor.cut + (size_t)STR_NUL_BYTES;
        return;
    }
    walk->candidate++;
    walk->factor.cut = walk->candidate;
    walk->offset = 1;
    walk->factor.period = 1;
}

static str_factor_t str_maximal_suffix(str_view_t needle, bool reverse_order)
{
    str_suffix_walk_t walk = {
        .factor = {.cut = 0, .period = 1},
        .candidate = 0,
        .offset = 1,
    };

    assert(needle.ptr != NULL);
    assert(needle.len > 0);
    while (walk.offset < needle.len - walk.candidate) {
        size_t suffix_idx = walk.factor.cut + walk.offset - (size_t)STR_NUL_BYTES;
        unsigned char suffix_byte = str_view_byte(needle, suffix_idx);
        unsigned char candidate_byte = str_view_byte(needle, walk.candidate + walk.offset);
        str_suffix_walk_step(&walk, candidate_byte, suffix_byte, reverse_order);
    }
    return walk.factor;
}

static str_factor_t str_critical_factor(str_view_t needle)
{
    str_factor_t forward = str_maximal_suffix(needle, false);
    str_factor_t reverse = str_maximal_suffix(needle, true);

    if (reverse.cut > forward.cut)
        return reverse;
    return forward;
}

static bool str_factor_is_periodic(str_view_t needle, str_factor_t factor)
{
    assert(needle.ptr != NULL);
    assert(factor.cut < needle.len);
    assert(factor.period > 0);
    if (factor.period > needle.len - factor.cut)
        return false;
    return str_bytes_equal(needle.ptr, needle.ptr + factor.period, factor.cut);
}

static size_t str_nonperiodic_shift(str_view_t needle, str_factor_t factor)
{
    assert(factor.cut > 0);
    assert(factor.cut < needle.len);

    size_t left_shift = factor.cut;
    size_t right_shift = needle.len - factor.cut + (size_t)STR_NUL_BYTES;
    if (left_shift > right_shift)
        return left_shift;
    return right_shift;
}

static size_t str_scan_right(str_view_t hay, str_view_t needle, str_factor_t factor,
                             str_search_state_t state)
{
    assert(hay.ptr != NULL);
    assert(needle.ptr != NULL);
    assert(hay.len >= needle.len);
    assert(state.idx <= hay.len - needle.len);
    assert(state.memory < needle.len);

    size_t scan = factor.cut;
    if (state.memory > scan)
        scan = state.memory;
    while (scan < needle.len && needle.ptr[scan] == hay.ptr[state.idx + scan])
        scan++;
    return scan;
}

static bool str_candidate_has_left_match(str_view_t hay, str_view_t needle, str_factor_t factor,
                                         str_search_state_t state)
{
    assert(hay.ptr != NULL);
    assert(needle.ptr != NULL);
    assert(hay.len >= needle.len);
    assert(state.idx <= hay.len - needle.len);
    assert(state.memory < needle.len);

    size_t scan = factor.cut;
    while (scan > state.memory) {
        size_t idx = scan - (size_t)STR_NUL_BYTES;
        if (needle.ptr[idx] != hay.ptr[state.idx + idx])
            return false;
        scan--;
    }
    return true;
}

static bool str_search_advance(str_search_state_t *state, size_t last_idx, size_t shift)
{
    assert(state != NULL);
    assert(state->idx <= last_idx);
    assert(shift > 0);
    if (shift > last_idx - state->idx)
        return false;
    state->idx += shift;
    return true;
}

static size_t str_view_search_mode(str_view_t hay, str_view_t needle, str_search_mode_t mode)
{
    assert(needle.ptr != NULL);
    assert(needle.len > 0);
    assert(mode == STR_SEARCH_FIRST || mode == STR_SEARCH_LAST);
    if (hay.ptr == NULL || hay.len < needle.len)
        return STR_NPOS;
    if (needle.len == (size_t)STR_NUL_BYTES)
        return str_view_search_byte(hay, (unsigned char)needle.ptr[0], mode);

    str_search_ctx_t ctx = str_search_context(hay, needle, mode);
    str_search_state_t state = {.idx = 0, .memory = 0};
    size_t found = STR_NPOS;

    for (size_t step = 0; step <= ctx.last_idx; step++) {
        assert(state.idx <= ctx.last_idx);
        if (!str_search_step(&ctx, &state, &found))
            return found;
    }
    return found;
}

static str_search_ctx_t str_search_context(str_view_t hay, str_view_t needle,
                                           str_search_mode_t mode)
{
    str_factor_t factor = str_critical_factor(needle);
    bool periodic = str_factor_is_periodic(needle, factor);

    return (str_search_ctx_t){
        .hay = hay,
        .needle = needle,
        .factor = factor,
        .full_shift = periodic ? factor.period : str_nonperiodic_shift(needle, factor),
        .saved_memory = periodic ? needle.len - factor.period : 0,
        .last_idx = hay.len - needle.len,
        .mode = mode,
    };
}

static bool str_search_on_mismatch(const str_search_ctx_t *ctx, str_search_state_t *state,
                                   size_t mismatch)
{
    assert(ctx != NULL);
    assert(state != NULL);
    assert(mismatch < ctx->needle.len);

    size_t shift = mismatch - ctx->factor.cut + (size_t)STR_NUL_BYTES;
    bool has_next = str_search_advance(state, ctx->last_idx, shift);
    state->memory = 0;
    return has_next;
}

static bool str_search_after_right_match(const str_search_ctx_t *ctx, str_search_state_t *state,
                                         size_t *found)
{
    assert(ctx != NULL);
    assert(state != NULL);
    assert(found != NULL);
    if (str_candidate_has_left_match(ctx->hay, ctx->needle, ctx->factor, *state)) {
        *found = state->idx;
        if (ctx->mode == STR_SEARCH_FIRST)
            return false;
    }

    bool has_next = str_search_advance(state, ctx->last_idx, ctx->full_shift);
    state->memory = ctx->saved_memory;
    return has_next;
}

static bool str_search_step(const str_search_ctx_t *ctx, str_search_state_t *state, size_t *found)
{
    assert(ctx != NULL);
    assert(state != NULL);
    assert(found != NULL);

    size_t mismatch = str_scan_right(ctx->hay, ctx->needle, ctx->factor, *state);
    if (mismatch < ctx->needle.len)
        return str_search_on_mismatch(ctx, state, mismatch);
    return str_search_after_right_match(ctx, state, found);
}

static size_t str_view_find_byte(str_view_t hay, unsigned char byte)
{
    assert(hay.ptr != NULL);
    assert(hay.len > 0);

    const char *match = memchr(hay.ptr, (int)byte, hay.len);
    if (match == NULL)
        return STR_NPOS;
    return (size_t)(match - hay.ptr);
}

static size_t str_view_rfind_byte(str_view_t hay, unsigned char byte)
{
    assert(hay.ptr != NULL);
    assert(hay.len > 0);

    size_t idx = hay.len;
    while (idx > 0) {
        idx--;
        if ((unsigned char)hay.ptr[idx] == byte)
            return idx;
    }
    return STR_NPOS;
}

static size_t str_view_search_byte(str_view_t hay, unsigned char byte, str_search_mode_t mode)
{
    assert(hay.ptr != NULL);
    assert(hay.len > 0);
    if (mode == STR_SEARCH_FIRST)
        return str_view_find_byte(hay, byte);
    return str_view_rfind_byte(hay, byte);
}

static bool str_is_ascii_whitespace(char c)
{
    switch (c) {
    case ' ':
    case '\t':
    case '\n':
    case '\r':
    case '\f':
    case '\v':
        return true;
    default:
        return false;
    }
}

static str_view_t str_view_trim_left(str_view_t view)
{
    while (view.len > 0 && str_is_ascii_whitespace(view.ptr[0])) {
        view.ptr++;
        view.len--;
    }
    return view;
}

static str_view_t str_view_trim_right(str_view_t view)
{
    while (view.len > 0 && str_is_ascii_whitespace(view.ptr[view.len - 1]))
        view.len--;
    return view;
}

static str_status_t str_validate_split(str_view_t src, const str_split_out_t *out)
{
    if (out == NULL)
        return str_arg_error();
    if (out->cap > 0 && out->parts == NULL)
        return str_arg_error();
    return str_validate_view(src);
}

static void str_split_put(str_split_out_t *out, size_t count, const char *ptr, size_t len)
{
    assert(out != NULL);
    if (count >= out->cap)
        return;
    assert(out->parts != NULL);

    str_view_t *part = &out->parts[count];
    part->ptr = ptr;
    part->len = len;
}

static size_t str_find_separator(str_view_t src, size_t start, char separator)
{
    const char *ptr = str_view_bytes(src);

    assert(start <= src.len);
    for (size_t idx = start; idx < src.len; idx++) {
        if (ptr[idx] == separator)
            return idx;
    }
    return STR_NPOS;
}

static str_status_t str_split_count(size_t *out_count, str_view_t src, char separator)
{
    size_t count = 1;
    size_t idx = 0;

    assert(out_count != NULL);
    assert(str_view_is_valid(src));
    while (idx < src.len) {
        size_t sep = str_find_separator(src, idx, separator);
        if (sep == STR_NPOS)
            break;
        STR_TRY(str_size_add(&count, count, 1));
        idx = sep + 1;
    }
    *out_count = count;
    return STR_OK;
}

static void str_split_fill(str_split_out_t *out, str_view_t src, char separator)
{
    const char *ptr = str_view_bytes(src);
    size_t start = 0;
    size_t part_idx = 0;
    size_t idx = 0;

    assert(out != NULL);
    while (idx < src.len) {
        size_t sep = str_find_separator(src, idx, separator);
        if (sep == STR_NPOS)
            break;
        str_split_put(out, part_idx, ptr + start, sep - start);
        part_idx++;
        start = sep + 1;
        idx = start;
    }
    str_split_put(out, part_idx, ptr + start, src.len - start);
}
