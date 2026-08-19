#ifndef STR_H
#define STR_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Public operation results. Error values remain stable API. */
typedef enum {
    /* Operation completed successfully. */
    STR_OK = 0,
    /* A pointer, object, view, or other argument is invalid. */
    STR_ERR_ARG = -1,
    /* Dynamic allocation failed. */
    STR_ERR_ALLOC = -2,
    /* A requested span lies outside current content. */
    STR_ERR_RANGE = -3,
    /* A size calculation cannot be represented by size_t. */
    STR_ERR_OVERFLOW = -4,
    /* Formatted output could not be produced consistently. */
    STR_ERR_FMT = -5
} str_status_t;

/*
 * A size_t sentinel cannot be represented by a C enum.
 * STR_NPOS is the standard maximum size and denotes a missing search result.
 */
#define STR_NPOS SIZE_MAX

/*
 * This guarded extension lets supported compilers check printf arguments.
 * Attribute suffix syntax cannot be replaced by a standard C declaration.
 */
#if defined(__GNUC__) || defined(__clang__)
#define STR_PRINTF_LIKE(format_index, first_arg)                                                   \
    __attribute__((format(printf, (format_index), (first_arg))))
#else
#define STR_PRINTF_LIKE(format_index, first_arg)
#endif

/*
 * Expansion occurs only after str_t is visible. The braces intentionally form an initializer:
 * a compound literal would also permit the unsupported assignment of one str_t to another.
 */
#define STR_EMPTY                                                                                  \
    {                                                                                              \
        .buf = NULL, .len = 0, .cap = 0, .status = STR_OK                                          \
    }

/*
 * Owned growable byte string. len counts content bytes, including embedded NULs.
 * cap counts allocated bytes, including the trailing NUL.
 * Invariants:
 * - cap == 0 implies buf == NULL and len == 0.
 * - cap > 0 implies buf != NULL, len < cap, and buf[len] == '\0'.
 *
 * Storage must be zero-initialized or passed to str_init exactly once before use.
 * Fields are caller-readable for stack allocation and diagnostics. Callers must not modify
 * them or copy this struct by assignment. Use str_copy or str_move.
 *
 * A failed mutator preserves bytes, length, capacity, and ownership. It records a sticky
 * status. Later mutators return that status without changing content until str_clear or
 * str_clear_error. Lifecycle operations remain available for cleanup or recovery.
 */
typedef struct str {
    char *buf;
    size_t len;
    size_t cap;
    str_status_t status;
} str_t;

/*
 * Borrowed byte span. It need not be NUL-terminated and never owns storage.
 * A view is structurally valid when ptr is non-NULL or len is zero. A nonempty view's
 * pointer must designate at least len readable bytes. A view into str_t storage is invalidated
 * by any successful mutation, reserve, shrink, move, detach, or deinit.
 */
typedef struct {
    const char *ptr;
    size_t len;
} str_view_t;

/*
 * Caller-owned storage for borrowed split views.
 * cap > 0 requires parts to designate cap writable elements. count is the full result size,
 * so count may exceed cap when the caller-provided array truncates the stored results.
 */
typedef struct {
    str_view_t *parts;
    size_t cap;
    size_t count;
} str_split_out_t;

/*
 * Independent strings may be used concurrently. Access to a shared string, or to a view into it,
 * requires external synchronization whenever any participant may mutate the owning string.
 */

/* Initializes previously uninitialized caller-owned storage. NULL is a no-op. */
void str_init(str_t *string);

/* Releases storage owned by string. NULL or already-deinitialized storage is accepted. */
void str_deinit(str_t *string);

/*
 * Restores initialized string to reusable empty state while retaining allocation.
 * NULL is a no-op.
 */
void str_clear(str_t *string);

/* Clears sticky status on initialized string without changing content. NULL is a no-op. */
void str_clear_error(str_t *string);

/*
 * Releases destination's current allocation, then transfers ownership plus sticky status from
 * initialized source. Source becomes empty. NULL or identical pointers are a no-op. Never fails.
 */
void str_move(str_t *destination, str_t *source);

/*
 * Deep-copies bytes from initialized source into initialized destination. A NULL destination
 * returns STR_ERR_ARG. A NULL source records STR_ERR_ARG on a destination whose status is STR_OK.
 * Source status is not copied. Identical pointers with STR_OK status are a successful no-op.
 * Existing destination status propagates unchanged. STR_ERR_ALLOC or STR_ERR_OVERFLOW preserves
 * destination content while recording the failure.
 */
str_status_t str_copy(str_t *destination, const str_t *source);

/*
 * Transfers string's owned allocation to the caller, then resets string to STR_EMPTY.
 * The caller releases the returned C string with free(). Empty content returns allocated storage.
 * NULL string returns NULL. STR_ERR_ALLOC returns NULL while recording the failure on string.
 */
char *str_detach(str_t *string);

/*
 * Returns a borrowed static status name. Unknown values map to "STR_ERR_UNKNOWN". Never fails.
 */
const char *str_status_name(str_status_t status);

/*
 * Unless stated otherwise, mutators require a non-NULL initialized string. A NULL string returns
 * STR_ERR_ARG, with no object on which to record it. Existing sticky status propagates unchanged.
 * A borrowed source may begin in existing content or at its terminator, but it must not extend
 * into spare capacity.
 * Mutators accepting borrowed pointers may perform work proportional to current allocation
 * capacity to classify supported self-sources without nonportable pointer ordering.
 */

/*
 * Replaces non-NULL initialized string from borrowed NUL-terminated source. Existing sticky status
 * propagates. NULL source records STR_ERR_ARG. STR_ERR_ALLOC or STR_ERR_OVERFLOW is recorded.
 */
str_status_t str_set(str_t *string, const char *source);

/*
 * Replaces non-NULL initialized string from len borrowed bytes at source.
 * NULL is valid for zero len.
 * A self-source may include the existing terminator, never spare capacity. Existing sticky status
 * propagates. STR_ERR_ARG, STR_ERR_ALLOC, or STR_ERR_OVERFLOW is recorded on failure.
 */
str_status_t str_set_n(str_t *string, const char *source, size_t len);

/*
 * Replaces non-NULL initialized string from borrowed source. Existing sticky status propagates.
 * STR_ERR_ARG, STR_ERR_ALLOC, or STR_ERR_OVERFLOW is recorded on failure.
 */
str_status_t str_set_view(str_t *string, str_view_t source);

/*
 * Appends borrowed NUL-terminated source to non-NULL initialized string. Sticky status propagates.
 * NULL source records STR_ERR_ARG. STR_ERR_ALLOC or STR_ERR_OVERFLOW is recorded.
 */
str_status_t str_append(str_t *string, const char *source);

/*
 * Appends len borrowed bytes at source to non-NULL initialized string. NULL is valid for zero len.
 * A self-source may include the existing terminator, never spare capacity. Existing sticky status
 * propagates. STR_ERR_ARG, STR_ERR_ALLOC, or STR_ERR_OVERFLOW is recorded on failure.
 */
str_status_t str_append_n(str_t *string, const char *source, size_t len);

/*
 * Appends borrowed source to non-NULL initialized string. Existing sticky status propagates.
 * STR_ERR_ARG, STR_ERR_ALLOC, or STR_ERR_OVERFLOW is recorded on failure.
 */
str_status_t str_append_view(str_t *string, str_view_t source);

/*
 * Appends byte to non-NULL initialized string, including NUL.
 * Existing sticky status propagates. STR_ERR_ALLOC or STR_ERR_OVERFLOW is recorded.
 */
str_status_t str_append_char(str_t *string, char byte);

/*
 * Appends printf output from borrowed arguments to non-NULL initialized string.
 * The borrowed format must be non-NULL. The caller retains arguments. The caller remains
 * responsible for va_end.
 * Format plus read-only pointer arguments may borrow from string because output is staged.
 * A borrowed format may begin in current content or at its terminator. A %n destination must not
 * overlap string. Output beyond the internal stack buffer may evaluate the format twice, so a %n
 * destination may receive the same write twice. A %n destination may be modified even if a later
 * allocation or render failure is returned. STR_ERR_ARG, STR_ERR_FMT, STR_ERR_ALLOC, or
 * STR_ERR_OVERFLOW is recorded. Existing sticky status propagates.
 */
str_status_t str_append_vfmt(str_t *string, const char *format, va_list arguments)
    STR_PRINTF_LIKE(2, 0);

/*
 * Appends printf output to non-NULL initialized string. format must not be NULL.
 * Pass untrusted text through a conversion such as "%s", never as format.
 * Staging, borrowing, %n behavior, sticky propagation, plus failure statuses match str_append_vfmt.
 */
str_status_t str_append_fmt(str_t *string, const char *format, ...) STR_PRINTF_LIKE(2, 3);

/*
 * Ensures non-NULL initialized string can hold len content bytes.
 * Existing sticky status propagates.
 * STR_ERR_ALLOC or STR_ERR_OVERFLOW is recorded on failure.
 */
str_status_t str_reserve(str_t *string, size_t len);

/*
 * Minimizes non-NULL initialized string allocation.
 * Existing sticky status propagates. STR_ERR_ALLOC or STR_ERR_OVERFLOW is recorded on failure.
 */
str_status_t str_shrink_to_fit(str_t *string);

/*
 * Resizes non-NULL initialized string to len bytes using fill. Existing sticky status propagates.
 * STR_ERR_ALLOC or STR_ERR_OVERFLOW is recorded on failure.
 */
str_status_t str_resize(str_t *string, size_t len, char fill);

/*
 * Inserts len borrowed bytes from source at idx in non-NULL initialized string. End idx appends.
 * NULL source is valid for zero len. A self-source may include the existing terminator,
 * never spare capacity. STR_ERR_ARG, STR_ERR_RANGE, STR_ERR_ALLOC, or STR_ERR_OVERFLOW is recorded.
 */
str_status_t str_insert_n(str_t *string, size_t idx, const char *source, size_t len);

/*
 * Inserts borrowed source at idx in non-NULL initialized string. Existing sticky status propagates.
 * STR_ERR_ARG, STR_ERR_RANGE, STR_ERR_ALLOC, or STR_ERR_OVERFLOW is recorded on failure.
 */
str_status_t str_insert_view(str_t *string, size_t idx, str_view_t source);

/*
 * Removes len bytes at idx from non-NULL initialized string. Existing sticky status propagates.
 * An outside span records STR_ERR_RANGE.
 */
str_status_t str_remove(str_t *string, size_t idx, size_t len);

/*
 * Replaces remove_len bytes at idx in non-NULL initialized string from borrowed replacement.
 * Replacement may borrow existing content through its terminator, never spare capacity. Failure
 * is atomic. Existing sticky status propagates. STR_ERR_ARG, STR_ERR_RANGE, STR_ERR_ALLOC, or
 * STR_ERR_OVERFLOW is recorded.
 */
str_status_t str_replace_view(str_t *string, size_t idx, size_t remove_len, str_view_t replacement);

/*
 * Returns a NUL-terminated observation pointer. Never NULL.
 * The returned pointer is borrowed until string mutates. NULL string is observed as static empty.
 */
const char *str_cstr(const str_t *string);

/* Returns initialized string's byte length. NULL string has length zero. */
size_t str_len(const str_t *string);

/* Returns content-byte capacity, excluding the terminator. NULL string has capacity zero. */
size_t str_capacity(const str_t *string);

/* True when initialized string has zero length. NULL string is empty. */
bool str_is_empty(const str_t *string);

/* Returns initialized string's sticky status. NULL string returns STR_ERR_ARG. */
str_status_t str_status(const str_t *string);

/* True when initialized string is non-NULL with status STR_OK. */
bool str_ok(const str_t *string);

/*
 * True when string is NULL or its status is not STR_OK.
 */
bool str_failed(const str_t *string);

/*
 * Compares borrowed initialized strings by byte content. A missing operand is non-equal.
 */
bool str_equals(const str_t *string, const str_t *other);

/* Compares initialized string with borrowed NUL-terminated other. NULL on either side is false. */
bool str_equals_cstr(const str_t *string, const char *other);

/* Empty prefix matches. NULL on either side is false. */
bool str_starts_with(const str_t *string, const char *prefix);

/* Empty suffix matches. NULL on either side is false. */
bool str_ends_with(const str_t *string, const char *suffix);

/*
 * Writes the first borrowed needle index in initialized string, or STR_NPOS. Empty needle matches
 * at zero. NULL string, out_idx, or needle returns STR_ERR_ARG without changing out_idx.
 */
str_status_t str_find(const str_t *string, size_t *out_idx, const char *needle);

/*
 * Writes the first byte index in initialized string, or STR_NPOS. NULL string or out_idx returns
 * STR_ERR_ARG without changing out_idx.
 */
str_status_t str_find_char(const str_t *string, size_t *out_idx, char byte);

/* Returns a whole-string borrowed view. Mutation invalidates it. NULL string produces empty. */
str_view_t str_view(const str_t *string);

/*
 * Writes a borrowed len-byte view at offset in initialized string. NULL string or out_view returns
 * STR_ERR_ARG. An outside span returns STR_ERR_RANGE. Failure leaves out_view unchanged.
 */
str_status_t str_slice(const str_t *string, str_view_t *out_view, size_t offset, size_t len);

/* Returns a view borrowing NUL-terminated source. NULL source produces an empty view. */
str_view_t str_view_from_cstr(const char *source);

/* Returns a view borrowing len source bytes. NULL with nonzero len produces an invalid view. */
str_view_t str_view_from_n(const char *source, size_t len);

/* True when view has a non-NULL pointer or zero length; it cannot verify pointed-to storage. */
bool str_view_is_valid(str_view_t view);

/* Byte equality. Invalid views compare false. Two valid empty views compare equal. */
bool str_view_equals(str_view_t left, str_view_t right);

/*
 * Writes unsigned-byte lexical order as -1, 0, or 1. Invalid borrowed views or NULL out_order
 * returns STR_ERR_ARG without changing out_order.
 */
str_status_t str_view_compare(int *out_order, str_view_t left, str_view_t right);

/* Empty prefix matches. Invalid views return false. */
bool str_view_starts_with(str_view_t value, str_view_t prefix);

/* Empty suffix matches. Invalid views return false. */
bool str_view_ends_with(str_view_t value, str_view_t suffix);

/*
 * Writes the first needle index or STR_NPOS. Empty needle matches at zero. Invalid borrowed views
 * or NULL out_idx returns STR_ERR_ARG without changing out_idx.
 */
str_status_t str_view_find(str_view_t haystack, size_t *out_idx, str_view_t needle);

/*
 * Writes the last needle index or STR_NPOS. Empty needle matches at haystack.len. Invalid borrowed
 * views or NULL out_idx returns STR_ERR_ARG without changing out_idx.
 */
str_status_t str_view_rfind(str_view_t haystack, size_t *out_idx, str_view_t needle);

/* Drops ASCII whitespace at both ends. Invalid input produces an empty view. */
str_view_t str_view_trim(str_view_t view);

/*
 * Splits borrowed source on separator into caller-owned split_output storage. parts must not
 * overlap source bytes. Leading, trailing, or consecutive separators produce empty parts. On
 * success, count receives the full result size. Truncation stores the first min(count, cap) parts
 * in source order. Invalid pointers or views return STR_ERR_ARG. Unrepresentable count returns
 * STR_ERR_OVERFLOW. Empty source produces one empty part. A zero-capacity output with NULL parts is
 * a valid count-only query. Failure leaves split_output unchanged.
 */
str_status_t str_split_view(str_view_t source, str_split_out_t *split_output, char separator);

#ifdef STR_TEST
/* Test-only. Fails allocation attempts after success_count successful attempts. */
void str_test_fail_alloc_after(size_t success_count);

/* Test-only. Restores normal allocation behavior. */
void str_test_reset_alloc_failures(void);
#endif

#undef STR_PRINTF_LIKE

#endif /* STR_H */
