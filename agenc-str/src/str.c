/* str.c: owns growable byte strings, including their heap buffers. */

#include "str.h"

#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    STR_MIN_CAPACITY_BYTES = 16,
    STR_CAPACITY_GROWTH_FACTOR = 2,
    STR_NUL_BYTES = 1,
    STR_FORMAT_STACK_BYTES = 256,
    STR_MAX_CAPACITY_GROWTH_STEPS = sizeof(size_t) * (size_t)CHAR_BIT,
    STR_SUFFIX_WALK_PASSES = 2
};

/* Propagates non-OK status. Permitted only in functions that acquire nothing. */
#define STR_TRY(expr)                                                                              \
    do {                                                                                           \
        str_status_t str_try_status_ = (expr);                                                     \
        if (str_try_status_ != STR_OK) {                                                           \
            return str_try_status_;                                                                \
        }                                                                                          \
    } while (0)

/* idx is meaningful only when is_inside is true. */
typedef struct {
    bool is_inside;
    size_t idx;
} str_overlap_t;

/* source is readable for len bytes. base_len is the destination length before the write. */
typedef struct {
    const char *source;
    size_t len;
    size_t base_len;
} str_write_request_t;

/* The source is readable for len bytes. idx lies in the destination content. */
typedef struct {
    size_t idx;
    const char *source;
    size_t len;
} str_insert_request_t;

/* owned_buf is NULL unless view points to storage released by str_release_staged_view. */
typedef struct {
    str_view_t view;
    char *owned_buf;
} str_staged_view_t;

/* The span lies in the destination. new_len is the fully validated result length. */
typedef struct {
    size_t idx;
    size_t remove_len;
    size_t new_len;
    str_view_t replacement;
} str_replace_request_t;

/* buf is non-NULL. capacity includes room for the terminating NUL. */
typedef struct {
    char *buf;
    size_t capacity;
} str_format_destination_t;

/* stack_destination owns no storage. format stays valid until rendering completes. */
typedef struct {
    str_format_destination_t stack_destination;
    size_t rendered_len;
    const char *format;
} str_format_payload_t;

typedef enum { STR_SEARCH_FIRST, STR_SEARCH_LAST } str_search_mode_t;

typedef enum {
    STR_COMPARE_LESS = -1,
    STR_COMPARE_EQUAL = 0,
    STR_COMPARE_GREATER = 1
} str_compare_order_t;

/* cut partitions the needle. period is always positive. */
typedef struct {
    size_t cut;
    size_t period;
} str_factor_t;

/* idx is the current candidate. memory is the known matching prefix length. */
typedef struct {
    size_t idx;
    size_t memory;
} str_search_state_t;

/* Views are valid borrowed inputs. Needle fits haystack. Shifts are positive. */
typedef struct {
    str_view_t haystack;
    str_view_t needle;
    str_factor_t factor;
    size_t full_shift;
    size_t saved_memory;
    size_t last_idx;
    str_search_mode_t mode;
} str_search_context_t;

/* candidate stays inside needle. offset plus candidate never exceeds needle length. */
typedef struct {
    str_factor_t factor;
    size_t candidate;
    size_t offset;
} str_suffix_walk_t;

/* Keeps byte search mode adjacent to its byte so callers cannot swap scalar arguments. */
typedef struct {
    unsigned char byte;
    str_search_mode_t mode;
} str_byte_search_t;

/* Keeps the split cursor adjacent to its separator. */
typedef struct {
    size_t idx;
    char separator;
} str_split_cursor_t;

/* offset begins in the string allocation. len is positive. */
typedef struct {
    size_t offset;
    size_t len;
} str_internal_span_t;

/* name points to static storage for status. */
typedef struct {
    str_status_t status;
    const char *name;
} str_status_name_t;

#ifdef STR_TEST
/* Deviation: test-only mutable state injects deterministic allocation failure. */
static bool str_test_alloc_failures_enabled;
static size_t str_test_alloc_budget;
#endif

static const char str_empty_buf[STR_NUL_BYTES] = {'\0'};

static const unsigned char str_ascii_whitespace_bytes[] = {0x20U, 0x09U, 0x0aU,
                                                           0x0dU, 0x0cU, 0x0bU};

static const str_status_name_t str_status_names[] = {
    {.status = STR_OK, .name = "STR_OK"},
    {.status = STR_ERR_ARG, .name = "STR_ERR_ARG"},
    {.status = STR_ERR_ALLOC, .name = "STR_ERR_ALLOC"},
    {.status = STR_ERR_RANGE, .name = "STR_ERR_RANGE"},
    {.status = STR_ERR_OVERFLOW, .name = "STR_ERR_OVERFLOW"},
    {.status = STR_ERR_FMT, .name = "STR_ERR_FMT"},
};

/* Returns STR_ERR_ARG. Sole producer of that status. */
static str_status_t str_argument_error(void);

/* Returns STR_ERR_RANGE. Sole producer of that status. */
static str_status_t str_range_error(void);

/* Returns STR_ERR_ALLOC. Sole producer of that status. */
static str_status_t str_allocation_error(void);

/* Returns STR_ERR_OVERFLOW. Sole producer of that status. */
static str_status_t str_overflow_error(void);

/* Returns STR_ERR_FMT. Sole producer of that status. */
static str_status_t str_format_error(void);

/* Resets non-NULL string to canonical empty state. Existing storage is not released. */
static void str_reset_state(str_t *string);

/* Transfers state from non-NULL source to non-NULL destination. Source becomes empty. */
static void str_transfer_state(str_t *destination, str_t *source);

/* True when a non-NULL string owns a heap buffer. */
static bool str_is_owned(const str_t *string);

/* Sets non-NULL string status to STR_OK. */
static void str_clear_failure(str_t *string);

/* Empties non-NULL string. Its allocation remains owned. */
static void str_clear_content(str_t *string);

#ifdef STR_TEST
/* Consumes one test allocation allowance. Returns true at the injected failure point. */
static bool str_test_consume_allocation_budget(void);

/* Claims one heap attempt. Returns false only when test injection rejects it. */
static bool str_claim_heap_attempt(void);
#endif

/* Writes a newly owned malloc block to non-NULL out_buf on success.
 * STR_ERR_ALLOC leaves out_buf unchanged. */
static str_status_t str_call_malloc(char **out_buf, size_t bytes);

/* Writes resized non-NULL buf to non-NULL out_buf on success.
 * STR_ERR_ALLOC leaves buf owned plus out_buf unchanged. */
static str_status_t str_call_realloc(char **out_buf, char *buf, size_t bytes);

/* Writes a newly owned heap block to non-NULL out_buf on success.
 * STR_ERR_ALLOC leaves out_buf unchanged. */
static str_status_t str_allocate_heap(char **out_buf, size_t bytes);

/* Writes resized non-NULL owned buf to non-NULL out_buf on success.
 * STR_ERR_ALLOC leaves buf owned plus out_buf unchanged. */
static str_status_t str_resize_heap(char **out_buf, char *buf, size_t bytes);

/* Releases owned buf through free. A NULL buf is permitted. */
static void str_release_heap(char *buf);

/* Writes owned expanded storage to non-NULL out_buf on success.
 * NULL buf requires has_owned_buffer false.
 * STR_ERR_ALLOC preserves buf ownership. out_buf remains unchanged. */
static str_status_t str_expand_heap(char **out_buf, char *buf, size_t bytes, bool has_owned_buffer);

/* Stores non-OK status on non-NULL string. */
static void str_set_failure(str_t *string, str_status_t status);

/* Returns the owned buffer from non-NULL string. String becomes empty. */
static char *str_take_owned_buffer(str_t *string);

/* Writes a newly owned empty C-string buffer to non-NULL out_buf on success.
 * STR_ERR_ALLOC leaves out_buf unchanged. */
static str_status_t str_allocate_empty_buffer(char **out_buf);

/* Accepts possibly NULL string. Fails with STR_ERR_ARG or its sticky status. */
static str_status_t str_validate_mutation(const str_t *string);

/* Accepts NULL source only for zero len. Fails with STR_ERR_ARG. */
static str_status_t str_validate_source(const char *source, size_t len);

/* Validates a borrowed view. Fails with STR_ERR_ARG. */
static str_status_t str_validate_view(str_view_t view);

/* True when both borrowed views are structurally valid. */
static bool str_has_valid_views(str_view_t left, str_view_t right);

/* Accepts possibly NULL string or source. Fails with sticky status or recorded STR_ERR_ARG. */
static str_status_t str_begin_mutation(str_t *string, const char *source, size_t len);

/* Validates a span inside [0, len]. Fails with STR_ERR_RANGE. */
static str_status_t str_validate_span(size_t len, size_t idx, size_t span);

/* Measures non-NULL source within available bytes into non-NULL out_len. Fails with STR_ERR_ARG. */
static str_status_t str_measure_bounded_c_string(size_t *out_len, const char *source,
                                                 size_t available);

/* Measures possibly NULL source into non-NULL out_len for possibly NULL string.
 * Fails with sticky status or STR_ERR_ARG. */
static str_status_t str_measure_c_string_input(str_t *string, size_t *out_len, const char *source);

/* Measures non-NULL source into non-NULL out_len for non-NULL string.
 * Invalid self-source records STR_ERR_ARG. */
static str_status_t str_measure_valid_c_string_input(str_t *string, size_t *out_len,
                                                     const char *source);

/* Measures internal C string at offset into non-NULL out_len. Records STR_ERR_ARG. */
static str_status_t str_measure_internal_c_string(str_t *string, size_t *out_len, size_t offset);

/* Records non-OK status on non-NULL string without changing its content. */
static str_status_t str_record_failure(str_t *string, str_status_t status);

/* Writes base plus increment to non-NULL out_sum. Fails with STR_ERR_OVERFLOW. */
static str_status_t str_add_size(size_t *out_sum, size_t base, size_t increment);

/* Converts a printf result into non-NULL out_len. Fails with STR_ERR_FMT or STR_ERR_OVERFLOW. */
static str_status_t str_convert_printed_length(size_t *out_len, int value);

/* Writes len plus additional_len plus NUL to out_need. Fails with STR_ERR_OVERFLOW. */
static str_status_t str_calculate_capacity(size_t *out_need, size_t len, size_t additional_len);

/* Returns the length of non-NULL borrowed source through strlen. */
static size_t str_measure_c_string(const char *source);

/* True when both readable operands contain equal len-byte spans. Zero len permits NULL. */
static bool str_is_byte_span_equal(const char *left, const char *right, size_t len);

/* Returns unsigned-byte order for readable len-byte spans. Zero len permits NULL. */
static str_compare_order_t str_compare_bytes(const char *left, const char *right, size_t len);

/* Returns lexical order from two lengths with equal prefixes. */
static str_compare_order_t str_compare_lengths(size_t left_len, size_t right_len);

/* Returns unsigned-byte lexical order for two valid views. */
static str_compare_order_t str_compare_valid_views(str_view_t left, str_view_t right);

/* True when two valid views contain equal bytes. */
static bool str_is_view_content_equal(str_view_t left, str_view_t right);

/* True when valid value begins with valid prefix. */
static bool str_has_view_prefix(str_view_t value, str_view_t prefix);

/* True when valid value ends with valid suffix. */
static bool str_has_view_suffix(str_view_t value, str_view_t suffix);

/* Moves len bytes from source to destination through memmove. Zero len permits NULL. */
static void str_move_bytes(char *destination, const char *source, size_t len);

/* Fills non-NULL destination with len copies of byte through memset. */
static void str_fill_bytes(char *destination, size_t len, unsigned char byte);

/* Returns the unsigned byte at idx in a valid nonempty view. */
static unsigned char str_read_view_byte(str_view_t view, size_t idx);

/* Returns borrowed readable storage for valid view. Empty NULL view maps to static storage. */
static const char *str_get_view_buffer(str_view_t view);

/* True when doubling cap would overflow size_t. */
static bool str_has_capacity_doubling_overflow(size_t cap);

/* Writes capacity covering need to non-NULL out_cap. Fails with STR_ERR_OVERFLOW. */
static str_status_t str_grow_capacity(size_t *out_cap, size_t cap, size_t need);

/* Writes geometric capacity covering need to non-NULL out_cap. Fails with STR_ERR_OVERFLOW. */
static str_status_t str_choose_capacity(size_t *out_cap, size_t current_cap, size_t need);

/* Grows non-NULL string storage to new_cap. Fails with STR_ERR_ALLOC. */
static str_status_t str_grow_heap(str_t *string, size_t new_cap);

/* Ensures non-NULL string has need allocated bytes.
 * Fails with STR_ERR_ALLOC or STR_ERR_OVERFLOW. */
static str_status_t str_ensure_capacity(str_t *string, size_t need);

/* Locates non-NULL source within non-NULL string's allocation. */
static size_t str_find_allocation_offset(const str_t *string, const char *source);

/* Validates a nonempty source span beginning at offset in string allocation.
 * Fails with STR_ERR_ARG. */
static str_status_t str_validate_internal_source_span(const str_t *string,
                                                      str_internal_span_t span);

/* Classifies nonempty source in string into out_overlap.
 * An invalid self-span fails with STR_ERR_ARG. */
static str_status_t str_detect_overlap(const str_t *string, str_overlap_t *out_overlap,
                                       const char *source, size_t len);

/* Prepares storage for non-NULL request and rebases an internal source after growth.
 * Fails with STR_ERR_ARG, STR_ERR_ALLOC, or STR_ERR_OVERFLOW. */
static str_status_t str_prepare_insert(str_t *string, str_insert_request_t *request);

/* Prepares non-NULL string for request. Writes a borrowed source to non-NULL out_source.
 * Fails with STR_ERR_ARG, STR_ERR_ALLOC, or STR_ERR_OVERFLOW. */
static str_status_t str_prepare_write(str_t *string, const char **out_source,
                                      str_write_request_t request);

/* Prepares request from a validated boundary. Writes borrowed source to out_source.
 * Records STR_ERR_ARG, STR_ERR_ALLOC, or STR_ERR_OVERFLOW. */
static str_status_t str_prepare_valid_span(str_t *string, const char **out_source,
                                           str_write_request_t request);

/* Replaces non-NULL string content with a previously validated source.
 * Records STR_ERR_ARG, STR_ERR_ALLOC, or STR_ERR_OVERFLOW. */
static str_status_t str_set_valid_span(str_t *string, const char *source, size_t len);

/* Appends a previously validated source to non-NULL string.
 * Records STR_ERR_ARG, STR_ERR_ALLOC, or STR_ERR_OVERFLOW. */
static str_status_t str_append_valid_span(str_t *string, const char *source, size_t len);

/* Appends a readable source known not to alias string.
 * Records STR_ERR_ALLOC or STR_ERR_OVERFLOW. */
static str_status_t str_append_unaliased_span(str_t *string, const char *source, size_t len);

/* Writes the terminator for a non-NULL allocated string. */
static void str_write_terminator(str_t *string);

/* Replaces non-NULL string content from a readable len-byte source. */
static void str_replace_content(str_t *string, const char *source, size_t len);

/* Appends a readable len-byte source to a non-NULL string with sufficient capacity. */
static void str_append_content(str_t *string, const char *source, size_t len);

/* Returns overlap source offset after opening an insertion gap. */
static size_t str_adjust_source_offset(str_overlap_t overlap, const str_insert_request_t *request);

/* Writes readable source at idx in non-NULL string without changing its length. */
static void str_write_content(str_t *string, size_t idx, const char *source, size_t len);

/* Commits request to a prepared non-NULL string. */
static void str_commit_insert(str_t *string, str_insert_request_t request);

/* Removes len bytes at idx from a non-NULL allocated string. */
static void str_close_gap(str_t *string, size_t idx, size_t len);

/* Extends non-NULL string to len with fill using existing capacity. */
static void str_extend_content(str_t *string, size_t len, char fill);

/* Grows non-NULL string to len using fill. Fails with STR_ERR_ALLOC or STR_ERR_OVERFLOW. */
static str_status_t str_grow_content(str_t *string, size_t len, char fill);

/* Releases the buffer of a non-NULL owned empty string. */
static void str_discard_heap(str_t *string);

/* Shrinks non-NULL owned string storage to exact_cap. Fails with STR_ERR_ALLOC. */
static str_status_t str_shrink_heap(str_t *string, size_t exact_cap);

/* Stages source borrowing from non-NULL string into non-NULL out_staged.
 * Fails with STR_ERR_ARG or STR_ERR_ALLOC. */
static str_status_t str_stage_replacement(const str_t *string, str_staged_view_t *out_staged,
                                          str_view_t source);

/* Releases storage owned by non-NULL staged. Staged becomes an empty borrowed view. */
static void str_release_staged_view(str_staged_view_t *staged);

/* Commits a validated replacement to non-NULL string using existing capacity. */
static void str_commit_replacement(str_t *string, size_t idx, size_t remove_len,
                                   str_view_t replacement);

/* Replaces an equal span in non-NULL string. Fails with STR_ERR_ARG for invalid self-source. */
static str_status_t str_replace_equal_span(str_t *string, size_t idx, str_view_t replacement);

/* Writes replacement length to non-NULL out_len. Fails with STR_ERR_OVERFLOW. */
static str_status_t str_calculate_replacement_length(size_t *out_len, size_t old_len,
                                                     size_t remove_len, size_t added_len);

/* Applies size-changing request to non-NULL string.
 * Propagates STR_ERR_ARG, STR_ERR_ALLOC, or STR_ERR_OVERFLOW. */
static str_status_t str_replace_resized_span(str_t *string, str_replace_request_t request);

/* Applies a validated replacement to non-NULL string.
 * Propagates STR_ERR_ARG, STR_ERR_ALLOC, or STR_ERR_OVERFLOW. */
static str_status_t str_apply_replacement(str_t *string, size_t idx, size_t remove_len,
                                          str_view_t replacement);

/* Renders non-NULL format into destination. Writes non-NULL out_len.
 * Fails with STR_ERR_FMT or STR_ERR_OVERFLOW. */
static str_status_t str_render_format(size_t *out_len, str_format_destination_t destination,
                                      const char *format, va_list arguments);

/* Renders non-NULL format into destination.
 * Fails with STR_ERR_FMT or STR_ERR_OVERFLOW. */
static str_status_t str_verify_format_render(str_format_destination_t destination,
                                             size_t expected_len, const char *format,
                                             va_list arguments);

/* Writes newly owned storage from non-NULL format to non-NULL out_buf.
 * Fails with STR_ERR_FMT, STR_ERR_ALLOC, or STR_ERR_OVERFLOW. */
static str_status_t str_build_formatted_buffer(char **out_buf, size_t expected_len,
                                               const char *format, va_list arguments);

/* Appends payload to non-NULL string.
 * Records STR_ERR_FMT, STR_ERR_ALLOC, or STR_ERR_OVERFLOW. */
static str_status_t str_append_format_payload(str_t *string, str_format_payload_t payload,
                                              va_list arguments);

/* Returns a borrowed view over non-NULL string. */
static str_view_t str_make_view(const str_t *string);

/* Returns the canonical empty borrowed view. */
static str_view_t str_make_empty_view(void);

/* Accepts possibly NULL string, out_idx, or needle. Fails with STR_ERR_ARG. */
static str_status_t str_validate_find_arguments(const str_t *string, const size_t *out_idx,
                                                const char *needle);

/* Writes the requested match to non-NULL out_idx. Fails with STR_ERR_ARG or STR_ERR_OVERFLOW. */
static str_status_t str_find_view_match(size_t *out_idx, str_view_t haystack, str_view_t needle,
                                        str_search_mode_t mode);

/* Returns the requested match for valid views. */
static size_t str_locate_view_match(str_view_t haystack, str_view_t needle, str_search_mode_t mode);

/* True when left precedes right under the selected byte order. */
static bool str_is_byte_before(unsigned char left, unsigned char right, bool is_reverse_order);

/* Advances non-NULL walk for one compared byte pair. */
static void str_suffix_walk_step(str_suffix_walk_t *walk, unsigned char candidate_byte,
                                 unsigned char suffix_byte, bool is_reverse_order);

/* Runs at most needle.len factorization steps. Returns true when non-NULL walk is complete. */
static bool str_run_suffix_walk_pass(str_suffix_walk_t *walk, str_view_t needle,
                                     bool is_reverse_order);

/* Returns ordered maximal-suffix factorization for a valid nonempty needle. */
static str_factor_t str_find_maximal_suffix(str_view_t needle, bool is_reverse_order);

/* Returns the critical factorization across both byte orders. */
static str_factor_t str_find_critical_factor(str_view_t needle);

/* True when the candidate period covers the full needle. */
static bool str_is_factor_periodic(str_view_t needle, str_factor_t factor);

/* Returns the full-window shift for a nonperiodic needle. */
static size_t str_calculate_nonperiodic_shift(str_view_t needle, str_factor_t factor);

/* Returns the first right-half mismatch for a valid search candidate. */
static size_t str_scan_right(str_view_t haystack, str_view_t needle, str_factor_t factor,
                             str_search_state_t state);

/* True when the unchecked left half matches. */
static bool str_has_candidate_left_match(str_view_t haystack, str_view_t needle,
                                         str_factor_t factor, str_search_state_t state);

/* Advances non-NULL state within last_idx. False means the scan is complete. */
static bool str_search_advance(str_search_state_t *state, size_t last_idx, size_t shift);

/* Returns the requested occurrence of valid nonempty needle in haystack. */
static size_t str_search_view(str_view_t haystack, str_view_t needle, str_search_mode_t mode);

/* Runs a Two-Way search for a multi-byte needle that fits in nonempty haystack. */
static size_t str_search_two_way(str_view_t haystack, str_view_t needle, str_search_mode_t mode);

/* Returns immutable state for one valid Two-Way search. */
static str_search_context_t str_build_search_context(str_view_t haystack, str_view_t needle,
                                                     str_search_mode_t mode);

/* Shifts non-NULL state past mismatch using non-NULL context. Returns false at final candidate. */
static bool str_search_on_mismatch(const str_search_context_t *context, str_search_state_t *state,
                                   size_t mismatch);

/* Processes a right-half match using non-NULL pointers. Returns false when search is complete. */
static bool str_search_after_right_match(const str_search_context_t *context,
                                         str_search_state_t *state, size_t *found);

/* Examines one candidate using non-NULL pointers. Returns false when search is complete. */
static bool str_search_step(const str_search_context_t *context, str_search_state_t *state,
                            size_t *found);

/* First index of byte, or STR_NPOS. */
static size_t str_view_find_byte(str_view_t haystack, unsigned char byte);

/* Last index of byte, or STR_NPOS. */
static size_t str_view_rfind_byte(str_view_t haystack, unsigned char byte);

/* Returns the requested byte occurrence from valid nonempty haystack. */
static size_t str_search_view_byte(str_view_t haystack, str_byte_search_t search);

/* True for ASCII space, tab, CR, LF, FF, VT byte values. */
static bool str_is_ascii_whitespace(unsigned char byte);

/* Drops leading ASCII whitespace. */
static str_view_t str_trim_view_left(str_view_t view);

/* Drops trailing ASCII whitespace. */
static str_view_t str_trim_view_right(str_view_t view);

/* Validates source plus possibly NULL split_output. Fails with STR_ERR_ARG. */
static str_status_t str_validate_split_arguments(str_view_t source,
                                                 const str_split_out_t *split_output);

/* Stores one borrowed span in non-NULL split_output when count is below capacity. */
static void str_store_split_part(str_split_out_t *split_output, size_t count, const char *source,
                                 size_t len);

/* Returns the next separator index from cursor in valid source, or STR_NPOS. */
static size_t str_find_separator(str_view_t source, str_split_cursor_t cursor);

/* Writes borrowed parts plus full count into non-NULL split_output.
 * Fails with STR_ERR_OVERFLOW without changing split_output. */
static str_status_t str_write_split_parts(str_split_out_t *split_output, str_view_t source,
                                          char separator);

void str_init(str_t *string)
{
    if (string == NULL) {
        return;
    }
    str_reset_state(string);
}

void str_deinit(str_t *string)
{
    if (string == NULL) {
        return;
    }
    if (str_is_owned(string)) {
        str_release_heap(string->buf);
    }
    str_reset_state(string);
}

void str_clear(str_t *string)
{
    if (string == NULL) {
        return;
    }
    str_clear_failure(string);
    str_clear_content(string);
}

void str_clear_error(str_t *string)
{
    if (string == NULL) {
        return;
    }
    str_clear_failure(string);
}

void str_move(str_t *destination, str_t *source)
{
    if (destination == NULL || source == NULL) {
        return;
    }
    if (destination == source) {
        return;
    }
    str_deinit(destination);
    str_transfer_state(destination, source);
}

str_status_t str_copy(str_t *destination, const str_t *source)
{
    if (destination == NULL) {
        return str_argument_error();
    }

    str_status_t status = str_validate_mutation(destination);
    if (status != STR_OK) {
        return status;
    }
    if (source == NULL) {
        status = str_argument_error();
        return str_record_failure(destination, status);
    }
    if (destination == source) {
        return STR_OK;
    }

    str_t scratch = STR_EMPTY;
    const char *source_buf = str_cstr(source);
    size_t len = str_len(source);
    status = str_set_valid_span(&scratch, source_buf, len);
    if (status != STR_OK) {
        str_deinit(&scratch);
        return str_record_failure(destination, status);
    }
    str_move(destination, &scratch);
    return STR_OK;
}

char *str_detach(str_t *string)
{
    if (string == NULL) {
        return NULL;
    }
    if (str_is_owned(string)) {
        return str_take_owned_buffer(string);
    }

    char *owned_buf = NULL;
    str_status_t status = str_allocate_empty_buffer(&owned_buf);
    if (status != STR_OK) {
        str_set_failure(string, status);
        return NULL;
    }
    str_reset_state(string);
    return owned_buf;
}

const char *str_status_name(str_status_t status)
{
    size_t name_count = sizeof(str_status_names) / sizeof(str_status_names[0]);
    for (size_t idx = 0; idx < name_count; idx++) {
        if (str_status_names[idx].status == status) {
            return str_status_names[idx].name;
        }
    }
    return "STR_ERR_UNKNOWN";
}

str_status_t str_set(str_t *string, const char *source)
{
    size_t len = 0;
    str_status_t status = str_measure_c_string_input(string, &len, source);

    if (status != STR_OK) {
        return status;
    }
    return str_set_valid_span(string, source, len);
}

str_status_t str_set_n(str_t *string, const char *source, size_t len)
{
    str_status_t status = str_begin_mutation(string, source, len);

    if (status != STR_OK) {
        return status;
    }
    return str_set_valid_span(string, source, len);
}

str_status_t str_set_view(str_t *string, str_view_t source)
{
    return str_set_n(string, source.ptr, source.len);
}

str_status_t str_append(str_t *string, const char *source)
{
    size_t len = 0;
    str_status_t status = str_measure_c_string_input(string, &len, source);

    if (status != STR_OK) {
        return status;
    }
    return str_append_valid_span(string, source, len);
}

str_status_t str_append_n(str_t *string, const char *source, size_t len)
{
    str_status_t status = str_begin_mutation(string, source, len);

    if (status != STR_OK) {
        return status;
    }
    return str_append_valid_span(string, source, len);
}

str_status_t str_append_view(str_t *string, str_view_t source)
{
    return str_append_n(string, source.ptr, source.len);
}

str_status_t str_append_char(str_t *string, char byte)
{
    str_status_t status = str_validate_mutation(string);
    if (status != STR_OK) {
        return status;
    }

    char source[] = {byte};
    return str_append_unaliased_span(string, source, sizeof(source));
}

str_status_t str_append_vfmt(str_t *string, const char *format, va_list arguments)
{
    size_t format_len = 0;
    str_status_t status = str_measure_c_string_input(string, &format_len, format);

    if (status != STR_OK) {
        return status;
    }

    char stack_buf[STR_FORMAT_STACK_BYTES];
    size_t rendered_len = 0;
    str_format_destination_t stack_destination = {
        .buf = stack_buf,
        .capacity = sizeof(stack_buf),
    };
    status = str_render_format(&rendered_len, stack_destination, format, arguments);
    if (status != STR_OK) {
        return str_record_failure(string, status);
    }

    str_format_payload_t payload = {
        .stack_destination = stack_destination,
        .rendered_len = rendered_len,
        .format = format,
    };
    return str_append_format_payload(string, payload, arguments);
}

str_status_t str_append_fmt(str_t *string, const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    str_status_t status = str_append_vfmt(string, format, arguments);
    va_end(arguments);
    return status;
}

str_status_t str_reserve(str_t *string, size_t len)
{
    str_status_t status = str_validate_mutation(string);

    if (status != STR_OK) {
        return status;
    }
    if (len == 0) {
        return STR_OK;
    }

    size_t need = 0;
    status = str_calculate_capacity(&need, 0, len);
    if (status != STR_OK) {
        return str_record_failure(string, status);
    }
    status = str_ensure_capacity(string, need);
    if (status != STR_OK) {
        return str_record_failure(string, status);
    }
    return STR_OK;
}

str_status_t str_shrink_to_fit(str_t *string)
{
    str_status_t status = str_validate_mutation(string);

    if (status != STR_OK) {
        return status;
    }
    if (string->cap == 0) {
        return STR_OK;
    }
    if (string->len == 0) {
        str_discard_heap(string);
        return STR_OK;
    }

    size_t exact_cap = 0;
    status = str_add_size(&exact_cap, string->len, (size_t)STR_NUL_BYTES);
    if (status != STR_OK) {
        return str_record_failure(string, status);
    }
    status = str_shrink_heap(string, exact_cap);
    if (status != STR_OK) {
        return str_record_failure(string, status);
    }
    return STR_OK;
}

str_status_t str_resize(str_t *string, size_t len, char fill)
{
    str_status_t status = str_validate_mutation(string);

    if (status != STR_OK) {
        return status;
    }
    if (len == string->len) {
        return STR_OK;
    }
    if (len < string->len) {
        str_close_gap(string, len, string->len - len);
        return STR_OK;
    }

    status = str_grow_content(string, len, fill);
    if (status != STR_OK) {
        return str_record_failure(string, status);
    }
    return STR_OK;
}

str_status_t str_insert_n(str_t *string, size_t idx, const char *source, size_t len)
{
    str_status_t status = str_begin_mutation(string, source, len);

    if (status != STR_OK) {
        return status;
    }
    status = str_validate_span(string->len, idx, 0);
    if (status != STR_OK) {
        return str_record_failure(string, status);
    }
    if (len == 0) {
        return STR_OK;
    }

    str_insert_request_t request = {.idx = idx, .source = source, .len = len};
    status = str_prepare_insert(string, &request);
    if (status != STR_OK) {
        return str_record_failure(string, status);
    }

    str_commit_insert(string, request);
    return STR_OK;
}

str_status_t str_insert_view(str_t *string, size_t idx, str_view_t source)
{
    return str_insert_n(string, idx, source.ptr, source.len);
}

str_status_t str_remove(str_t *string, size_t idx, size_t len)
{
    str_status_t status = str_validate_mutation(string);

    if (status != STR_OK) {
        return status;
    }
    status = str_validate_span(string->len, idx, len);
    if (status != STR_OK) {
        return str_record_failure(string, status);
    }
    if (len == 0) {
        return STR_OK;
    }

    str_close_gap(string, idx, len);
    return STR_OK;
}

str_status_t str_replace_view(str_t *string, size_t idx, size_t remove_len, str_view_t replacement)
{
    str_status_t status = str_begin_mutation(string, replacement.ptr, replacement.len);

    if (status != STR_OK) {
        return status;
    }
    status = str_validate_span(string->len, idx, remove_len);
    if (status != STR_OK) {
        return str_record_failure(string, status);
    }
    if (remove_len == 0 && replacement.len == 0) {
        return STR_OK;
    }

    status = str_apply_replacement(string, idx, remove_len, replacement);
    if (status != STR_OK) {
        return str_record_failure(string, status);
    }
    return STR_OK;
}

const char *str_cstr(const str_t *string)
{
    if (string == NULL || string->buf == NULL) {
        return str_empty_buf;
    }
    return string->buf;
}

size_t str_len(const str_t *string)
{
    if (string == NULL) {
        return 0;
    }
    return string->len;
}

size_t str_capacity(const str_t *string)
{
    if (string == NULL || string->cap == 0) {
        return 0;
    }
    return string->cap - (size_t)STR_NUL_BYTES;
}

bool str_is_empty(const str_t *string)
{
    return str_len(string) == 0;
}

str_status_t str_status(const str_t *string)
{
    if (string == NULL) {
        return str_argument_error();
    }
    return string->status;
}

bool str_ok(const str_t *string)
{
    return string != NULL && string->status == STR_OK;
}

bool str_failed(const str_t *string)
{
    return !str_ok(string);
}

bool str_equals(const str_t *string, const str_t *other)
{
    if (string == NULL || other == NULL) {
        return false;
    }

    str_view_t left = str_make_view(string);
    str_view_t right = str_make_view(other);
    return str_is_view_content_equal(left, right);
}

bool str_equals_cstr(const str_t *string, const char *other)
{
    if (string == NULL || other == NULL) {
        return false;
    }

    str_view_t left = str_make_view(string);
    str_view_t right = str_view_from_cstr(other);
    return str_is_view_content_equal(left, right);
}

bool str_starts_with(const str_t *string, const char *prefix)
{
    if (string == NULL || prefix == NULL) {
        return false;
    }

    str_view_t value = str_make_view(string);
    str_view_t prefix_view = str_view_from_cstr(prefix);
    return str_has_view_prefix(value, prefix_view);
}

bool str_ends_with(const str_t *string, const char *suffix)
{
    if (string == NULL || suffix == NULL) {
        return false;
    }

    str_view_t value = str_make_view(string);
    str_view_t suffix_view = str_view_from_cstr(suffix);
    return str_has_view_suffix(value, suffix_view);
}

str_status_t str_find(const str_t *string, size_t *out_idx, const char *needle)
{
    STR_TRY(str_validate_find_arguments(string, out_idx, needle));
    str_view_t haystack = str_make_view(string);
    str_view_t needle_view = str_view_from_cstr(needle);
    *out_idx = str_locate_view_match(haystack, needle_view, STR_SEARCH_FIRST);
    return STR_OK;
}

str_status_t str_find_char(const str_t *string, size_t *out_idx, char byte)
{
    if (string == NULL || out_idx == NULL) {
        return str_argument_error();
    }

    char needle_buffer[] = {byte};
    str_view_t haystack = str_make_view(string);
    str_view_t needle = str_view_from_n(needle_buffer, sizeof(needle_buffer));
    *out_idx = str_locate_view_match(haystack, needle, STR_SEARCH_FIRST);
    return STR_OK;
}

str_view_t str_view(const str_t *string)
{
    if (string == NULL) {
        return str_make_empty_view();
    }
    return str_make_view(string);
}

str_status_t str_slice(const str_t *string, str_view_t *out_view, size_t offset, size_t len)
{
    if (string == NULL || out_view == NULL) {
        return str_argument_error();
    }
    STR_TRY(str_validate_span(string->len, offset, len));
    out_view->ptr = str_cstr(string) + offset;
    out_view->len = len;
    return STR_OK;
}

str_view_t str_view_from_cstr(const char *source)
{
    if (source == NULL) {
        return str_make_empty_view();
    }

    size_t len = str_measure_c_string(source);
    return str_view_from_n(source, len);
}

str_view_t str_view_from_n(const char *source, size_t len)
{
    if (source == NULL && len == 0) {
        return str_make_empty_view();
    }
    return (str_view_t){.ptr = source, .len = len};
}

bool str_view_is_valid(str_view_t view)
{
    return view.ptr != NULL || view.len == 0;
}

bool str_view_equals(str_view_t left, str_view_t right)
{
    if (!str_has_valid_views(left, right)) {
        return false;
    }
    return str_is_view_content_equal(left, right);
}

str_status_t str_view_compare(int *out_order, str_view_t left, str_view_t right)
{
    if (out_order == NULL) {
        return str_argument_error();
    }

    str_status_t status = str_validate_view(left);
    if (status != STR_OK) {
        return status;
    }
    status = str_validate_view(right);
    if (status != STR_OK) {
        return status;
    }

    *out_order = str_compare_valid_views(left, right);
    return STR_OK;
}

bool str_view_starts_with(str_view_t value, str_view_t prefix)
{
    if (!str_has_valid_views(value, prefix)) {
        return false;
    }
    return str_has_view_prefix(value, prefix);
}

bool str_view_ends_with(str_view_t value, str_view_t suffix)
{
    if (!str_has_valid_views(value, suffix)) {
        return false;
    }
    return str_has_view_suffix(value, suffix);
}

str_status_t str_view_find(str_view_t haystack, size_t *out_idx, str_view_t needle)
{
    return str_find_view_match(out_idx, haystack, needle, STR_SEARCH_FIRST);
}

str_status_t str_view_rfind(str_view_t haystack, size_t *out_idx, str_view_t needle)
{
    return str_find_view_match(out_idx, haystack, needle, STR_SEARCH_LAST);
}

str_view_t str_view_trim(str_view_t view)
{
    if (!str_view_is_valid(view) || view.len == 0) {
        return str_make_empty_view();
    }

    str_view_t trimmed = str_trim_view_left(view);
    return str_trim_view_right(trimmed);
}

str_status_t str_split_view(str_view_t source, str_split_out_t *split_output, char separator)
{
    STR_TRY(str_validate_split_arguments(source, split_output));
    return str_write_split_parts(split_output, source, separator);
}

#ifdef STR_TEST
void str_test_fail_alloc_after(size_t success_count)
{
    str_test_alloc_failures_enabled = true;
    str_test_alloc_budget = success_count;
}

void str_test_reset_alloc_failures(void)
{
    str_test_alloc_failures_enabled = false;
    str_test_alloc_budget = 0;
}
#endif

static str_status_t str_argument_error(void)
{
    return STR_ERR_ARG;
}

static str_status_t str_range_error(void)
{
    return STR_ERR_RANGE;
}

static str_status_t str_allocation_error(void)
{
    return STR_ERR_ALLOC;
}

static str_status_t str_overflow_error(void)
{
    return STR_ERR_OVERFLOW;
}

static str_status_t str_format_error(void)
{
    return STR_ERR_FMT;
}

static void str_reset_state(str_t *string)
{
    assert(string != NULL);
    string->buf = NULL;
    string->len = 0;
    string->cap = 0;
    string->status = STR_OK;
}

static void str_transfer_state(str_t *destination, str_t *source)
{
    assert(destination != NULL);
    assert(source != NULL);
    assert(destination != source);

    /* Direct assignment is reserved for this ownership-transfer leaf. */
    *destination = *source;
    str_reset_state(source);
}

static bool str_is_owned(const str_t *string)
{
    assert(string != NULL);
    assert((string->cap == 0) == (string->buf == NULL));
    return string->cap > 0;
}

static void str_clear_failure(str_t *string)
{
    assert(string != NULL);
    string->status = STR_OK;
}

static void str_clear_content(str_t *string)
{
    assert(string != NULL);
    string->len = 0;
    if (string->cap > 0) {
        assert(string->buf != NULL);
        char *buf = string->buf;
        buf[0] = '\0';
    }
}

#ifdef STR_TEST
static bool str_test_consume_allocation_budget(void)
{
    if (!str_test_alloc_failures_enabled) {
        return false;
    }
    if (str_test_alloc_budget == 0) {
        return true;
    }
    str_test_alloc_budget--;
    return false;
}

static bool str_claim_heap_attempt(void)
{
    bool has_injected_failure = str_test_consume_allocation_budget();
    return !has_injected_failure;
}
#endif

static str_status_t str_call_malloc(char **out_buf, size_t bytes)
{
    assert(out_buf != NULL);
    assert(bytes > 0);

    char *allocated_buf = malloc(bytes);
    if (allocated_buf == NULL) {
        return str_allocation_error();
    }
    *out_buf = allocated_buf;
    return STR_OK;
}

static str_status_t str_call_realloc(char **out_buf, char *buf, size_t bytes)
{
    assert(out_buf != NULL);
    assert(buf != NULL);
    assert(bytes > 0);

    char *resized_buf = realloc(buf, bytes);
    if (resized_buf == NULL) {
        return str_allocation_error();
    }
    *out_buf = resized_buf;
    return STR_OK;
}

static str_status_t str_allocate_heap(char **out_buf, size_t bytes)
{
    assert(out_buf != NULL);
    assert(bytes > 0);
#ifdef STR_TEST
    if (!str_claim_heap_attempt()) {
        return str_allocation_error();
    }
#endif
    return str_call_malloc(out_buf, bytes);
}

static str_status_t str_resize_heap(char **out_buf, char *buf, size_t bytes)
{
    assert(out_buf != NULL);
    assert(buf != NULL);
    assert(bytes > 0);
#ifdef STR_TEST
    if (!str_claim_heap_attempt()) {
        return str_allocation_error();
    }
#endif
    return str_call_realloc(out_buf, buf, bytes);
}

static void str_release_heap(char *buf)
{
    free(buf);
}

static str_status_t str_expand_heap(char **out_buf, char *buf, size_t bytes, bool has_owned_buffer)
{
    assert(out_buf != NULL);
    assert(bytes > 0);
    assert((buf != NULL) == has_owned_buffer);
    if (has_owned_buffer) {
        return str_resize_heap(out_buf, buf, bytes);
    }
    return str_allocate_heap(out_buf, bytes);
}

static void str_set_failure(str_t *string, str_status_t status)
{
    assert(string != NULL);
    assert(status != STR_OK);
    string->status = status;
}

static char *str_take_owned_buffer(str_t *string)
{
    assert(string != NULL);
    assert(str_is_owned(string));

    char *owned_buf = string->buf;
    str_reset_state(string);
    return owned_buf;
}

static str_status_t str_allocate_empty_buffer(char **out_buf)
{
    assert(out_buf != NULL);

    char *owned_buf = NULL;
    str_status_t status = str_allocate_heap(&owned_buf, (size_t)STR_NUL_BYTES);
    if (status != STR_OK) {
        return status;
    }
    owned_buf[0] = '\0';
    *out_buf = owned_buf;
    return STR_OK;
}

static str_status_t str_validate_mutation(const str_t *string)
{
    if (string == NULL) {
        return str_argument_error();
    }
    if (string->status != STR_OK) {
        return string->status;
    }
    return STR_OK;
}

static str_status_t str_validate_source(const char *source, size_t len)
{
    if (len == 0) {
        return STR_OK;
    }
    if (source == NULL) {
        return str_argument_error();
    }
    return STR_OK;
}

static str_status_t str_validate_view(str_view_t view)
{
    if (!str_view_is_valid(view)) {
        return str_argument_error();
    }
    return STR_OK;
}

static bool str_has_valid_views(str_view_t left, str_view_t right)
{
    return str_view_is_valid(left) && str_view_is_valid(right);
}

static str_status_t str_begin_mutation(str_t *string, const char *source, size_t len)
{
    STR_TRY(str_validate_mutation(string));

    str_status_t status = str_validate_source(source, len);
    if (status != STR_OK) {
        return str_record_failure(string, status);
    }
    return STR_OK;
}

static str_status_t str_validate_span(size_t len, size_t idx, size_t span)
{
    if (idx > len) {
        return str_range_error();
    }
    if (span > len - idx) {
        return str_range_error();
    }
    return STR_OK;
}

static str_status_t str_measure_bounded_c_string(size_t *out_len, const char *source,
                                                 size_t available)
{
    assert(out_len != NULL);
    assert(source != NULL);

    for (size_t idx = 0; idx < available; idx++) {
        if (source[idx] == '\0') {
            *out_len = idx;
            return STR_OK;
        }
    }
    return str_argument_error();
}

static str_status_t str_measure_c_string_input(str_t *string, size_t *out_len, const char *source)
{
    assert(out_len != NULL);
    str_status_t status = str_validate_mutation(string);
    if (status != STR_OK) {
        return status;
    }
    if (source == NULL) {
        status = str_argument_error();
        return str_record_failure(string, status);
    }

    return str_measure_valid_c_string_input(string, out_len, source);
}

static str_status_t str_measure_valid_c_string_input(str_t *string, size_t *out_len,
                                                     const char *source)
{
    assert(string != NULL);
    assert(out_len != NULL);
    assert(source != NULL);

    size_t idx = str_find_allocation_offset(string, source);
    if (idx == STR_NPOS) {
        *out_len = str_measure_c_string(source);
        return STR_OK;
    }

    return str_measure_internal_c_string(string, out_len, idx);
}

static str_status_t str_measure_internal_c_string(str_t *string, size_t *out_len, size_t offset)
{
    assert(string != NULL);
    assert(out_len != NULL);
    assert(string->buf != NULL);

    str_internal_span_t span = {.offset = offset, .len = (size_t)STR_NUL_BYTES};
    str_status_t status = str_validate_internal_source_span(string, span);
    if (status != STR_OK) {
        return str_record_failure(string, status);
    }

    size_t available = (string->len - offset) + (size_t)STR_NUL_BYTES;
    const char *source = string->buf + offset;
    status = str_measure_bounded_c_string(out_len, source, available);
    if (status != STR_OK) {
        return str_record_failure(string, status);
    }
    return STR_OK;
}

static str_status_t str_record_failure(str_t *string, str_status_t status)
{
    assert(string != NULL);
    assert(status != STR_OK);
    str_set_failure(string, status);
    return status;
}

static str_status_t str_add_size(size_t *out_sum, size_t base, size_t increment)
{
    assert(out_sum != NULL);
    if (base > SIZE_MAX - increment) {
        return str_overflow_error();
    }
    *out_sum = base + increment;
    return STR_OK;
}

static str_status_t str_convert_printed_length(size_t *out_len, int value)
{
    assert(out_len != NULL);
    if (value < 0) {
        return str_format_error();
    }
    if ((uintmax_t)value > (uintmax_t)SIZE_MAX) {
        return str_overflow_error();
    }
    *out_len = (size_t)value;
    return STR_OK;
}

static str_status_t str_calculate_capacity(size_t *out_need, size_t len, size_t additional_len)
{
    size_t bytes = 0;

    assert(out_need != NULL);
    STR_TRY(str_add_size(&bytes, len, additional_len));
    return str_add_size(out_need, bytes, (size_t)STR_NUL_BYTES);
}

static size_t str_measure_c_string(const char *source)
{
    assert(source != NULL);
    return strlen(source);
}

static bool str_is_byte_span_equal(const char *left, const char *right, size_t len)
{
    if (len == 0) {
        return true;
    }
    assert(left != NULL);
    assert(right != NULL);
    return memcmp(left, right, len) == 0;
}

static str_compare_order_t str_compare_bytes(const char *left, const char *right, size_t len)
{
    if (len == 0) {
        return STR_COMPARE_EQUAL;
    }
    assert(left != NULL);
    assert(right != NULL);

    int order = memcmp(left, right, len);
    if (order < 0) {
        return STR_COMPARE_LESS;
    }
    if (order > 0) {
        return STR_COMPARE_GREATER;
    }
    return STR_COMPARE_EQUAL;
}

static str_compare_order_t str_compare_lengths(size_t left_len, size_t right_len)
{
    if (left_len < right_len) {
        return STR_COMPARE_LESS;
    }
    if (left_len > right_len) {
        return STR_COMPARE_GREATER;
    }
    return STR_COMPARE_EQUAL;
}

static str_compare_order_t str_compare_valid_views(str_view_t left, str_view_t right)
{
    assert(str_view_is_valid(left));
    assert(str_view_is_valid(right));

    size_t common_len = left.len < right.len ? left.len : right.len;
    str_compare_order_t order = str_compare_bytes(left.ptr, right.ptr, common_len);
    if (order != STR_COMPARE_EQUAL) {
        return order;
    }
    return str_compare_lengths(left.len, right.len);
}

static bool str_is_view_content_equal(str_view_t left, str_view_t right)
{
    assert(str_view_is_valid(left));
    assert(str_view_is_valid(right));
    if (left.len != right.len) {
        return false;
    }
    return str_is_byte_span_equal(left.ptr, right.ptr, left.len);
}

static bool str_has_view_prefix(str_view_t value, str_view_t prefix)
{
    assert(str_view_is_valid(value));
    assert(str_view_is_valid(prefix));
    if (prefix.len > value.len) {
        return false;
    }
    return str_is_byte_span_equal(value.ptr, prefix.ptr, prefix.len);
}

static bool str_has_view_suffix(str_view_t value, str_view_t suffix)
{
    assert(str_view_is_valid(value));
    assert(str_view_is_valid(suffix));
    if (suffix.len > value.len) {
        return false;
    }
    if (suffix.len == 0) {
        return true;
    }

    size_t suffix_offset = value.len - suffix.len;
    return str_is_byte_span_equal(value.ptr + suffix_offset, suffix.ptr, suffix.len);
}

static void str_move_bytes(char *destination, const char *source, size_t len)
{
    if (len == 0) {
        return;
    }
    assert(destination != NULL);
    assert(source != NULL);
    /* Annex K memmove_s is optional. Callers assert writable bounds. */
    /* NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling) */
    memmove(destination, source, len);
}

static void str_fill_bytes(char *destination, size_t len, unsigned char byte)
{
    assert(destination != NULL);
    /* Annex K memset_s is optional. Callers assert writable bounds. */
    /* NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling) */
    memset(destination, (int)byte, len);
}

static unsigned char str_read_view_byte(str_view_t view, size_t idx)
{
    assert(view.ptr != NULL);
    assert(idx < view.len);
    return (unsigned char)view.ptr[idx];
}

static const char *str_get_view_buffer(str_view_t view)
{
    assert(str_view_is_valid(view));
    if (view.ptr == NULL) {
        return str_empty_buf;
    }
    return view.ptr;
}

static bool str_has_capacity_doubling_overflow(size_t cap)
{
    return cap > SIZE_MAX / (size_t)STR_CAPACITY_GROWTH_FACTOR;
}

static str_status_t str_grow_capacity(size_t *out_cap, size_t cap, size_t need)
{
    assert(out_cap != NULL);
    for (size_t step = 0; step < (size_t)STR_MAX_CAPACITY_GROWTH_STEPS; step++) {
        if (cap >= need) {
            *out_cap = cap;
            return STR_OK;
        }
        if (str_has_capacity_doubling_overflow(cap)) {
            *out_cap = need;
            return STR_OK;
        }
        cap *= (size_t)STR_CAPACITY_GROWTH_FACTOR;
    }
    if (cap >= need) {
        *out_cap = cap;
        return STR_OK;
    }
    return str_overflow_error();
}

static str_status_t str_choose_capacity(size_t *out_cap, size_t current_cap, size_t need)
{
    assert(out_cap != NULL);
    if (need <= current_cap) {
        *out_cap = current_cap;
        return STR_OK;
    }

    size_t cap = current_cap;
    if (cap < (size_t)STR_MIN_CAPACITY_BYTES) {
        cap = (size_t)STR_MIN_CAPACITY_BYTES;
    }
    return str_grow_capacity(out_cap, cap, need);
}

static str_status_t str_grow_heap(str_t *string, size_t new_cap)
{
    assert(string != NULL);
    assert(new_cap > string->cap);

    bool has_owned_buffer = str_is_owned(string);
    char *grown_buf = NULL;
    str_status_t status = str_expand_heap(&grown_buf, string->buf, new_cap, has_owned_buffer);
    if (status != STR_OK) {
        return status;
    }
    if (!has_owned_buffer) {
        grown_buf[0] = '\0';
    }
    string->buf = grown_buf;
    string->cap = new_cap;
    return STR_OK;
}

static str_status_t str_ensure_capacity(str_t *string, size_t need)
{
    assert(string != NULL);
    if (need <= string->cap) {
        return STR_OK;
    }

    size_t new_cap = 0;
    str_status_t status = str_choose_capacity(&new_cap, string->cap, need);
    if (status != STR_OK) {
        return status;
    }
    return str_grow_heap(string, new_cap);
}

static size_t str_find_allocation_offset(const str_t *string, const char *source)
{
    assert(string != NULL);
    assert(source != NULL);
    if (string->buf == NULL) {
        return STR_NPOS;
    }

#if defined(UINTPTR_MAX) && !defined(STR_STRICT_ISO_OVERLAP)
    /*
     * Deviation: pointer-to-integer conversion is implementation-defined. Every supported
     * target maps the bytes of one object to consecutive integer values, so a subtraction in
     * uintptr_t classifies source against the allocation in constant time. Unsigned wraparound
     * sends an address below the buffer past cap. Build with STR_STRICT_ISO_OVERLAP to select
     * the strictly conforming equality scan on a target without this mapping guarantee.
     */
    uintptr_t base = (uintptr_t)(const void *)string->buf;
    uintptr_t addr = (uintptr_t)(const void *)source;
    uintptr_t delta = addr - base;
    if (delta < (uintptr_t)string->cap) {
        return (size_t)delta;
    }
    return STR_NPOS;
#else
    /* Pointer equality against each allocation byte is defined for any valid pointer. */
    for (size_t offset = 0; offset < string->cap; offset++) {
        if (source == string->buf + offset) {
            return offset;
        }
    }
    return STR_NPOS;
#endif
}

static str_status_t str_validate_internal_source_span(const str_t *string, str_internal_span_t span)
{
    assert(string != NULL);
    assert(string->buf != NULL);
    assert(span.len > 0);

    if (span.offset > string->len) {
        return str_argument_error();
    }
    size_t remaining = (string->len - span.offset) + (size_t)STR_NUL_BYTES;
    if (span.len > remaining) {
        return str_argument_error();
    }
    return STR_OK;
}

static str_status_t str_detect_overlap(const str_t *string, str_overlap_t *out_overlap,
                                       const char *source, size_t len)
{
    assert(string != NULL);
    assert(out_overlap != NULL);
    assert(source != NULL);
    assert(len > 0);

    out_overlap->is_inside = false;
    out_overlap->idx = 0;
    size_t idx = str_find_allocation_offset(string, source);
    if (idx == STR_NPOS) {
        return STR_OK;
    }
    str_internal_span_t span = {.offset = idx, .len = len};
    STR_TRY(str_validate_internal_source_span(string, span));

    out_overlap->is_inside = true;
    out_overlap->idx = idx;
    return STR_OK;
}

static str_status_t str_prepare_insert(str_t *string, str_insert_request_t *request)
{
    assert(string != NULL);
    assert(request != NULL);
    assert(request->source != NULL);
    assert(request->len > 0);

    size_t need = 0;
    str_status_t status = str_calculate_capacity(&need, string->len, request->len);
    if (status != STR_OK) {
        return status;
    }

    str_overlap_t overlap;
    status = str_detect_overlap(string, &overlap, request->source, request->len);
    if (status != STR_OK) {
        return status;
    }

    size_t adjusted_source_offset = 0;
    if (overlap.is_inside) {
        adjusted_source_offset = str_adjust_source_offset(overlap, request);
    }

    status = str_ensure_capacity(string, need);
    if (status != STR_OK) {
        return status;
    }

    if (overlap.is_inside) {
        request->source = string->buf + adjusted_source_offset;
    }
    return STR_OK;
}

static str_status_t str_prepare_write(str_t *string, const char **out_source,
                                      str_write_request_t request)
{
    assert(string != NULL);
    assert(out_source != NULL);
    assert(request.source != NULL);
    assert(request.len > 0);

    size_t need = 0;
    str_status_t status = str_calculate_capacity(&need, request.base_len, request.len);
    if (status != STR_OK) {
        return status;
    }

    str_overlap_t overlap;
    status = str_detect_overlap(string, &overlap, request.source, request.len);
    if (status != STR_OK) {
        return status;
    }

    status = str_ensure_capacity(string, need);
    if (status != STR_OK) {
        return status;
    }

    if (overlap.is_inside) {
        *out_source = string->buf + overlap.idx;
    } else {
        *out_source = request.source;
    }
    return STR_OK;
}

static str_status_t str_prepare_valid_span(str_t *string, const char **out_source,
                                           str_write_request_t request)
{
    assert(string != NULL);
    assert(string->status == STR_OK);
    assert(out_source != NULL);
    assert(request.source != NULL);
    assert(request.len > 0);

    str_status_t status = str_prepare_write(string, out_source, request);
    if (status != STR_OK) {
        return str_record_failure(string, status);
    }
    return STR_OK;
}

static str_status_t str_set_valid_span(str_t *string, const char *source, size_t len)
{
    assert(string != NULL);
    assert(string->status == STR_OK);
    if (len == 0) {
        str_clear_content(string);
        return STR_OK;
    }

    const char *ready_source = NULL;
    str_write_request_t request = {.source = source, .len = len, .base_len = 0};
    str_status_t status = str_prepare_valid_span(string, &ready_source, request);
    if (status != STR_OK) {
        return status;
    }

    str_replace_content(string, ready_source, len);
    return STR_OK;
}

static str_status_t str_append_valid_span(str_t *string, const char *source, size_t len)
{
    assert(string != NULL);
    assert(string->status == STR_OK);
    if (len == 0) {
        return STR_OK;
    }

    const char *ready_source = NULL;
    str_write_request_t request = {.source = source, .len = len, .base_len = string->len};
    str_status_t status = str_prepare_valid_span(string, &ready_source, request);
    if (status != STR_OK) {
        return status;
    }

    str_append_content(string, ready_source, len);
    return STR_OK;
}

static str_status_t str_append_unaliased_span(str_t *string, const char *source, size_t len)
{
    assert(string != NULL);
    assert(string->status == STR_OK);
    assert(source != NULL);
    if (len == 0) {
        return STR_OK;
    }

    size_t need = 0;
    str_status_t status = str_calculate_capacity(&need, string->len, len);
    if (status != STR_OK) {
        return str_record_failure(string, status);
    }
    status = str_ensure_capacity(string, need);
    if (status != STR_OK) {
        return str_record_failure(string, status);
    }

    str_append_content(string, source, len);
    return STR_OK;
}

static void str_write_terminator(str_t *string)
{
    assert(string != NULL);
    assert(string->buf != NULL);
    assert(string->cap > 0);
    assert(string->len < string->cap);
    char *buf = string->buf;
    size_t len = string->len;
    buf[len] = '\0';
}

static void str_replace_content(str_t *string, const char *source, size_t len)
{
    assert(string != NULL);
    assert(string->buf != NULL);
    assert(len < string->cap);
    str_move_bytes(string->buf, source, len);
    string->len = len;
    str_write_terminator(string);
}

static void str_append_content(str_t *string, const char *source, size_t len)
{
    assert(string != NULL);
    assert(string->buf != NULL);
    assert(string->len < string->cap);
    assert(len <= string->cap - string->len - (size_t)STR_NUL_BYTES);
    str_move_bytes(string->buf + string->len, source, len);
    string->len += len;
    str_write_terminator(string);
}

static size_t str_adjust_source_offset(str_overlap_t overlap, const str_insert_request_t *request)
{
    assert(overlap.is_inside);
    assert(request != NULL);
    assert(request->len > 0);
    if (overlap.idx >= request->idx) {
        return overlap.idx + request->len;
    }
    return overlap.idx;
}

static void str_write_content(str_t *string, size_t idx, const char *source, size_t len)
{
    assert(string != NULL);
    assert(string->buf != NULL);
    assert(idx <= string->cap);
    assert(len <= string->cap - idx);
    str_move_bytes(string->buf + idx, source, len);
}

static void str_commit_insert(str_t *string, str_insert_request_t request)
{
    assert(string != NULL);
    assert(string->buf != NULL);
    assert(request.idx <= string->len);
    assert(request.source != NULL);
    assert(request.len <= string->cap - string->len - (size_t)STR_NUL_BYTES);

    size_t tail_len = (string->len - request.idx) + (size_t)STR_NUL_BYTES;
    str_move_bytes(string->buf + request.idx + request.len, string->buf + request.idx, tail_len);
    str_move_bytes(string->buf + request.idx, request.source, request.len);
    string->len += request.len;
    str_write_terminator(string);
}

static void str_close_gap(str_t *string, size_t idx, size_t len)
{
    assert(string != NULL);
    assert(string->buf != NULL);
    assert(idx <= string->len);
    assert(len <= string->len - idx);

    size_t tail_len = (string->len - idx - len) + (size_t)STR_NUL_BYTES;
    str_move_bytes(string->buf + idx, string->buf + idx + len, tail_len);
    string->len -= len;
    str_write_terminator(string);
}

static void str_extend_content(str_t *string, size_t len, char fill)
{
    assert(string != NULL);
    assert(string->buf != NULL);
    assert(string->len < len);
    assert(len < string->cap);
    str_fill_bytes(string->buf + string->len, len - string->len, (unsigned char)fill);
    string->len = len;
    str_write_terminator(string);
}

static str_status_t str_grow_content(str_t *string, size_t len, char fill)
{
    assert(string != NULL);
    assert(len > string->len);

    size_t need = 0;
    str_status_t status = str_calculate_capacity(&need, 0, len);
    if (status != STR_OK) {
        return status;
    }
    status = str_ensure_capacity(string, need);
    if (status != STR_OK) {
        return status;
    }
    str_extend_content(string, len, fill);
    return STR_OK;
}

static void str_discard_heap(str_t *string)
{
    assert(string != NULL);
    assert(string->len == 0);
    assert(str_is_owned(string));
    str_release_heap(string->buf);
    str_reset_state(string);
}

static str_status_t str_shrink_heap(str_t *string, size_t exact_cap)
{
    assert(string != NULL);
    assert(string->buf != NULL);
    assert(exact_cap > 0);
    if (exact_cap == string->cap) {
        return STR_OK;
    }

    char *shrunk_buf = NULL;
    str_status_t status = str_resize_heap(&shrunk_buf, string->buf, exact_cap);
    if (status != STR_OK) {
        return status;
    }
    string->buf = shrunk_buf;
    string->cap = exact_cap;
    return STR_OK;
}

static str_status_t str_stage_replacement(const str_t *string, str_staged_view_t *out_staged,
                                          str_view_t source)
{
    assert(string != NULL);
    assert(out_staged != NULL);
    assert(str_view_is_valid(source));
    out_staged->view = source;
    out_staged->owned_buf = NULL;
    if (source.len == 0) {
        return STR_OK;
    }

    str_overlap_t overlap = {0};
    str_status_t status = str_detect_overlap(string, &overlap, source.ptr, source.len);
    if (status != STR_OK) {
        return status;
    }
    if (!overlap.is_inside) {
        return STR_OK;
    }

    char *owned_buf = NULL;
    status = str_allocate_heap(&owned_buf, source.len);
    if (status != STR_OK) {
        return status;
    }
    str_move_bytes(owned_buf, source.ptr, source.len);
    out_staged->view.ptr = owned_buf;
    out_staged->owned_buf = owned_buf;
    return STR_OK;
}

static void str_release_staged_view(str_staged_view_t *staged)
{
    assert(staged != NULL);
    str_release_heap(staged->owned_buf);
    staged->view = str_make_empty_view();
    staged->owned_buf = NULL;
}

static void str_commit_replacement(str_t *string, size_t idx, size_t remove_len,
                                   str_view_t replacement)
{
    assert(string != NULL);
    assert(string->buf != NULL);
    assert(str_view_is_valid(replacement));
    assert(idx <= string->len);
    assert(remove_len <= string->len - idx);

    size_t suffix_idx = idx + remove_len;
    size_t suffix_len = (string->len - suffix_idx) + (size_t)STR_NUL_BYTES;
    size_t new_len = (string->len - remove_len) + replacement.len;
    assert(new_len < string->cap);

    str_move_bytes(string->buf + idx + replacement.len, string->buf + suffix_idx, suffix_len);
    str_move_bytes(string->buf + idx, replacement.ptr, replacement.len);
    string->len = new_len;
    str_write_terminator(string);
}

static str_status_t str_replace_equal_span(str_t *string, size_t idx, str_view_t replacement)
{
    assert(string != NULL);
    assert(idx <= string->len);
    assert(replacement.len <= string->len - idx);
    assert(replacement.len > 0);

    str_overlap_t overlap = {0};
    str_status_t status = str_detect_overlap(string, &overlap, replacement.ptr, replacement.len);
    if (status != STR_OK) {
        return status;
    }
    const char *valid_source = replacement.ptr;
    if (overlap.is_inside) {
        valid_source = string->buf + overlap.idx;
    }
    str_write_content(string, idx, valid_source, replacement.len);
    return STR_OK;
}

static str_status_t str_calculate_replacement_length(size_t *out_len, size_t old_len,
                                                     size_t remove_len, size_t added_len)
{
    assert(out_len != NULL);
    assert(remove_len <= old_len);
    return str_add_size(out_len, old_len - remove_len, added_len);
}

static str_status_t str_replace_resized_span(str_t *string, str_replace_request_t request)
{
    assert(string != NULL);

    size_t need = 0;
    str_status_t status = str_calculate_capacity(&need, 0, request.new_len);
    if (status != STR_OK) {
        return status;
    }

    str_staged_view_t staged = {.view = request.replacement, .owned_buf = NULL};
    status = str_stage_replacement(string, &staged, request.replacement);
    if (status != STR_OK) {
        return status;
    }

    status = str_ensure_capacity(string, need);
    if (status != STR_OK) {
        str_release_staged_view(&staged);
        return status;
    }

    str_commit_replacement(string, request.idx, request.remove_len, staged.view);
    str_release_staged_view(&staged);
    return STR_OK;
}

static str_status_t str_apply_replacement(str_t *string, size_t idx, size_t remove_len,
                                          str_view_t replacement)
{
    assert(string != NULL);
    assert(str_view_is_valid(replacement));
    assert(idx <= string->len);
    assert(remove_len <= string->len - idx);
    if (remove_len == replacement.len) {
        return str_replace_equal_span(string, idx, replacement);
    }

    size_t new_len = 0;
    str_status_t status =
        str_calculate_replacement_length(&new_len, string->len, remove_len, replacement.len);
    if (status != STR_OK) {
        return status;
    }

    str_replace_request_t request = {
        .idx = idx,
        .remove_len = remove_len,
        .new_len = new_len,
        .replacement = replacement,
    };
    return str_replace_resized_span(string, request);
}

static str_status_t str_render_format(size_t *out_len, str_format_destination_t destination,
                                      const char *format, va_list arguments)
{
    assert(out_len != NULL);
    assert(destination.buf != NULL);
    assert(destination.capacity > 0);
    assert(format != NULL);

    va_list arguments_copy;
    va_copy(arguments_copy, arguments);
    /* Annex K vsnprintf_s is optional. Destination capacity is explicit. */
    /* NOLINTNEXTLINE(clang-analyzer-security.*, clang-analyzer-valist.Uninitialized) */
    int printed = vsnprintf(destination.buf, destination.capacity, format, arguments_copy);
    va_end(arguments_copy);
    if (printed < 0) {
        return str_format_error();
    }
    return str_convert_printed_length(out_len, printed);
}

static str_status_t str_verify_format_render(str_format_destination_t destination,
                                             size_t expected_len, const char *format,
                                             va_list arguments)
{
    assert(destination.buf != NULL);
    assert(format != NULL);

    size_t rendered_len = 0;
    str_status_t status = str_render_format(&rendered_len, destination, format, arguments);

    if (status != STR_OK) {
        return status;
    }
    if (rendered_len != expected_len) {
        return str_format_error();
    }
    return STR_OK;
}

static str_status_t str_build_formatted_buffer(char **out_buf, size_t expected_len,
                                               const char *format, va_list arguments)
{
    assert(out_buf != NULL);
    assert(format != NULL);

    size_t capacity = 0;
    str_status_t status = str_calculate_capacity(&capacity, 0, expected_len);
    if (status != STR_OK) {
        return status;
    }

    char *buf = NULL;
    status = str_allocate_heap(&buf, capacity);
    if (status != STR_OK) {
        return status;
    }

    str_format_destination_t destination = {.buf = buf, .capacity = capacity};
    status = str_verify_format_render(destination, expected_len, format, arguments);
    if (status != STR_OK) {
        str_release_heap(buf);
        return status;
    }

    *out_buf = buf;
    return STR_OK;
}

static str_status_t str_append_format_payload(str_t *string, str_format_payload_t payload,
                                              va_list arguments)
{
    assert(string != NULL);
    assert(payload.format != NULL);
    assert(payload.stack_destination.buf != NULL);
    if (payload.rendered_len < payload.stack_destination.capacity) {
        return str_append_unaliased_span(string, payload.stack_destination.buf,
                                         payload.rendered_len);
    }

    char *rendered_buf = NULL;
    /* Standard C has no side-effect-free size query; the public contract documents rerendering. */
    str_status_t status =
        str_build_formatted_buffer(&rendered_buf, payload.rendered_len, payload.format, arguments);
    if (status != STR_OK) {
        return str_record_failure(string, status);
    }

    status = str_append_unaliased_span(string, rendered_buf, payload.rendered_len);
    str_release_heap(rendered_buf);
    return status;
}

static str_view_t str_make_view(const str_t *string)
{
    assert(string != NULL);
    return (str_view_t){.ptr = str_cstr(string), .len = string->len};
}

static str_view_t str_make_empty_view(void)
{
    return (str_view_t){.ptr = str_empty_buf, .len = 0};
}

static str_status_t str_validate_find_arguments(const str_t *string, const size_t *out_idx,
                                                const char *needle)
{
    if (string == NULL || out_idx == NULL || needle == NULL) {
        return str_argument_error();
    }
    return STR_OK;
}

static str_status_t str_find_view_match(size_t *out_idx, str_view_t haystack, str_view_t needle,
                                        str_search_mode_t mode)
{
    if (out_idx == NULL) {
        return str_argument_error();
    }

    str_status_t status = str_validate_view(haystack);
    if (status != STR_OK) {
        return status;
    }
    status = str_validate_view(needle);
    if (status != STR_OK) {
        return status;
    }
    if (mode == STR_SEARCH_LAST && needle.len == 0 && haystack.len == STR_NPOS) {
        return str_overflow_error();
    }

    *out_idx = str_locate_view_match(haystack, needle, mode);
    return STR_OK;
}

static size_t str_locate_view_match(str_view_t haystack, str_view_t needle, str_search_mode_t mode)
{
    assert(str_view_is_valid(haystack));
    assert(str_view_is_valid(needle));
    assert(mode == STR_SEARCH_FIRST || mode == STR_SEARCH_LAST);
    if (needle.len > 0) {
        return str_search_view(haystack, needle, mode);
    }
    if (mode == STR_SEARCH_FIRST) {
        return 0;
    }
    return haystack.len;
}

static bool str_is_byte_before(unsigned char left, unsigned char right, bool is_reverse_order)
{
    if (is_reverse_order) {
        return left > right;
    }
    return left < right;
}

static void str_suffix_walk_step(str_suffix_walk_t *walk, unsigned char candidate_byte,
                                 unsigned char suffix_byte, bool is_reverse_order)
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
    if (str_is_byte_before(candidate_byte, suffix_byte, is_reverse_order)) {
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

static bool str_run_suffix_walk_pass(str_suffix_walk_t *walk, str_view_t needle,
                                     bool is_reverse_order)
{
    assert(walk != NULL);
    assert(needle.ptr != NULL);
    assert(walk->candidate < needle.len);
    assert(walk->offset > 0);

    for (size_t step = 0; step < needle.len; step++) {
        if (walk->offset >= needle.len - walk->candidate) {
            return true;
        }

        size_t suffix_idx = walk->factor.cut + walk->offset - (size_t)STR_NUL_BYTES;
        unsigned char suffix_byte = str_read_view_byte(needle, suffix_idx);
        size_t candidate_idx = walk->candidate + walk->offset;
        unsigned char candidate_byte = str_read_view_byte(needle, candidate_idx);
        str_suffix_walk_step(walk, candidate_byte, suffix_byte, is_reverse_order);
    }
    return walk->offset >= needle.len - walk->candidate;
}

static str_factor_t str_find_maximal_suffix(str_view_t needle, bool is_reverse_order)
{
    str_suffix_walk_t walk = {
        .factor = {.cut = 0, .period = 1},
        .candidate = 0,
        .offset = 1,
    };

    assert(needle.ptr != NULL);
    assert(needle.len > 0);

    for (size_t pass = 0; pass < (size_t)STR_SUFFIX_WALK_PASSES; pass++) {
        bool is_complete = str_run_suffix_walk_pass(&walk, needle, is_reverse_order);
        if (is_complete) {
            return walk.factor;
        }
    }

    /* Two-Way maximal-suffix factorization performs fewer than twice len comparisons. */
    assert(walk.offset >= needle.len - walk.candidate);
    return walk.factor;
}

static str_factor_t str_find_critical_factor(str_view_t needle)
{
    assert(needle.ptr != NULL);
    assert(needle.len > 0);

    str_factor_t forward = str_find_maximal_suffix(needle, false);
    str_factor_t reverse = str_find_maximal_suffix(needle, true);

    if (reverse.cut > forward.cut) {
        return reverse;
    }
    return forward;
}

static bool str_is_factor_periodic(str_view_t needle, str_factor_t factor)
{
    assert(needle.ptr != NULL);
    assert(factor.cut < needle.len);
    assert(factor.period > 0);
    if (factor.period > needle.len - factor.cut) {
        return false;
    }
    return str_is_byte_span_equal(needle.ptr, needle.ptr + factor.period, factor.cut);
}

static size_t str_calculate_nonperiodic_shift(str_view_t needle, str_factor_t factor)
{
    assert(factor.cut > 0);
    assert(factor.cut < needle.len);

    size_t left_shift = factor.cut;
    size_t right_shift = needle.len - factor.cut + (size_t)STR_NUL_BYTES;
    if (left_shift > right_shift) {
        return left_shift;
    }
    return right_shift;
}

static size_t str_scan_right(str_view_t haystack, str_view_t needle, str_factor_t factor,
                             str_search_state_t state)
{
    assert(haystack.ptr != NULL);
    assert(needle.ptr != NULL);
    assert(haystack.len >= needle.len);
    assert(state.idx <= haystack.len - needle.len);
    assert(state.memory < needle.len);

    size_t scan = factor.cut;
    if (state.memory > scan) {
        scan = state.memory;
    }
    while (scan < needle.len && needle.ptr[scan] == haystack.ptr[state.idx + scan]) {
        scan++;
    }
    return scan;
}

static bool str_has_candidate_left_match(str_view_t haystack, str_view_t needle,
                                         str_factor_t factor, str_search_state_t state)
{
    assert(haystack.ptr != NULL);
    assert(needle.ptr != NULL);
    assert(haystack.len >= needle.len);
    assert(state.idx <= haystack.len - needle.len);
    assert(state.memory < needle.len);

    size_t scan = factor.cut;
    while (scan > state.memory) {
        size_t idx = scan - (size_t)STR_NUL_BYTES;
        if (needle.ptr[idx] != haystack.ptr[state.idx + idx]) {
            return false;
        }
        scan--;
    }
    return true;
}

static bool str_search_advance(str_search_state_t *state, size_t last_idx, size_t shift)
{
    assert(state != NULL);
    assert(state->idx <= last_idx);
    assert(shift > 0);
    if (shift > last_idx - state->idx) {
        return false;
    }
    state->idx += shift;
    return true;
}

static size_t str_search_view(str_view_t haystack, str_view_t needle, str_search_mode_t mode)
{
    assert(needle.ptr != NULL);
    assert(needle.len > 0);
    assert(mode == STR_SEARCH_FIRST || mode == STR_SEARCH_LAST);
    if (haystack.ptr == NULL || haystack.len < needle.len) {
        return STR_NPOS;
    }
    if (needle.len == (size_t)STR_NUL_BYTES) {
        str_byte_search_t search = {.byte = (unsigned char)needle.ptr[0], .mode = mode};
        return str_search_view_byte(haystack, search);
    }

    return str_search_two_way(haystack, needle, mode);
}

static size_t str_search_two_way(str_view_t haystack, str_view_t needle, str_search_mode_t mode)
{
    assert(haystack.ptr != NULL);
    assert(haystack.len >= needle.len);
    assert(needle.len > (size_t)STR_NUL_BYTES);

    str_search_context_t context = str_build_search_context(haystack, needle, mode);
    str_search_state_t state = {.idx = 0, .memory = 0};
    size_t found = STR_NPOS;

    for (size_t step = 0; step <= context.last_idx; step++) {
        assert(state.idx <= context.last_idx);
        bool has_next = str_search_step(&context, &state, &found);
        if (!has_next) {
            return found;
        }
    }
    return found;
}

static str_search_context_t str_build_search_context(str_view_t haystack, str_view_t needle,
                                                     str_search_mode_t mode)
{
    assert(haystack.ptr != NULL);
    assert(needle.ptr != NULL);
    assert(haystack.len >= needle.len);
    assert(needle.len > (size_t)STR_NUL_BYTES);
    assert(mode == STR_SEARCH_FIRST || mode == STR_SEARCH_LAST);

    str_factor_t factor = str_find_critical_factor(needle);
    bool is_periodic = str_is_factor_periodic(needle, factor);

    return (str_search_context_t){
        .haystack = haystack,
        .needle = needle,
        .factor = factor,
        .full_shift = is_periodic ? factor.period : str_calculate_nonperiodic_shift(needle, factor),
        .saved_memory = is_periodic ? needle.len - factor.period : 0,
        .last_idx = haystack.len - needle.len,
        .mode = mode,
    };
}

static bool str_search_on_mismatch(const str_search_context_t *context, str_search_state_t *state,
                                   size_t mismatch)
{
    assert(context != NULL);
    assert(state != NULL);
    assert(mismatch < context->needle.len);

    size_t shift = mismatch - context->factor.cut + (size_t)STR_NUL_BYTES;
    bool has_next = str_search_advance(state, context->last_idx, shift);
    state->memory = 0;
    return has_next;
}

static bool str_search_after_right_match(const str_search_context_t *context,
                                         str_search_state_t *state, size_t *found)
{
    assert(context != NULL);
    assert(state != NULL);
    assert(found != NULL);
    if (str_has_candidate_left_match(context->haystack, context->needle, context->factor, *state)) {
        *found = state->idx;
        if (context->mode == STR_SEARCH_FIRST) {
            return false;
        }
    }

    bool has_next = str_search_advance(state, context->last_idx, context->full_shift);
    state->memory = context->saved_memory;
    return has_next;
}

static bool str_search_step(const str_search_context_t *context, str_search_state_t *state,
                            size_t *found)
{
    assert(context != NULL);
    assert(state != NULL);
    assert(found != NULL);

    size_t mismatch = str_scan_right(context->haystack, context->needle, context->factor, *state);
    if (mismatch < context->needle.len) {
        return str_search_on_mismatch(context, state, mismatch);
    }
    return str_search_after_right_match(context, state, found);
}

static size_t str_view_find_byte(str_view_t haystack, unsigned char byte)
{
    assert(haystack.ptr != NULL);
    assert(haystack.len > 0);

    for (size_t idx = 0; idx < haystack.len; idx++) {
        if ((unsigned char)haystack.ptr[idx] == byte) {
            return idx;
        }
    }
    return STR_NPOS;
}

static size_t str_view_rfind_byte(str_view_t haystack, unsigned char byte)
{
    assert(haystack.ptr != NULL);
    assert(haystack.len > 0);

    size_t idx = haystack.len;
    while (idx > 0) {
        idx--;
        if ((unsigned char)haystack.ptr[idx] == byte) {
            return idx;
        }
    }
    return STR_NPOS;
}

static size_t str_search_view_byte(str_view_t haystack, str_byte_search_t search)
{
    assert(haystack.ptr != NULL);
    assert(haystack.len > 0);
    if (search.mode == STR_SEARCH_FIRST) {
        return str_view_find_byte(haystack, search.byte);
    }
    return str_view_rfind_byte(haystack, search.byte);
}

static bool str_is_ascii_whitespace(unsigned char byte)
{
    size_t whitespace_count =
        sizeof(str_ascii_whitespace_bytes) / sizeof(str_ascii_whitespace_bytes[0]);
    for (size_t idx = 0; idx < whitespace_count; idx++) {
        if (byte == str_ascii_whitespace_bytes[idx]) {
            return true;
        }
    }
    return false;
}

static str_view_t str_trim_view_left(str_view_t view)
{
    assert(str_view_is_valid(view));
    while (view.len > 0 && str_is_ascii_whitespace((unsigned char)view.ptr[0])) {
        view.ptr++;
        view.len--;
    }
    return view;
}

static str_view_t str_trim_view_right(str_view_t view)
{
    assert(str_view_is_valid(view));
    while (view.len > 0 && str_is_ascii_whitespace((unsigned char)view.ptr[view.len - 1])) {
        view.len--;
    }
    return view;
}

static str_status_t str_validate_split_arguments(str_view_t source,
                                                 const str_split_out_t *split_output)
{
    if (split_output == NULL) {
        return str_argument_error();
    }
    if (split_output->cap > 0 && split_output->parts == NULL) {
        return str_argument_error();
    }
    return str_validate_view(source);
}

static void str_store_split_part(str_split_out_t *split_output, size_t count, const char *source,
                                 size_t len)
{
    assert(split_output != NULL);
    if (count >= split_output->cap) {
        return;
    }
    assert(split_output->parts != NULL);

    str_view_t *parts = split_output->parts;
    str_view_t *part = &parts[count];
    part->ptr = source;
    part->len = len;
}

static size_t str_find_separator(str_view_t source, str_split_cursor_t cursor)
{
    assert(str_view_is_valid(source));
    assert(cursor.idx <= source.len);
    const char *buffer = str_get_view_buffer(source);
    for (size_t idx = cursor.idx; idx < source.len; idx++) {
        if (buffer[idx] == cursor.separator) {
            return idx;
        }
    }
    return STR_NPOS;
}

static str_status_t str_write_split_parts(str_split_out_t *split_output, str_view_t source,
                                          char separator)
{
    assert(split_output != NULL);
    assert(str_view_is_valid(source));

    const char *buffer = str_get_view_buffer(source);
    if (source.len == SIZE_MAX) {
        size_t idx = 0;
        while (idx < source.len && buffer[idx] == separator) {
            idx++;
        }
        if (idx == source.len) {
            return str_overflow_error();
        }
    }

    size_t start = 0;
    size_t count = 0;
    str_split_cursor_t cursor = {.idx = 0, .separator = separator};
    while (cursor.idx < source.len) {
        size_t separator_idx = str_find_separator(source, cursor);
        if (separator_idx == STR_NPOS) {
            break;
        }
        str_store_split_part(split_output, count, buffer + start, separator_idx - start);
        count++;
        start = separator_idx + (size_t)STR_NUL_BYTES;
        cursor.idx = start;
    }
    str_store_split_part(split_output, count, buffer + start, source.len - start);
    count++;
    split_output->count = count;
    assert(split_output->count > 0);
    assert(source.len == SIZE_MAX || split_output->count <= source.len + (size_t)STR_NUL_BYTES);
    return STR_OK;
}
