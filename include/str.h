#ifndef STR_H
#define STR_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    STR_OK = 0,
    STR_ERR_ARG = -1,
    STR_ERR_ALLOC = -2,
    STR_ERR_RANGE = -3,
    STR_ERR_OVERFLOW = -4,
    STR_ERR_FMT = -5
} str_status_t;

/*
 * Deviation: size_t cannot be an enum constant.
 * Missing-find sentinel, equal to SIZE_MAX.
 */
#define STR_NPOS ((size_t) - 1)

/*
 * Deviation: this guarded extension lets compilers check printf arguments.
 * It expands to nothing on compilers without GNU-style format attributes.
 */
#if defined(__GNUC__) || defined(__clang__)
#define STR_PRINTF_LIKE(format_index, first_arg)                                                   \
    __attribute__((format(printf, format_index, first_arg)))
#else
#define STR_PRINTF_LIKE(format_index, first_arg)
#endif

/*
 * Owned growable byte string. len counts content bytes, including embedded NULs.
 * cap counts allocated bytes, including the trailing NUL. cap == 0 owns no heap memory.
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

/* Caller-owned storage for borrowed split views. count may exceed cap. */
typedef struct {
    str_view_t *parts;
    size_t cap;
    size_t count;
} str_split_out_t;

/*
 * Deviation: a compound initializer cannot be an enum.
 * Valid empty string. Equivalent to zero-initialization.
 */
#define STR_EMPTY                                                                                  \
    {                                                                                              \
        .buf = NULL, .len = 0, .cap = 0, .status = STR_OK                                          \
    }

/* Deviation: never fails. Initializes previously uninitialized storage. NULL is a no-op. */
void str_init(str_t *s);

/* Deviation: never fails. Releases ownership. Safe on NULL or an already deinitialized object. */
void str_deinit(str_t *s);

/* Deviation: never fails. Empties content, retains capacity, then clears sticky status. */
void str_clear(str_t *s);

/* Deviation: never fails. Clears sticky status without changing content. NULL is a no-op. */
void str_clear_error(str_t *s);

/*
 * Deviation: never fails. Transfers ownership and sticky status from s to out.
 * Both objects must be initialized. NULL or identical arguments are a no-op.
 */
void str_move(str_t *out, str_t *s);

/*
 * Deep-copies the preserved bytes in s into initialized out. Source status is not copied.
 * A failed copy preserves out's bytes and records the failure on out.
 */
str_status_t str_copy(str_t *out, const str_t *s);

/*
 * Deviation: returns ownership directly. The caller releases the result with free().
 * Empty content still returns an allocated one-byte C string. Failure returns NULL.
 */
char *str_detach(str_t *s);

/* Stable symbolic name for status. Unknown values return "STR_ERR_UNKNOWN". */
const char *str_status_name(str_status_t status);

/* Replaces content with the C string src. NULL src records STR_ERR_ARG. */
str_status_t str_set(str_t *s, const char *src);

/*
 * Replaces content with len bytes at src. NULL src is valid only when len is zero.
 * A self-source may include the existing terminator, but never spare capacity.
 */
str_status_t str_set_n(str_t *s, const char *src, size_t len);

/* Replaces content with src. Invalid src records STR_ERR_ARG. */
str_status_t str_set_view(str_t *s, str_view_t src);

/* Appends the C string src. NULL src records STR_ERR_ARG. */
str_status_t str_append(str_t *s, const char *src);

/*
 * Appends len bytes at src. NULL src is valid only when len is zero.
 * A self-source may include the existing terminator, but never spare capacity.
 */
str_status_t str_append_n(str_t *s, const char *src, size_t len);

/* Appends src. Invalid src records STR_ERR_ARG. */
str_status_t str_append_view(str_t *s, str_view_t src);

/* Appends one byte, including NUL. */
str_status_t str_append_char(str_t *s, char c);

/*
 * Appends printf output from args. format must not be NULL.
 * Formatting is staged, so format plus read-only pointer arguments may borrow from s.
 * A format borrowed from s may begin in current content or at its existing terminator.
 * A %n destination must not overlap s.
 */
str_status_t str_append_vfmt(str_t *s, const char *format, va_list args) STR_PRINTF_LIKE(2, 0);

/*
 * Appends printf output. format must not be NULL.
 * Pass untrusted text through a conversion such as "%s", never as format.
 * Formatting has the same staging, borrowing, and %n rules as str_append_vfmt.
 */
str_status_t str_append_fmt(str_t *s, const char *format, ...) STR_PRINTF_LIKE(2, 3);

/* Ensures at least len bytes of total content capacity. Never shrinks. */
str_status_t str_reserve(str_t *s, size_t len);

/* Reduces allocation to the minimum for current content. */
str_status_t str_shrink_to_fit(str_t *s);

/* Changes length to len. New bytes receive fill. */
str_status_t str_resize(str_t *s, size_t len, char fill);

/*
 * Inserts len bytes at idx. idx equal to current length appends.
 * Self-sources may include the existing terminator, but never spare capacity.
 */
str_status_t str_insert_n(str_t *s, size_t idx, const char *src, size_t len);

/* Inserts src at idx. Invalid src records STR_ERR_ARG. */
str_status_t str_insert_view(str_t *s, size_t idx, str_view_t src);

/* Removes len bytes at idx. A span outside content records STR_ERR_RANGE. */
str_status_t str_remove(str_t *s, size_t idx, size_t len);

/* Atomically replaces remove_len bytes at idx. A replacement may borrow from s. */
str_status_t str_replace_view(str_t *s, size_t idx, size_t remove_len, str_view_t replacement);

/*
 * Returns a NUL-terminated observation pointer. Never NULL.
 * Deviation: NULL s is observed as empty.
 */
const char *str_cstr(const str_t *s);

/* Byte length. Deviation: NULL s has length zero. */
size_t str_len(const str_t *s);

/* Usable content capacity, excluding the terminator. Deviation: NULL returns zero. */
size_t str_capacity(const str_t *s);

/* True when length is zero. Deviation: NULL s is empty. */
bool str_is_empty(const str_t *s);

/* Sticky status. NULL s is STR_ERR_ARG. */
str_status_t str_status(const str_t *s);

/* True when s is non-NULL with status STR_OK. */
bool str_ok(const str_t *s);

/*
 * Deviation: negated form of str_ok, kept as the established query pair.
 * True when s is NULL or its status is not STR_OK.
 */
bool str_failed(const str_t *s);

/*
 * Deviation: boolean observers return false for a NULL operand instead of a status.
 * The established query API treats a missing operand as non-equal / non-matching.
 */
bool str_equals(const str_t *s, const str_t *other);

/* Byte equality with a C string. NULL on either side is false. */
bool str_equals_cstr(const str_t *s, const char *other);

/* True when s begins with prefix. NULL on either side is false. */
bool str_starts_with(const str_t *s, const char *prefix);

/* True when s ends with suffix. NULL on either side is false. */
bool str_ends_with(const str_t *s, const char *suffix);

/* Writes the first needle index or STR_NPOS. Empty needle matches at zero. */
str_status_t str_find(const str_t *s, size_t *out_idx, const char *needle);

/* Writes the first c index or STR_NPOS. */
str_status_t str_find_char(const str_t *s, size_t *out_idx, char c);

/* Whole-string borrowed view. NULL s produces an empty view. */
str_view_t str_view(const str_t *s);

/* Writes a borrowed content span. A span outside content is STR_ERR_RANGE. */
str_status_t str_slice(const str_t *s, str_view_t *out, size_t off, size_t len);

/* Borrowed view of a C string. NULL src produces an empty view. */
str_view_t str_view_from_cstr(const char *src);

/* Borrowed view of len bytes. NULL with nonzero len produces an invalid view. */
str_view_t str_view_from_n(const char *src, size_t len);

/* True when view has a non-NULL pointer or zero length; it cannot verify pointed-to storage. */
bool str_view_is_valid(str_view_t view);

/* Byte equality. Invalid views compare false. Two valid empty views compare equal. */
bool str_view_equals(str_view_t a, str_view_t b);

/* Writes unsigned-byte lexical order as -1, 0, or 1. Invalid views are STR_ERR_ARG. */
str_status_t str_view_compare(int *out_order, str_view_t a, str_view_t b);

/* True when value begins with prefix. Invalid views return false. */
bool str_view_starts_with(str_view_t value, str_view_t prefix);

/* True when value ends with suffix. Invalid views return false. */
bool str_view_ends_with(str_view_t value, str_view_t suffix);

/*
 * Deviation: output follows hay to preserve the established str_view_find signature.
 * Writes the first needle index or STR_NPOS. Empty needle matches at zero.
 */
str_status_t str_view_find(str_view_t hay, size_t *out_idx, str_view_t needle);

/*
 * Deviation: output follows hay to mirror str_view_find.
 * Writes the last needle index or STR_NPOS. Empty needle matches at hay.len.
 */
str_status_t str_view_rfind(str_view_t hay, size_t *out_idx, str_view_t needle);

/* Drops ASCII whitespace at both ends. Invalid input produces an empty view. */
str_view_t str_view_trim(str_view_t view);

/*
 * Splits src on separator into caller-owned out->parts storage.
 * out->count receives the full count. count greater than cap means truncation.
 * The parts array must not overlap src's bytes.
 */
str_status_t str_split_view(str_view_t src, str_split_out_t *out, char separator);

#ifdef STR_TEST
/* Test-only. Fails allocation attempts after success_count successful attempts. */
void str_test_fail_alloc_after(size_t success_count);

/* Test-only. Restores normal allocation behavior. */
void str_test_reset_alloc_failures(void);
#endif

#undef STR_PRINTF_LIKE

#endif /* STR_H */
