/* main.c: printable walkthrough of the public str API. */

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "str.h"

enum {
    DEMO_REPLACE_IDX = 2,
    DEMO_REPLACE_LEN = 2,
    DEMO_INSERT_IDX = 4,
    DEMO_REMOVE_IDX = 0,
    DEMO_REMOVE_LEN = 2,
    DEMO_SLICE_OFF = 6,
    DEMO_SLICE_LEN = 5,
    DEMO_SPLIT_CAP = 8,
    DEMO_EMBEDDED_LEN = 3,
    DEMO_RESERVE_LEN = 64,
    DEMO_VIEW_PRINT_MAX = 64,
    DEMO_NUM_BUF_LEN = 32,
    DEMO_SEP = ','
};

#define DEMO_HELLO "hello"
#define DEMO_WORLD "world"
#define DEMO_GREETING_FMT ", %s"
#define DEMO_EDIT_START "abcdef"
#define DEMO_REPLACEMENT "XY"
#define DEMO_BANG "!"
#define DEMO_HAY "one fish two fish"
#define DEMO_NEEDLE "fish"
#define DEMO_MISSING "shark"
#define DEMO_PADDED "  hello world  "
#define DEMO_SPLIT_SRC "red,green,,blue"
#define DEMO_OWNED "owned"
#define DEMO_KEEP "keep"
#define DEMO_SHORT "hi"
#define DEMO_EMPTY_MARK "(empty)"
#define DEMO_YES "yes"
#define DEMO_NO "no"
#define DEMO_NPOS_NAME "STR_NPOS"

/* Propagates any non-OK status. Permitted only in functions that acquire nothing. */
#define DEMO_TRY(expr)                                                                             \
    do {                                                                                           \
        str_status_t demo_try_s_ = (expr);                                                         \
        if (demo_try_s_ != STR_OK)                                                                 \
            return demo_try_s_;                                                                    \
    } while (0)

/* Maps status to a process exit code. Writes failures to stderr. */
static int demo_exit(str_status_t status);

/* Runs every printable scene. */
static str_status_t demo_run(void);

/* Prints an append-built greeting. */
static str_status_t demo_build(void);

/* Prints in-place edit results. */
static str_status_t demo_edit(void);

/* Prints an overlap-safe self-append. */
static str_status_t demo_overlap(void);

/* Prints substring match indexes. */
static str_status_t demo_search(void);

/* Prints borrowed-view queries. */
static str_status_t demo_views(void);

/* Prints a comma-separated split. */
static str_status_t demo_split(void);

/* Prints ownership transfer results. */
static str_status_t demo_ownership(void);

/* Prints a deep copy of one string. */
static str_status_t demo_copy(void);

/* Prints a move that empties the source. */
static str_status_t demo_move(void);

/* Prints a heap buffer detached for free(). */
static str_status_t demo_detach(void);

/* Prints sticky-error recovery. */
static str_status_t demo_sticky(void);

/* Prints a payload that contains an embedded NUL. */
static str_status_t demo_binary(void);

/* Prints heap capacity changes. */
static str_status_t demo_capacity(void);

/* Releases s. Returns status. */
static str_status_t demo_release(str_t *s, str_status_t status);

/* Releases two initialized strings. Returns status. */
static str_status_t demo_release_pair(str_t *a, str_t *b, str_status_t status);

/* Prints s under key, or s's sticky status. */
static str_status_t demo_emit(str_t *s, const char *key);

/* Prints the first match index of needle. */
static str_status_t demo_emit_find(const str_t *s, const char *key, const char *needle);

/* Prints the last match index of needle. */
static str_status_t demo_emit_rfind(const str_t *s, const char *key, const char *needle);

/* Writes a section heading to stdout. */
static str_status_t demo_print_section(const char *title);

/* Writes a key/value line to stdout. */
static str_status_t demo_print_kv(const char *key, const char *value);

/* Writes a size_t value as a key/value line. */
static str_status_t demo_print_size(const char *key, size_t value);

/* Writes a search index, using STR_NPOS for a miss. */
static str_status_t demo_print_idx(const char *key, size_t idx);

/* Writes yes or no for a boolean. */
static str_status_t demo_print_bool(const char *key, bool value);

/* Writes a status name. */
static str_status_t demo_print_status(const char *key, str_status_t status);

/* Writes a borrowed span as a key/value line. */
static str_status_t demo_print_span(const char *key, str_view_t view);

/* Writes one split part. */
static str_status_t demo_print_part(size_t idx, str_view_t part);

/* Writes DEMO_EMBEDDED_LEN bytes as hex. */
static str_status_t demo_print_embedded_hex(const char *key, const char *bytes);

int main(void)
{
    return demo_exit(demo_run());
}

static int demo_exit(str_status_t status)
{
    if (status == STR_OK)
        return 0;

    int wrote = fprintf(stderr, "%s\n", str_status_name(status));
    if (wrote < 0)
        return 1;
    return 1;
}

static str_status_t demo_run(void)
{
    DEMO_TRY(demo_build());
    DEMO_TRY(demo_edit());
    DEMO_TRY(demo_overlap());
    DEMO_TRY(demo_search());
    DEMO_TRY(demo_views());
    DEMO_TRY(demo_split());
    DEMO_TRY(demo_ownership());
    DEMO_TRY(demo_sticky());
    DEMO_TRY(demo_binary());
    DEMO_TRY(demo_capacity());
    return STR_OK;
}

static str_status_t demo_build(void)
{
    str_t msg = STR_EMPTY;
    str_status_t status = demo_print_section("build");

    if (status != STR_OK)
        return demo_release(&msg, status);

    str_append(&msg, DEMO_HELLO);
    str_append_fmt(&msg, DEMO_GREETING_FMT, DEMO_WORLD);
    status = demo_emit(&msg, "text");
    if (status != STR_OK)
        return demo_release(&msg, status);

    status = demo_print_size("len", str_len(&msg));
    return demo_release(&msg, status);
}

static str_status_t demo_edit(void)
{
    str_t s = STR_EMPTY;
    str_status_t status = demo_print_section("edit");

    if (status != STR_OK)
        return demo_release(&s, status);

    str_set(&s, DEMO_EDIT_START);
    status = demo_emit(&s, "start");
    if (status != STR_OK)
        return demo_release(&s, status);

    str_replace_view(&s, DEMO_REPLACE_IDX, DEMO_REPLACE_LEN, str_view_from_cstr(DEMO_REPLACEMENT));
    status = demo_emit(&s, "replaced");
    if (status != STR_OK)
        return demo_release(&s, status);

    str_insert_n(&s, DEMO_INSERT_IDX, DEMO_BANG, sizeof(DEMO_BANG) - 1);
    status = demo_emit(&s, "inserted");
    if (status != STR_OK)
        return demo_release(&s, status);

    str_remove(&s, DEMO_REMOVE_IDX, DEMO_REMOVE_LEN);
    status = demo_emit(&s, "removed");
    return demo_release(&s, status);
}

static str_status_t demo_overlap(void)
{
    str_t s = STR_EMPTY;
    str_status_t status = demo_print_section("overlap");

    if (status != STR_OK)
        return demo_release(&s, status);

    str_set(&s, DEMO_HELLO);
    status = demo_emit(&s, "base");
    if (status != STR_OK)
        return demo_release(&s, status);

    str_append_n(&s, str_cstr(&s), str_len(&s));
    status = demo_emit(&s, "self-append");
    return demo_release(&s, status);
}

static str_status_t demo_search(void)
{
    str_t s = STR_EMPTY;
    str_status_t status = demo_print_section("search");

    if (status != STR_OK)
        return demo_release(&s, status);

    str_set(&s, DEMO_HAY);
    status = demo_emit(&s, "hay");
    if (status != STR_OK)
        return demo_release(&s, status);

    status = demo_emit_find(&s, "first", DEMO_NEEDLE);
    if (status != STR_OK)
        return demo_release(&s, status);
    status = demo_emit_rfind(&s, "last", DEMO_NEEDLE);
    if (status != STR_OK)
        return demo_release(&s, status);
    status = demo_emit_find(&s, "missing", DEMO_MISSING);
    return demo_release(&s, status);
}

static str_status_t demo_views(void)
{
    str_t s = STR_EMPTY;
    str_status_t status = demo_print_section("views");

    if (status != STR_OK)
        return demo_release(&s, status);

    status = demo_print_kv("raw", DEMO_PADDED);
    if (status != STR_OK)
        return demo_release(&s, status);

    str_set_view(&s, str_view_trim(str_view_from_cstr(DEMO_PADDED)));
    status = demo_emit(&s, "trim");
    if (status != STR_OK)
        return demo_release(&s, status);

    str_view_t slice = {0};
    status = str_slice(&s, &slice, DEMO_SLICE_OFF, DEMO_SLICE_LEN);
    if (status != STR_OK)
        return demo_release(&s, status);
    status = demo_print_span("slice", slice);
    if (status != STR_OK)
        return demo_release(&s, status);

    status = demo_print_bool("starts_with hello", str_starts_with(&s, DEMO_HELLO));
    if (status != STR_OK)
        return demo_release(&s, status);
    status = demo_print_bool("ends_with world", str_ends_with(&s, DEMO_WORLD));
    return demo_release(&s, status);
}

static str_status_t demo_split(void)
{
    str_view_t parts[DEMO_SPLIT_CAP];
    str_split_out_t out = {.parts = parts, .cap = DEMO_SPLIT_CAP, .count = 0};
    str_view_t src = str_view_from_cstr(DEMO_SPLIT_SRC);

    DEMO_TRY(demo_print_section("split"));
    DEMO_TRY(demo_print_kv("src", DEMO_SPLIT_SRC));
    DEMO_TRY(str_split_view(src, &out, (char)DEMO_SEP));
    DEMO_TRY(demo_print_size("count", out.count));

    for (size_t idx = 0; idx < (size_t)DEMO_SPLIT_CAP; idx++) {
        if (idx >= out.count)
            break;
        DEMO_TRY(demo_print_part(idx, parts[idx]));
    }
    return STR_OK;
}

static str_status_t demo_ownership(void)
{
    DEMO_TRY(demo_print_section("ownership"));
    DEMO_TRY(demo_copy());
    DEMO_TRY(demo_move());
    DEMO_TRY(demo_detach());
    return STR_OK;
}

static str_status_t demo_copy(void)
{
    str_t src = STR_EMPTY;
    str_t dst = STR_EMPTY;

    str_set(&src, DEMO_OWNED);
    if (str_failed(&src))
        return demo_release_pair(&src, &dst, str_status(&src));

    str_status_t status = str_copy(&dst, &src);
    if (status != STR_OK)
        return demo_release_pair(&src, &dst, status);

    status = demo_emit(&src, "src");
    if (status != STR_OK)
        return demo_release_pair(&src, &dst, status);
    status = demo_emit(&dst, "copy");
    if (status != STR_OK)
        return demo_release_pair(&src, &dst, status);

    status = demo_print_bool("equals", str_equals(&src, &dst));
    return demo_release_pair(&src, &dst, status);
}

static str_status_t demo_move(void)
{
    str_t src = STR_EMPTY;
    str_t dst = STR_EMPTY;

    str_set(&src, DEMO_OWNED);
    if (str_failed(&src))
        return demo_release_pair(&src, &dst, str_status(&src));

    str_move(&dst, &src);

    str_status_t status = demo_emit(&dst, "moved");
    if (status != STR_OK)
        return demo_release_pair(&src, &dst, status);
    status = demo_emit(&src, "src after move");
    return demo_release_pair(&src, &dst, status);
}

static str_status_t demo_detach(void)
{
    str_t s = STR_EMPTY;

    str_set(&s, DEMO_OWNED);
    if (str_failed(&s))
        return demo_release(&s, str_status(&s));

    char *owned = str_detach(&s);
    if (owned == NULL)
        return demo_release(&s, str_status(&s));

    str_deinit(&s);
    str_status_t status = demo_print_kv("detached", owned);
    free(owned);
    return status;
}

static str_status_t demo_sticky(void)
{
    str_t s = STR_EMPTY;
    str_status_t status = demo_print_section("sticky");

    if (status != STR_OK)
        return demo_release(&s, status);

    str_set(&s, DEMO_KEEP);
    status = demo_emit(&s, "content");
    if (status != STR_OK)
        return demo_release(&s, status);

    str_status_t removed = str_remove(&s, str_len(&s), 1);
    status = demo_print_status("bad remove", removed);
    if (status != STR_OK)
        return demo_release(&s, status);

    str_append(&s, DEMO_BANG);
    status = demo_print_kv("blocked append", str_cstr(&s));
    if (status != STR_OK)
        return demo_release(&s, status);
    status = demo_print_status("sticky", str_status(&s));
    if (status != STR_OK)
        return demo_release(&s, status);

    str_clear_error(&s);
    str_append(&s, DEMO_BANG);
    status = demo_emit(&s, "after clear_error");
    return demo_release(&s, status);
}

static str_status_t demo_binary(void)
{
    const char raw[DEMO_EMBEDDED_LEN] = {'a', '\0', 'b'};
    str_t s = STR_EMPTY;
    str_status_t status = demo_print_section("binary");

    if (status != STR_OK)
        return demo_release(&s, status);

    str_set_n(&s, raw, (size_t)DEMO_EMBEDDED_LEN);
    if (str_failed(&s))
        return demo_release(&s, str_status(&s));

    status = demo_print_size("len", str_len(&s));
    if (status != STR_OK)
        return demo_release(&s, status);
    status = demo_print_embedded_hex("hex", str_cstr(&s));
    if (status != STR_OK)
        return demo_release(&s, status);
    status = demo_print_kv("cstr", str_cstr(&s));
    return demo_release(&s, status);
}

static str_status_t demo_capacity(void)
{
    str_t s = STR_EMPTY;
    str_status_t status = demo_print_section("capacity");

    if (status != STR_OK)
        return demo_release(&s, status);

    status = demo_print_size("requested", (size_t)DEMO_RESERVE_LEN);
    if (status != STR_OK)
        return demo_release(&s, status);

    str_reserve(&s, (size_t)DEMO_RESERVE_LEN);
    if (str_failed(&s))
        return demo_release(&s, str_status(&s));
    status = demo_print_size("reserved", str_capacity(&s));
    if (status != STR_OK)
        return demo_release(&s, status);

    str_set(&s, DEMO_SHORT);
    str_shrink_to_fit(&s);
    if (str_failed(&s))
        return demo_release(&s, str_status(&s));
    status = demo_print_size("after shrink", str_capacity(&s));
    return demo_release(&s, status);
}

static str_status_t demo_release(str_t *s, str_status_t status)
{
    assert(s != NULL);
    str_deinit(s);
    return status;
}

static str_status_t demo_release_pair(str_t *a, str_t *b, str_status_t status)
{
    assert(a != NULL);
    assert(b != NULL);
    str_deinit(a);
    str_deinit(b);
    return status;
}

static str_status_t demo_emit(str_t *s, const char *key)
{
    assert(s != NULL);
    assert(key != NULL);
    if (str_failed(s))
        return str_status(s);
    if (str_is_empty(s))
        return demo_print_kv(key, DEMO_EMPTY_MARK);
    return demo_print_kv(key, str_cstr(s));
}

static str_status_t demo_emit_find(const str_t *s, const char *key, const char *needle)
{
    size_t idx = STR_NPOS;
    str_status_t status = str_find(s, &idx, needle);

    if (status != STR_OK)
        return status;
    return demo_print_idx(key, idx);
}

static str_status_t demo_emit_rfind(const str_t *s, const char *key, const char *needle)
{
    size_t idx = STR_NPOS;
    str_status_t status = str_view_rfind(str_view(s), &idx, str_view_from_cstr(needle));

    if (status != STR_OK)
        return status;
    return demo_print_idx(key, idx);
}

/*
 * Deviation: stdout write failures reuse STR_ERR_FMT.
 * The demo has no I/O status of its own.
 */
static str_status_t demo_print_section(const char *title)
{
    assert(title != NULL);

    int wrote = printf("\n== %s\n", title);
    if (wrote < 0)
        return STR_ERR_FMT;
    return STR_OK;
}

static str_status_t demo_print_kv(const char *key, const char *value)
{
    assert(key != NULL);
    assert(value != NULL);

    int wrote = printf("%s: %s\n", key, value);
    if (wrote < 0)
        return STR_ERR_FMT;
    return STR_OK;
}

static str_status_t demo_print_size(const char *key, size_t value)
{
    char buf[DEMO_NUM_BUF_LEN];

    assert(key != NULL);

    int n = snprintf(buf, sizeof(buf), "%zu", value);
    if (n < 0 || n >= (int)DEMO_NUM_BUF_LEN)
        return STR_ERR_FMT;
    return demo_print_kv(key, buf);
}

static str_status_t demo_print_idx(const char *key, size_t idx)
{
    if (idx == STR_NPOS)
        return demo_print_kv(key, DEMO_NPOS_NAME);
    return demo_print_size(key, idx);
}

static str_status_t demo_print_bool(const char *key, bool value)
{
    return demo_print_kv(key, value ? DEMO_YES : DEMO_NO);
}

static str_status_t demo_print_status(const char *key, str_status_t status)
{
    return demo_print_kv(key, str_status_name(status));
}

static str_status_t demo_print_span(const char *key, str_view_t view)
{
    assert(key != NULL);
    assert(str_view_is_valid(view));
    assert(view.len <= (size_t)DEMO_VIEW_PRINT_MAX);
    if (view.len == 0)
        return demo_print_kv(key, DEMO_EMPTY_MARK);

    int wrote = printf("%s: %.*s\n", key, (int)view.len, view.ptr);
    if (wrote < 0)
        return STR_ERR_FMT;
    return STR_OK;
}

static str_status_t demo_print_part(size_t idx, str_view_t part)
{
    char key[DEMO_NUM_BUF_LEN];
    int n = snprintf(key, sizeof(key), "part[%zu]", idx);

    if (n < 0 || n >= (int)DEMO_NUM_BUF_LEN)
        return STR_ERR_FMT;
    return demo_print_span(key, part);
}

static str_status_t demo_print_embedded_hex(const char *key, const char *bytes)
{
    _Static_assert(DEMO_EMBEDDED_LEN == 3, "this printer writes three bytes");
    assert(key != NULL);
    assert(bytes != NULL);

    int wrote = printf("%s: %02x %02x %02x\n", key, (unsigned char)bytes[0],
                       (unsigned char)bytes[1], (unsigned char)bytes[2]);
    if (wrote < 0)
        return STR_ERR_FMT;
    return STR_OK;
}
