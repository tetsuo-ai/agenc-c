/* main.c: printable walkthrough of the public str API. */

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "str.h"

enum {
    DEMO_REPLACE_INDEX = 2,
    DEMO_REPLACE_LENGTH = 2,
    DEMO_INSERT_INDEX = 4,
    DEMO_REMOVE_INDEX = 0,
    DEMO_REMOVE_LENGTH = 2,
    DEMO_SLICE_OFFSET = 6,
    DEMO_SLICE_LENGTH = 5,
    DEMO_SPLIT_CAPACITY = 8,
    DEMO_EMBEDDED_FIRST_INDEX = 0,
    DEMO_EMBEDDED_SECOND_INDEX = 1,
    DEMO_EMBEDDED_THIRD_INDEX = 2,
    DEMO_EMBEDDED_BYTES = DEMO_EMBEDDED_THIRD_INDEX + 1,
    DEMO_RESERVE_CONTENT_BYTES = 64,
    DEMO_NUMBER_BUFFER_BYTES = 32,
    DEMO_SEPARATOR = ','
};

#define DEMO_HELLO "hello"
#define DEMO_WORLD "world"
#define DEMO_GREETING_FORMAT ", %s"
#define DEMO_EDIT_SOURCE "abcdef"
#define DEMO_REPLACEMENT_TEXT "XY"
#define DEMO_EXCLAMATION "!"
#define DEMO_HAYSTACK "one fish two fish"
#define DEMO_NEEDLE "fish"
#define DEMO_MISSING "shark"
#define DEMO_PADDED_TEXT "  hello world  "
#define DEMO_SPLIT_SOURCE "red,green,,blue"
#define DEMO_OWNED_TEXT "owned"
#define DEMO_PRESERVED_TEXT "keep"
#define DEMO_SHORT_TEXT "hi"
#define DEMO_EMPTY_MARK "(empty)"
#define DEMO_YES "yes"
#define DEMO_NO "no"
#define DEMO_NPOS_NAME "STR_NPOS"
#define DEMO_LENGTH_KEY "len"
#define DEMO_SOURCE_KEY "src"
#define DEMO_STICKY_KEY "sticky"

typedef enum { DEMO_MATCH_FIRST, DEMO_MATCH_LAST } demo_match_mode_t;

/* key plus needle are borrowed non-NULL strings. */
typedef struct {
    const char *key;
    const char *needle;
    demo_match_mode_t mode;
} demo_match_request_t;

typedef str_status_t (*demo_scene_fn_t)(void);

/* Writes non-OK status to stderr, then returns EXIT_FAILURE. */
static int demo_report_failure(str_status_t status);

/* Flushes standard output. Fails with STR_ERR_FMT. */
static str_status_t demo_flush_output(void);

/* Runs every printable scene. Propagates the first scene status. */
static str_status_t demo_run(void);

/* Prints an append-built greeting. Propagates library or presentation status. */
static str_status_t demo_build(void);

/* Prints in-place edit results. Propagates library or presentation status. */
static str_status_t demo_edit(void);

/* Prints an overlap-safe self-append. Propagates library or presentation status. */
static str_status_t demo_overlap(void);

/* Prints substring match indexes. Propagates library or presentation status. */
static str_status_t demo_search(void);

/* Prints borrowed-view queries. Propagates library or presentation status. */
static str_status_t demo_views(void);

/* Prints a comma-separated split. Propagates library or presentation status. */
static str_status_t demo_split(void);

/* Prints stored parts from non-NULL borrowed split_output. Propagates STR_ERR_FMT. */
static str_status_t demo_print_split_parts(const str_split_out_t *split_output);

/* Prints ownership transfer results. Propagates library or presentation status. */
static str_status_t demo_ownership(void);

/* Prints a deep copy of one string. Propagates library or presentation status. */
static str_status_t demo_copy(void);

/* Prints a move that empties the source. Propagates library or presentation status. */
static str_status_t demo_move(void);

/* Prints a heap buffer detached for free(). Propagates library or presentation status. */
static str_status_t demo_detach(void);

/* Prints sticky-error recovery. Propagates library or presentation status. */
static str_status_t demo_sticky(void);

/* Demonstrates sticky rejection on non-NULL initialized string. Propagates presentation status. */
static str_status_t demo_demonstrate_sticky_failure(str_t *string);

/* Prints a payload that contains an embedded NUL. Propagates library or presentation status. */
static str_status_t demo_binary(void);

/* Prints heap capacity changes. Propagates library or presentation status. */
static str_status_t demo_capacity(void);

/* Releases non-NULL initialized string. Returns status unchanged. */
static str_status_t demo_release(str_t *string, str_status_t status);

/* Releases non-NULL initialized first plus second. Returns status unchanged. */
static str_status_t demo_release_pair(str_t *first, str_t *second, str_status_t status);

/* Releases caller-owned buffer through free(). NULL is accepted. */
static void demo_release_buffer(char *buffer);

/* Prints non-NULL borrowed string under non-NULL borrowed key. Propagates sticky or I/O status. */
static str_status_t demo_print_string(const str_t *string, const char *key);

/* Prints request's match in non-NULL borrowed string. Pointers in request must be non-NULL. */
static str_status_t demo_print_match(const str_t *string, demo_match_request_t request);

/* Writes non-NULL borrowed title as a section heading. Fails with STR_ERR_FMT. */
static str_status_t demo_print_section(const char *title);

/* Writes borrowed non-NULL key plus value. Fails with STR_ERR_FMT. */
static str_status_t demo_print_key_value(const char *key, const char *value);

/* Writes value under non-NULL borrowed key. Fails with STR_ERR_FMT. */
static str_status_t demo_print_size(const char *key, size_t value);

/* Writes idx under non-NULL borrowed key, using STR_NPOS for a miss. Fails with STR_ERR_FMT. */
static str_status_t demo_print_index(const char *key, size_t idx);

/* Writes yes or no under non-NULL borrowed key. Fails with STR_ERR_FMT. */
static str_status_t demo_print_bool(const char *key, bool value);

/* Writes status name under non-NULL borrowed key. Fails with STR_ERR_FMT. */
static str_status_t demo_print_status(const char *key, str_status_t status);

/* Writes valid borrowed view under non-NULL borrowed key. Fails with STR_ERR_FMT. */
static str_status_t demo_print_span(const char *key, str_view_t view);

/* Writes non-NULL borrowed key followed by a separator. Fails with STR_ERR_FMT. */
static str_status_t demo_print_span_prefix(const char *key);

/* Writes nonempty valid borrowed view bytes. Fails with STR_ERR_FMT. */
static str_status_t demo_write_view_bytes(str_view_t view);

/* Writes one newline. Fails with STR_ERR_FMT. */
static str_status_t demo_print_newline(void);

/* Writes one valid borrowed split part. Fails with STR_ERR_FMT. */
static str_status_t demo_print_part(size_t idx, str_view_t part);

/* Writes value as decimal text into non-NULL out_buffer. Fails with STR_ERR_FMT. */
static str_status_t demo_format_size_value(char *out_buffer, size_t capacity, size_t value);

/* Writes a part key into non-NULL out_buffer. Fails with STR_ERR_FMT. */
static str_status_t demo_format_part_key(char *out_buffer, size_t capacity, size_t idx);

/* Writes the first DEMO_EMBEDDED_BYTES of valid borrowed view as hex. Fails with STR_ERR_FMT. */
static str_status_t demo_print_embedded_hex(const char *key, str_view_t view);

int main(void)
{
    str_status_t status = demo_run();
    str_status_t flush_status = demo_flush_output();
    if (status == STR_OK) {
        status = flush_status;
    }
    if (status == STR_OK) {
        return EXIT_SUCCESS;
    }
    return demo_report_failure(status);
}

static int demo_report_failure(str_status_t status)
{
    assert(status != STR_OK);

    /* The format is fixed and the result is checked. */
    /* NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling) */
    int written = fprintf(stderr, "%s\n", str_status_name(status));
    if (written < 0) {
        return EXIT_FAILURE;
    }
    return EXIT_FAILURE;
}

static str_status_t demo_flush_output(void)
{
    if (fflush(stdout) == EOF) {
        return STR_ERR_FMT;
    }
    return STR_OK;
}

static str_status_t demo_run(void)
{
    static const demo_scene_fn_t demo_scenes[] = {
        demo_build, demo_edit,      demo_overlap, demo_search, demo_views,
        demo_split, demo_ownership, demo_sticky,  demo_binary, demo_capacity,
    };
    size_t scene_count = sizeof(demo_scenes) / sizeof(demo_scenes[0]);

    for (size_t idx = 0; idx < scene_count; idx++) {
        str_status_t status = demo_scenes[idx]();
        if (status != STR_OK) {
            return status;
        }
    }
    return STR_OK;
}

static str_status_t demo_build(void)
{
    str_t message = STR_EMPTY;
    str_status_t status = demo_print_section("build");

    if (status != STR_OK) {
        return demo_release(&message, status);
    }

    status = str_append(&message, DEMO_HELLO);
    if (status != STR_OK) {
        return demo_release(&message, status);
    }
    status = str_append_fmt(&message, DEMO_GREETING_FORMAT, DEMO_WORLD);
    if (status != STR_OK) {
        return demo_release(&message, status);
    }
    status = demo_print_string(&message, "text");
    if (status != STR_OK) {
        return demo_release(&message, status);
    }

    status = demo_print_size(DEMO_LENGTH_KEY, str_len(&message));
    return demo_release(&message, status);
}

static str_status_t demo_edit(void)
{
    str_t string = STR_EMPTY;
    str_status_t status = demo_print_section("edit");

    if (status != STR_OK) {
        return demo_release(&string, status);
    }

    status = str_set(&string, DEMO_EDIT_SOURCE);
    if (status != STR_OK) {
        return demo_release(&string, status);
    }
    status = demo_print_string(&string, "start");
    if (status != STR_OK) {
        return demo_release(&string, status);
    }

    status = str_replace_view(&string, DEMO_REPLACE_INDEX, DEMO_REPLACE_LENGTH,
                              str_view_from_cstr(DEMO_REPLACEMENT_TEXT));
    if (status != STR_OK) {
        return demo_release(&string, status);
    }
    status = demo_print_string(&string, "replaced");
    if (status != STR_OK) {
        return demo_release(&string, status);
    }

    status =
        str_insert_n(&string, DEMO_INSERT_INDEX, DEMO_EXCLAMATION, sizeof(DEMO_EXCLAMATION) - 1);
    if (status != STR_OK) {
        return demo_release(&string, status);
    }
    status = demo_print_string(&string, "inserted");
    if (status != STR_OK) {
        return demo_release(&string, status);
    }

    status = str_remove(&string, DEMO_REMOVE_INDEX, DEMO_REMOVE_LENGTH);
    if (status != STR_OK) {
        return demo_release(&string, status);
    }
    status = demo_print_string(&string, "removed");
    return demo_release(&string, status);
}

static str_status_t demo_overlap(void)
{
    str_t string = STR_EMPTY;
    str_status_t status = demo_print_section("overlap");

    if (status != STR_OK) {
        return demo_release(&string, status);
    }

    status = str_set(&string, DEMO_HELLO);
    if (status != STR_OK) {
        return demo_release(&string, status);
    }
    status = demo_print_string(&string, "base");
    if (status != STR_OK) {
        return demo_release(&string, status);
    }

    status = str_append_n(&string, str_cstr(&string), str_len(&string));
    if (status != STR_OK) {
        return demo_release(&string, status);
    }
    status = demo_print_string(&string, "self-append");
    return demo_release(&string, status);
}

static str_status_t demo_search(void)
{
    str_t string = STR_EMPTY;
    str_status_t status = demo_print_section("search");

    if (status != STR_OK) {
        return demo_release(&string, status);
    }

    status = str_set(&string, DEMO_HAYSTACK);
    if (status != STR_OK) {
        return demo_release(&string, status);
    }
    status = demo_print_string(&string, "hay");
    if (status != STR_OK) {
        return demo_release(&string, status);
    }

    status = demo_print_match(&string, (demo_match_request_t){
                                           .key = "first",
                                           .needle = DEMO_NEEDLE,
                                           .mode = DEMO_MATCH_FIRST,
                                       });
    if (status != STR_OK) {
        return demo_release(&string, status);
    }
    status = demo_print_match(&string, (demo_match_request_t){
                                           .key = "last",
                                           .needle = DEMO_NEEDLE,
                                           .mode = DEMO_MATCH_LAST,
                                       });
    if (status != STR_OK) {
        return demo_release(&string, status);
    }
    status = demo_print_match(&string, (demo_match_request_t){
                                           .key = "missing",
                                           .needle = DEMO_MISSING,
                                           .mode = DEMO_MATCH_FIRST,
                                       });
    return demo_release(&string, status);
}

static str_status_t demo_views(void)
{
    str_t string = STR_EMPTY;
    str_status_t status = demo_print_section("views");

    if (status != STR_OK) {
        return demo_release(&string, status);
    }

    status = demo_print_key_value("raw", DEMO_PADDED_TEXT);
    if (status != STR_OK) {
        return demo_release(&string, status);
    }

    status = str_set_view(&string, str_view_trim(str_view_from_cstr(DEMO_PADDED_TEXT)));
    if (status != STR_OK) {
        return demo_release(&string, status);
    }
    status = demo_print_string(&string, "trim");
    if (status != STR_OK) {
        return demo_release(&string, status);
    }

    str_view_t slice = {.ptr = NULL, .len = 0};
    status = str_slice(&string, &slice, DEMO_SLICE_OFFSET, DEMO_SLICE_LENGTH);
    if (status != STR_OK) {
        return demo_release(&string, status);
    }
    status = demo_print_span("slice", slice);
    if (status != STR_OK) {
        return demo_release(&string, status);
    }

    status = demo_print_bool("starts_with hello", str_starts_with(&string, DEMO_HELLO));
    if (status != STR_OK) {
        return demo_release(&string, status);
    }
    status = demo_print_bool("ends_with world", str_ends_with(&string, DEMO_WORLD));
    return demo_release(&string, status);
}

static str_status_t demo_split(void)
{
    str_view_t parts[DEMO_SPLIT_CAPACITY] = {0};
    str_split_out_t split_output = {
        .parts = parts,
        .cap = DEMO_SPLIT_CAPACITY,
        .count = 0,
    };
    str_view_t source = str_view_from_cstr(DEMO_SPLIT_SOURCE);

    str_status_t status = demo_print_section("split");
    if (status != STR_OK) {
        return status;
    }
    status = demo_print_key_value(DEMO_SOURCE_KEY, DEMO_SPLIT_SOURCE);
    if (status != STR_OK) {
        return status;
    }
    status = str_split_view(source, &split_output, (char)DEMO_SEPARATOR);
    if (status != STR_OK) {
        return status;
    }
    status = demo_print_size("count", split_output.count);
    if (status != STR_OK) {
        return status;
    }
    return demo_print_split_parts(&split_output);
}

static str_status_t demo_print_split_parts(const str_split_out_t *split_output)
{
    assert(split_output != NULL);
    assert(split_output->cap == 0 || split_output->parts != NULL);

    size_t stored_count = split_output->count;
    if (stored_count > split_output->cap) {
        stored_count = split_output->cap;
    }
    for (size_t idx = 0; idx < stored_count; idx++) {
        str_status_t status = demo_print_part(idx, split_output->parts[idx]);
        if (status != STR_OK) {
            return status;
        }
    }
    return STR_OK;
}

static str_status_t demo_ownership(void)
{
    static const demo_scene_fn_t demo_ownership_scenes[] = {
        demo_copy,
        demo_move,
        demo_detach,
    };
    str_status_t status = demo_print_section("ownership");
    if (status != STR_OK) {
        return status;
    }

    size_t scene_count = sizeof(demo_ownership_scenes) / sizeof(demo_ownership_scenes[0]);
    for (size_t idx = 0; idx < scene_count; idx++) {
        status = demo_ownership_scenes[idx]();
        if (status != STR_OK) {
            return status;
        }
    }
    return STR_OK;
}

static str_status_t demo_copy(void)
{
    str_t source = STR_EMPTY;
    str_t destination = STR_EMPTY;

    str_status_t status = str_set(&source, DEMO_OWNED_TEXT);
    if (status != STR_OK) {
        return demo_release_pair(&source, &destination, status);
    }

    status = str_copy(&destination, &source);
    if (status != STR_OK) {
        return demo_release_pair(&source, &destination, status);
    }

    status = demo_print_string(&source, DEMO_SOURCE_KEY);
    if (status != STR_OK) {
        return demo_release_pair(&source, &destination, status);
    }
    status = demo_print_string(&destination, "copy");
    if (status != STR_OK) {
        return demo_release_pair(&source, &destination, status);
    }

    status = demo_print_bool("equals", str_equals(&source, &destination));
    return demo_release_pair(&source, &destination, status);
}

static str_status_t demo_move(void)
{
    str_t source = STR_EMPTY;
    str_t destination = STR_EMPTY;

    str_status_t status = str_set(&source, DEMO_OWNED_TEXT);
    if (status != STR_OK) {
        return demo_release_pair(&source, &destination, status);
    }

    str_move(&destination, &source);

    status = demo_print_string(&destination, "moved");
    if (status != STR_OK) {
        return demo_release_pair(&source, &destination, status);
    }
    status = demo_print_string(&source, "src after move");
    return demo_release_pair(&source, &destination, status);
}

static str_status_t demo_detach(void)
{
    str_t string = STR_EMPTY;

    str_status_t status = str_set(&string, DEMO_OWNED_TEXT);
    if (status != STR_OK) {
        return demo_release(&string, status);
    }

    char *owned_buffer = str_detach(&string);
    if (owned_buffer == NULL) {
        return demo_release(&string, str_status(&string));
    }

    status = demo_print_key_value("detached", owned_buffer);
    demo_release_buffer(owned_buffer);
    return demo_release(&string, status);
}

static str_status_t demo_sticky(void)
{
    str_t string = STR_EMPTY;
    str_status_t status = demo_print_section(DEMO_STICKY_KEY);

    if (status != STR_OK) {
        return demo_release(&string, status);
    }

    status = str_set(&string, DEMO_PRESERVED_TEXT);
    if (status != STR_OK) {
        return demo_release(&string, status);
    }
    status = demo_print_string(&string, "content");
    if (status != STR_OK) {
        return demo_release(&string, status);
    }

    status = demo_demonstrate_sticky_failure(&string);
    if (status != STR_OK) {
        return demo_release(&string, status);
    }

    str_clear_error(&string);
    status = str_append(&string, DEMO_EXCLAMATION);
    if (status != STR_OK) {
        return demo_release(&string, status);
    }
    status = demo_print_string(&string, "after clear_error");
    return demo_release(&string, status);
}

static str_status_t demo_demonstrate_sticky_failure(str_t *string)
{
    assert(string != NULL);
    assert(str_ok(string));

    str_status_t remove_status = str_remove(string, str_len(string), 1);
    if (remove_status == STR_OK) {
        return STR_ERR_FMT;
    }
    if (remove_status != STR_ERR_RANGE) {
        return remove_status;
    }
    str_status_t status = demo_print_status("bad remove", remove_status);
    if (status != STR_OK) {
        return status;
    }

    str_status_t blocked_status = str_append(string, DEMO_EXCLAMATION);
    if (blocked_status == STR_OK) {
        return STR_ERR_FMT;
    }
    if (blocked_status != remove_status) {
        return blocked_status;
    }
    status = demo_print_key_value("blocked append", str_cstr(string));
    if (status != STR_OK) {
        return status;
    }
    status = demo_print_status(DEMO_STICKY_KEY, str_status(string));
    assert(str_status(string) == remove_status);
    return status;
}

static str_status_t demo_binary(void)
{
    const char raw[DEMO_EMBEDDED_BYTES] = {'a', '\0', 'b'};
    str_t string = STR_EMPTY;
    str_status_t status = demo_print_section("binary");

    if (status != STR_OK) {
        return demo_release(&string, status);
    }

    status = str_set_n(&string, raw, (size_t)DEMO_EMBEDDED_BYTES);
    if (status != STR_OK) {
        return demo_release(&string, status);
    }

    status = demo_print_size(DEMO_LENGTH_KEY, str_len(&string));
    if (status != STR_OK) {
        return demo_release(&string, status);
    }
    status = demo_print_embedded_hex("hex", str_view(&string));
    if (status != STR_OK) {
        return demo_release(&string, status);
    }
    status = demo_print_key_value("cstr", str_cstr(&string));
    return demo_release(&string, status);
}

static str_status_t demo_capacity(void)
{
    str_t string = STR_EMPTY;
    str_status_t status = demo_print_section("capacity");

    if (status != STR_OK) {
        return demo_release(&string, status);
    }

    status = demo_print_size("requested", (size_t)DEMO_RESERVE_CONTENT_BYTES);
    if (status != STR_OK) {
        return demo_release(&string, status);
    }

    status = str_reserve(&string, (size_t)DEMO_RESERVE_CONTENT_BYTES);
    if (status != STR_OK) {
        return demo_release(&string, status);
    }
    status = demo_print_size("reserved", str_capacity(&string));
    if (status != STR_OK) {
        return demo_release(&string, status);
    }

    status = str_set(&string, DEMO_SHORT_TEXT);
    if (status != STR_OK) {
        return demo_release(&string, status);
    }
    status = str_shrink_to_fit(&string);
    if (status != STR_OK) {
        return demo_release(&string, status);
    }
    status = demo_print_size("after shrink", str_capacity(&string));
    return demo_release(&string, status);
}

static str_status_t demo_release(str_t *string, str_status_t status)
{
    assert(string != NULL);
    str_deinit(string);
    assert(string->buf == NULL);
    assert(string->len == 0);
    return status;
}

static str_status_t demo_release_pair(str_t *first, str_t *second, str_status_t status)
{
    assert(first != NULL);
    assert(second != NULL);
    str_deinit(first);
    str_deinit(second);
    assert(first->buf == NULL);
    assert(second->buf == NULL);
    return status;
}

static void demo_release_buffer(char *buffer)
{
    free(buffer);
}

static str_status_t demo_print_string(const str_t *string, const char *key)
{
    assert(string != NULL);
    assert(key != NULL);
    if (str_failed(string)) {
        return str_status(string);
    }
    if (str_is_empty(string)) {
        return demo_print_key_value(key, DEMO_EMPTY_MARK);
    }
    return demo_print_key_value(key, str_cstr(string));
}

static str_status_t demo_print_match(const str_t *string, demo_match_request_t request)
{
    assert(string != NULL);
    assert(request.key != NULL);
    assert(request.needle != NULL);
    assert(request.mode == DEMO_MATCH_FIRST || request.mode == DEMO_MATCH_LAST);

    size_t idx = STR_NPOS;
    str_status_t status = STR_OK;
    if (request.mode == DEMO_MATCH_FIRST) {
        status = str_find(string, &idx, request.needle);
    } else {
        status = str_view_rfind(str_view(string), &idx, str_view_from_cstr(request.needle));
    }

    if (status != STR_OK) {
        return status;
    }
    return demo_print_index(request.key, idx);
}

/*
 * Presentation and demo-invariant failures reuse STR_ERR_FMT.
 * The demo has no separate status module.
 */
static str_status_t demo_print_section(const char *title)
{
    assert(title != NULL);

    int written = printf("\n== %s\n", title);
    if (written < 0) {
        return STR_ERR_FMT;
    }
    return STR_OK;
}

static str_status_t demo_print_key_value(const char *key, const char *value)
{
    assert(key != NULL);
    assert(value != NULL);

    int written = printf("%s: %s\n", key, value);
    if (written < 0) {
        return STR_ERR_FMT;
    }
    return STR_OK;
}

static str_status_t demo_print_size(const char *key, size_t value)
{
    assert(key != NULL);

    char number_buffer[DEMO_NUMBER_BUFFER_BYTES] = {0};
    str_status_t status = demo_format_size_value(number_buffer, sizeof(number_buffer), value);
    if (status != STR_OK) {
        return status;
    }
    return demo_print_key_value(key, number_buffer);
}

static str_status_t demo_print_index(const char *key, size_t idx)
{
    assert(key != NULL);
    if (idx == STR_NPOS) {
        return demo_print_key_value(key, DEMO_NPOS_NAME);
    }
    return demo_print_size(key, idx);
}

static str_status_t demo_print_bool(const char *key, bool value)
{
    assert(key != NULL);
    return demo_print_key_value(key, value ? DEMO_YES : DEMO_NO);
}

static str_status_t demo_print_status(const char *key, str_status_t status)
{
    assert(key != NULL);
    return demo_print_key_value(key, str_status_name(status));
}

static str_status_t demo_print_span(const char *key, str_view_t view)
{
    assert(key != NULL);
    assert(str_view_is_valid(view));
    if (view.len == 0) {
        return demo_print_key_value(key, DEMO_EMPTY_MARK);
    }

    str_status_t status = demo_print_span_prefix(key);
    if (status != STR_OK) {
        return status;
    }
    status = demo_write_view_bytes(view);
    if (status != STR_OK) {
        return status;
    }
    return demo_print_newline();
}

static str_status_t demo_print_span_prefix(const char *key)
{
    assert(key != NULL);

    int written = printf("%s: ", key);
    if (written < 0) {
        return STR_ERR_FMT;
    }
    return STR_OK;
}

static str_status_t demo_write_view_bytes(str_view_t view)
{
    assert(str_view_is_valid(view));
    assert(view.ptr != NULL);
    assert(view.len > 0);

    size_t written = fwrite(view.ptr, sizeof(view.ptr[0]), view.len, stdout);
    if (written != view.len) {
        return STR_ERR_FMT;
    }
    return STR_OK;
}

static str_status_t demo_print_newline(void)
{
    int written = fputc('\n', stdout);
    if (written == EOF) {
        return STR_ERR_FMT;
    }
    return STR_OK;
}

static str_status_t demo_print_part(size_t idx, str_view_t part)
{
    assert(str_view_is_valid(part));

    char key[DEMO_NUMBER_BUFFER_BYTES] = {0};
    str_status_t status = demo_format_part_key(key, sizeof(key), idx);
    if (status != STR_OK) {
        return status;
    }
    return demo_print_span(key, part);
}

static str_status_t demo_format_size_value(char *out_buffer, size_t capacity, size_t value)
{
    assert(out_buffer != NULL);
    assert(capacity > 0);

    /* Output capacity is explicit and the result is checked. */
    /* NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling) */
    int printed = snprintf(out_buffer, capacity, "%zu", value);
    if (printed < 0) {
        return STR_ERR_FMT;
    }
    if ((size_t)printed >= capacity) {
        return STR_ERR_FMT;
    }
    return STR_OK;
}

static str_status_t demo_format_part_key(char *out_buffer, size_t capacity, size_t idx)
{
    assert(out_buffer != NULL);
    assert(capacity > 0);

    /* Output capacity is explicit and the result is checked. */
    /* NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling) */
    int printed = snprintf(out_buffer, capacity, "part[%zu]", idx);
    if (printed < 0) {
        return STR_ERR_FMT;
    }
    if ((size_t)printed >= capacity) {
        return STR_ERR_FMT;
    }
    return STR_OK;
}

static str_status_t demo_print_embedded_hex(const char *key, str_view_t view)
{
    assert(key != NULL);
    assert(str_view_is_valid(view));
    assert(view.ptr != NULL);
    assert(view.len >= (size_t)DEMO_EMBEDDED_BYTES);

    int written = printf("%s: %02x %02x %02x\n", key,
                         (unsigned int)(unsigned char)view.ptr[DEMO_EMBEDDED_FIRST_INDEX],
                         (unsigned int)(unsigned char)view.ptr[DEMO_EMBEDDED_SECOND_INDEX],
                         (unsigned int)(unsigned char)view.ptr[DEMO_EMBEDDED_THIRD_INDEX]);
    if (written < 0) {
        return STR_ERR_FMT;
    }
    return STR_OK;
}
