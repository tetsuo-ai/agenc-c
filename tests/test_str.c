/* test_str.c: driver for the str module. */

#include <stdbool.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "str.h"

enum {
    TEST_LONG_LEN = 40,
    TEST_EMBEDDED_LEN = 3,
    TEST_SPLIT_CAP = 4,
    TEST_SPLIT_TRUNC = 1,
    TEST_RESERVE_ARG = 8,
    TEST_SLICE_OFF = 2,
    TEST_SLICE_LEN = 3,
    TEST_INSERT_BASE_LEN = 15,
    TEST_INSERT_STORAGE = 32,
    TEST_INSERT_RESERVE = 64,
    TEST_RESIZE_LEN = 20,
    TEST_SEARCH_HAY_MAX = 7,
    TEST_SEARCH_NEEDLE_MAX = 5,
    TEST_SEARCH_STORAGE = 8,
    TEST_SEARCH_ALPHABET_SIZE = 3,
    TEST_RANDOM_CASES = 20000,
    TEST_RANDOM_HAY_STORAGE = 128,
    TEST_RANDOM_NEEDLE_STORAGE = 64,
    TEST_OUTPUT_SENTINEL = 17,
    TEST_UNKNOWN_STATUS = 99,
    TEST_UNSIGNED_LOW = 0x7f,
    TEST_UNSIGNED_HIGH = 0x80
};

static struct {
    int run;
} test_ctx;

static void test_expect(bool cond, const char *file, int line, const char *expr)
{
    test_ctx.run++;
    if (cond)
        return;
    fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
    abort();
}

#define EXPECT(cond) test_expect((bool)(cond), __FILE__, __LINE__, #cond)

#ifdef STR_TEST
static void test_restore_alloc(void)
{
    str_test_reset_alloc_failures();
}
#endif

static str_status_t test_append_vfmt(str_t *s, const char *format, ...)
{
    va_list args;
    str_status_t status;

    va_start(args, format);
    status = str_append_vfmt(s, format, args);
    va_end(args);
    return status;
}

static size_t test_size_pow(size_t base, size_t exponent)
{
    size_t result = 1;

    for (size_t idx = 0; idx < exponent; idx++)
        result *= base;
    return result;
}

static void test_fill_word(char *out, size_t len, size_t code)
{
    static const unsigned char alphabet[TEST_SEARCH_ALPHABET_SIZE] = {0x00, TEST_UNSIGNED_LOW,
                                                                      TEST_UNSIGNED_HIGH};

    for (size_t idx = 0; idx < len; idx++) {
        size_t digit = code % (size_t)TEST_SEARCH_ALPHABET_SIZE;
        out[idx] = (char)alphabet[digit];
        code /= (size_t)TEST_SEARCH_ALPHABET_SIZE;
    }
}

static uint32_t test_random_next(uint32_t *state)
{
    uint32_t value = *state;

    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

static void test_fill_random(char *out, size_t len, uint32_t *state)
{
    for (size_t idx = 0; idx < len; idx++)
        out[idx] = (char)(test_random_next(state) & UINT32_C(0xff));
}

static size_t test_search_reference(str_view_t hay, str_view_t needle, bool find_last)
{
    if (needle.len == 0)
        return find_last ? hay.len : 0;
    if (needle.len > hay.len)
        return STR_NPOS;

    size_t last_idx = hay.len - needle.len;
    size_t found = STR_NPOS;
    for (size_t idx = 0; idx <= last_idx; idx++) {
        if (memcmp(hay.ptr + idx, needle.ptr, needle.len) != 0)
            continue;
        if (!find_last)
            return idx;
        found = idx;
    }
    return found;
}

static bool test_search_exhaustive_result(void)
{
    char hay_buf[TEST_SEARCH_STORAGE] = {0};
    char needle_buf[TEST_SEARCH_STORAGE] = {0};

    for (size_t hay_len = 0; hay_len <= TEST_SEARCH_HAY_MAX; hay_len++) {
        size_t hay_count = test_size_pow(TEST_SEARCH_ALPHABET_SIZE, hay_len);
        for (size_t hay_code = 0; hay_code < hay_count; hay_code++) {
            test_fill_word(hay_buf, hay_len, hay_code);
            str_view_t hay = str_view_from_n(hay_buf, hay_len);

            for (size_t needle_len = 0; needle_len <= TEST_SEARCH_NEEDLE_MAX; needle_len++) {
                size_t needle_count = test_size_pow(TEST_SEARCH_ALPHABET_SIZE, needle_len);
                for (size_t needle_code = 0; needle_code < needle_count; needle_code++) {
                    test_fill_word(needle_buf, needle_len, needle_code);
                    str_view_t needle = str_view_from_n(needle_buf, needle_len);
                    size_t first = STR_NPOS;
                    size_t last = STR_NPOS;

                    if (str_view_find(hay, &first, needle) != STR_OK)
                        return false;
                    if (str_view_rfind(hay, &last, needle) != STR_OK)
                        return false;
                    if (first != test_search_reference(hay, needle, false))
                        return false;
                    if (last != test_search_reference(hay, needle, true))
                        return false;
                }
            }
        }
    }
    return true;
}

static bool test_search_random_result(void)
{
    char hay_buf[TEST_RANDOM_HAY_STORAGE];
    char needle_buf[TEST_RANDOM_NEEDLE_STORAGE];
    uint32_t state = UINT32_C(0x243f6a88);

    for (size_t sample = 0; sample < (size_t)TEST_RANDOM_CASES; sample++) {
        size_t hay_len =
            (size_t)(test_random_next(&state) % (uint32_t)(TEST_RANDOM_HAY_STORAGE + 1));
        size_t needle_len =
            (size_t)(test_random_next(&state) % (uint32_t)(TEST_RANDOM_NEEDLE_STORAGE + 1));
        test_fill_random(hay_buf, hay_len, &state);
        test_fill_random(needle_buf, needle_len, &state);
        str_view_t hay = str_view_from_n(hay_buf, hay_len);
        str_view_t needle = str_view_from_n(needle_buf, needle_len);
        size_t first = STR_NPOS;
        size_t last = STR_NPOS;

        if (str_view_find(hay, &first, needle) != STR_OK)
            return false;
        if (str_view_rfind(hay, &last, needle) != STR_OK)
            return false;
        if (first != test_search_reference(hay, needle, false))
            return false;
        if (last != test_search_reference(hay, needle, true))
            return false;
    }
    return true;
}

static void test_zero_init_cstr(void)
{
    str_t s = STR_EMPTY;

    EXPECT(str_cstr(&s) != NULL);
    EXPECT(str_cstr(&s)[0] == '\0');
    EXPECT(str_cstr(NULL)[0] == '\0');
    EXPECT(str_len(&s) == 0);
    EXPECT(str_is_empty(&s));
    EXPECT(str_is_empty(NULL));
}

static void test_append_grows(void)
{
    str_t s = STR_EMPTY;
    size_t i;

    for (i = 0; i < TEST_LONG_LEN; i++)
        EXPECT(str_append_char(&s, 'a') == STR_OK);

    EXPECT(str_len(&s) == TEST_LONG_LEN);
    EXPECT(s.cap > str_len(&s));
    EXPECT(str_cstr(&s)[TEST_LONG_LEN] == '\0');
    EXPECT(str_cstr(&s)[0] == 'a');
    str_deinit(&s);
}

static void test_embedded_nul(void)
{
    str_t s = STR_EMPTY;
    const char raw[TEST_EMBEDDED_LEN] = {'a', '\0', 'b'};

    EXPECT(str_set_n(&s, raw, TEST_EMBEDDED_LEN) == STR_OK);
    EXPECT(str_len(&s) == TEST_EMBEDDED_LEN);
    EXPECT(str_cstr(&s)[0] == 'a');
    EXPECT(str_cstr(&s)[1] == '\0');
    EXPECT(str_cstr(&s)[2] == 'b');
    EXPECT(str_cstr(&s)[3] == '\0');
    str_deinit(&s);
}

static void test_append_overlap(void)
{
    str_t s = STR_EMPTY;

    EXPECT(str_set(&s, "hello") == STR_OK);
    EXPECT(str_append_n(&s, str_cstr(&s), str_len(&s)) == STR_OK);
    EXPECT(str_equals_cstr(&s, "hellohello"));
    str_deinit(&s);
}

static void test_cstr_self_overlap(void)
{
    str_t s = STR_EMPTY;

    EXPECT(str_set(&s, "0123456789abcde") == STR_OK);
    EXPECT(str_append(&s, str_cstr(&s) + 5) == STR_OK);
    EXPECT(str_equals_cstr(&s, "0123456789abcde56789abcde"));
    EXPECT(str_set(&s, str_cstr(&s) + TEST_SLICE_OFF) == STR_OK);
    EXPECT(str_equals_cstr(&s, "23456789abcde56789abcde"));
    str_deinit(&s);
}

static void test_insert_overlap(void)
{
    str_t s = STR_EMPTY;

    EXPECT(str_set(&s, "abcdefgh") == STR_OK);
    EXPECT(str_insert_n(&s, 3, str_cstr(&s) + 2, 3) == STR_OK);
    EXPECT(str_equals_cstr(&s, "abccdedefgh"));
    str_deinit(&s);

    EXPECT(str_set(&s, "abcdefgh") == STR_OK);
    EXPECT(str_insert_n(&s, 2, str_cstr(&s) + 4, 3) == STR_OK);
    EXPECT(str_equals_cstr(&s, "abefgcdefgh"));
    str_deinit(&s);
}

static void test_insert_overlap_crosses_gap(void)
{
    str_t s = STR_EMPTY;

    EXPECT(str_set(&s, "abcdefgh") == STR_OK);
    EXPECT(str_insert_n(&s, 4, str_cstr(&s) + 2, 5) == STR_OK);
    EXPECT(str_equals_cstr(&s, "abcdcdefgefgh"));
    str_deinit(&s);
}

static void test_insert_own_terminator(void)
{
    str_t s = STR_EMPTY;
    const char expected[TEST_SPLIT_CAP] = {'\0', 'a', 'b', 'c'};

    EXPECT(str_set(&s, "abc") == STR_OK);
    EXPECT(str_insert_n(&s, 0, str_cstr(&s) + str_len(&s), 1) == STR_OK);
    EXPECT(str_len(&s) == sizeof(expected));
    EXPECT(memcmp(str_cstr(&s), expected, sizeof(expected)) == 0);
    str_deinit(&s);
}

#ifdef STR_TEST
static void test_oom_preserves(void)
{
    str_t s = STR_EMPTY;

    EXPECT(str_append(&s, "hello") == STR_OK);
    str_test_fail_alloc_after(0);
    EXPECT(str_append(&s, "0123456789ABCDEF") == STR_ERR_ALLOC);
    EXPECT(str_equals_cstr(&s, "hello"));
    EXPECT(str_failed(&s));
    EXPECT(str_append(&s, "x") == STR_ERR_ALLOC);
    EXPECT(str_equals_cstr(&s, "hello"));
    test_restore_alloc();
    str_deinit(&s);
}
#endif

static void test_overflow_size(void)
{
    str_t s = STR_EMPTY;

    EXPECT(str_append(&s, "x") == STR_OK);
    EXPECT(str_append_n(&s, "z", SIZE_MAX) == STR_ERR_OVERFLOW);
    EXPECT(str_equals_cstr(&s, "x"));
    EXPECT(str_failed(&s));
    str_deinit(&s);
}

static void test_deinit_twice(void)
{
    str_t s = STR_EMPTY;

    EXPECT(str_set(&s, "once") == STR_OK);
    str_deinit(&s);
    str_deinit(&s);
    EXPECT(str_is_empty(&s));
    EXPECT(str_ok(&s));
}

static void test_move(void)
{
    str_t src = STR_EMPTY;
    str_t dst = STR_EMPTY;

    EXPECT(str_set(&src, "moved") == STR_OK);
    EXPECT(str_set(&dst, "old") == STR_OK);
    str_move(&dst, &src);
    EXPECT(str_equals_cstr(&dst, "moved"));
    EXPECT(str_is_empty(&src));
    str_deinit(&dst);
    str_deinit(&src);
}

static void test_copy(void)
{
    str_t src = STR_EMPTY;
    str_t dst = STR_EMPTY;

    EXPECT(str_set(&src, "copy") == STR_OK);
    EXPECT(str_copy(&dst, &src) == STR_OK);
    EXPECT(str_equals_cstr(&dst, "copy"));
    EXPECT(str_equals_cstr(&src, "copy"));
    str_deinit(&dst);
    str_deinit(&src);
}

static void test_copy_obeys_sticky_error(void)
{
    str_t src = STR_EMPTY;
    str_t dst = STR_EMPTY;

    EXPECT(str_set(&src, "new") == STR_OK);
    EXPECT(str_set(&dst, "keep") == STR_OK);
    EXPECT(str_append_n(&dst, "x", SIZE_MAX) == STR_ERR_OVERFLOW);
    EXPECT(str_copy(&dst, &src) == STR_ERR_OVERFLOW);
    EXPECT(str_equals_cstr(&dst, "keep"));
    EXPECT(str_status(&dst) == STR_ERR_OVERFLOW);
    str_deinit(&dst);
    str_deinit(&src);
}

static void test_find(void)
{
    str_t s = STR_EMPTY;
    size_t idx;

    EXPECT(str_set(&s, "hello hello") == STR_OK);
    EXPECT(str_find(&s, &idx, "hello") == STR_OK);
    EXPECT(idx == 0);
    EXPECT(str_find(&s, &idx, "missing") == STR_OK);
    EXPECT(idx == STR_NPOS);
    EXPECT(str_find(&s, &idx, "") == STR_OK);
    EXPECT(idx == 0);
    EXPECT(str_find_char(&s, &idx, 'e') == STR_OK);
    EXPECT(idx == 1);
    str_deinit(&s);
}

static void test_slice(void)
{
    str_t s = STR_EMPTY;
    str_view_t view;

    EXPECT(str_set(&s, "abcdef") == STR_OK);
    EXPECT(str_slice(&s, &view, 6, 0) == STR_OK);
    EXPECT(view.len == 0);
    EXPECT(str_slice(&s, &view, TEST_SLICE_OFF, TEST_SLICE_LEN) == STR_OK);
    EXPECT(view.len == TEST_SLICE_LEN);
    EXPECT(memcmp(view.ptr, "cde", TEST_SLICE_LEN) == 0);
    EXPECT(str_slice(&s, &view, 7, 0) == STR_ERR_RANGE);
    EXPECT(str_slice(&s, &view, 2, 5) == STR_ERR_RANGE);
    str_deinit(&s);
}

static void test_split(void)
{
    str_view_t parts[TEST_SPLIT_CAP];
    str_view_t src = str_view_from_cstr("a,,b");
    str_split_out_t out = {.parts = parts, .cap = TEST_SPLIT_CAP, .count = 0};

    EXPECT(str_split_view(src, &out, ',') == STR_OK);
    EXPECT(out.count == 3);
    EXPECT(out.parts[0].len == 1 && out.parts[0].ptr[0] == 'a');
    EXPECT(out.parts[1].len == 0);
    EXPECT(out.parts[2].len == 1 && out.parts[2].ptr[0] == 'b');

    out.cap = TEST_SPLIT_TRUNC;
    EXPECT(str_split_view(src, &out, ',') == STR_OK);
    EXPECT(out.count == 3);
    EXPECT(out.count > out.cap);
}

static void test_append_fmt(void)
{
    str_t s = STR_EMPTY;

    EXPECT(str_append_fmt(&s, "%s-%d", "abc", 12) == STR_OK);
    EXPECT(str_equals_cstr(&s, "abc-12"));
    EXPECT(str_append_fmt(&s, "-%s", "xxxxxxxxxxxxxxxxxxxx") == STR_OK);
    EXPECT(str_starts_with(&s, "abc-12-"));
    EXPECT(str_ends_with(&s, "xxxxxxxxxxxxxxxxxxxx"));
    str_deinit(&s);
}

static void test_append_fmt_self_alias(void)
{
    str_t s = STR_EMPTY;
    const char *format;

    EXPECT(str_set(&s, "%100s") == STR_OK);
    format = str_cstr(&s);
    EXPECT(str_append_fmt(&s, format, "x") == STR_OK);
    EXPECT(str_starts_with(&s, "%100s"));
    EXPECT(str_ends_with(&s, "x"));
    str_deinit(&s);

    EXPECT(str_set(&s, "123456789abcdef") == STR_OK);
    EXPECT(str_append_fmt(&s, "%s", str_cstr(&s)) == STR_OK);
    EXPECT(str_equals_cstr(&s, "123456789abcdef123456789abcdef"));
    str_deinit(&s);
}

static void test_detach_empty(void)
{
    str_t s = STR_EMPTY;
    char *owned;

    owned = str_detach(&s);
    EXPECT(owned != NULL);
    EXPECT(owned[0] == '\0');
    free(owned);
    EXPECT(str_is_empty(&s));
}

static void test_null_mutator(void)
{
    str_t s = STR_EMPTY;

    EXPECT(str_append(NULL, "x") == STR_ERR_ARG);
    EXPECT(str_set(NULL, "x") == STR_ERR_ARG);
    EXPECT(str_reserve(NULL, TEST_RESERVE_ARG) == STR_ERR_ARG);
    EXPECT(str_shrink_to_fit(NULL) == STR_ERR_ARG);
    EXPECT(str_resize(NULL, 0, 'x') == STR_ERR_ARG);
    EXPECT(str_insert_n(NULL, 0, NULL, 0) == STR_ERR_ARG);
    EXPECT(str_remove(NULL, 0, 0) == STR_ERR_ARG);
    EXPECT(str_replace_view(NULL, 0, 0, str_view_from_n(NULL, 0)) == STR_ERR_ARG);
    EXPECT(str_append_fmt(NULL, "%s", "x") == STR_ERR_ARG);
    EXPECT(test_append_vfmt(NULL, "%s", "x") == STR_ERR_ARG);
    EXPECT(str_copy(NULL, &s) == STR_ERR_ARG);
    EXPECT(str_detach(NULL) == NULL);
    EXPECT(str_status(NULL) == STR_ERR_ARG);
    EXPECT(str_failed(NULL));
    EXPECT(!str_ok(NULL));

    str_init(NULL);
    str_clear(NULL);
    str_clear_error(NULL);
    str_move(NULL, NULL);
    str_deinit(NULL);
    str_deinit(&s);
}

static void test_invalid_sources(void)
{
    str_t s = STR_EMPTY;
    str_view_t invalid = str_view_from_n(NULL, TEST_SPLIT_TRUNC);

    EXPECT(str_set(&s, NULL) == STR_ERR_ARG);
    EXPECT(str_append(&s, "blocked") == STR_ERR_ARG);
    str_clear_error(&s);
    EXPECT(str_set_n(&s, NULL, 0) == STR_OK);
    EXPECT(str_append_n(&s, NULL, 0) == STR_OK);
    EXPECT(str_insert_n(&s, 0, NULL, 0) == STR_OK);
    EXPECT(str_set_view(&s, invalid) == STR_ERR_ARG);
    str_clear_error(&s);
    EXPECT(str_append_view(&s, invalid) == STR_ERR_ARG);
    str_clear_error(&s);
    EXPECT(str_insert_view(&s, 0, invalid) == STR_ERR_ARG);
    str_clear_error(&s);
    EXPECT(str_replace_view(&s, 0, 0, invalid) == STR_ERR_ARG);
    str_clear_error(&s);
    EXPECT(str_append_fmt(&s, NULL) == STR_ERR_ARG);
    str_clear_error(&s);
    EXPECT(str_copy(&s, NULL) == STR_ERR_ARG);
    str_deinit(&s);
}

static void test_null_outputs(void)
{
    str_t s = STR_EMPTY;
    size_t idx = TEST_OUTPUT_SENTINEL;
    str_view_t view = str_view_from_cstr("sentinel");

    EXPECT(str_find(NULL, &idx, "x") == STR_ERR_ARG);
    EXPECT(str_find(&s, NULL, "x") == STR_ERR_ARG);
    EXPECT(str_find(&s, &idx, NULL) == STR_ERR_ARG);
    EXPECT(idx == TEST_OUTPUT_SENTINEL);
    EXPECT(str_find_char(NULL, &idx, 'x') == STR_ERR_ARG);
    EXPECT(str_find_char(&s, NULL, 'x') == STR_ERR_ARG);
    EXPECT(str_slice(NULL, &view, 0, 0) == STR_ERR_ARG);
    EXPECT(str_slice(&s, NULL, 0, 0) == STR_ERR_ARG);
    EXPECT(view.len == strlen("sentinel"));
    EXPECT(str_view_compare(NULL, str_view(&s), str_view(&s)) == STR_ERR_ARG);
    EXPECT(str_view_find(str_view(&s), NULL, str_view(&s)) == STR_ERR_ARG);
    EXPECT(str_view_rfind(str_view(&s), NULL, str_view(&s)) == STR_ERR_ARG);
    EXPECT(str_split_view(str_view(&s), NULL, ',') == STR_ERR_ARG);
    str_split_out_t out = {.parts = NULL, .cap = 1, .count = TEST_OUTPUT_SENTINEL};
    EXPECT(str_split_view(str_view(&s), &out, ',') == STR_ERR_ARG);
    EXPECT(out.count == TEST_OUTPUT_SENTINEL);
    str_deinit(&s);
}

static void test_view_trim(void)
{
    str_view_t view;

    view = str_view_trim(str_view_from_cstr("  hi\t\n"));
    EXPECT(view.len == 2);
    EXPECT(view.ptr[0] == 'h' && view.ptr[1] == 'i');
    view = str_view_trim(str_view_from_cstr("   \n"));
    EXPECT(view.len == 0);
    view = str_view_trim(str_view_from_n(NULL, 0));
    EXPECT(view.len == 0);
}

static void test_clear(void)
{
    str_t s = STR_EMPTY;

    EXPECT(str_set(&s, "keep-cap") == STR_OK);
    str_clear(&s);
    EXPECT(str_is_empty(&s));
    EXPECT(str_ok(&s));
    EXPECT(s.cap > 0);
    EXPECT(!str_equals_cstr(&s, "keep-cap"));
    EXPECT(str_equals_cstr(&s, ""));
    EXPECT(!str_equals(NULL, &s));
    str_deinit(&s);
}

static void test_remove_insert(void)
{
    str_t s = STR_EMPTY;

    EXPECT(str_set(&s, "hello") == STR_OK);
    EXPECT(str_remove(&s, 1, 2) == STR_OK);
    EXPECT(str_equals_cstr(&s, "hlo"));
    EXPECT(str_insert_n(&s, 3, "!", 1) == STR_OK);
    EXPECT(str_equals_cstr(&s, "hlo!"));
    EXPECT(str_remove(&s, 0, 10) == STR_ERR_RANGE);
    str_deinit(&s);
}

static void test_init_explicit(void)
{
    str_t s;

    str_init(&s);
    EXPECT(str_cstr(&s)[0] == '\0');
    EXPECT(str_append(&s, "ok") == STR_OK);
    str_deinit(&s);
}

static void test_status_names(void)
{
    EXPECT(strcmp(str_status_name(STR_OK), "STR_OK") == 0);
    EXPECT(strcmp(str_status_name(STR_ERR_ARG), "STR_ERR_ARG") == 0);
    EXPECT(strcmp(str_status_name(STR_ERR_ALLOC), "STR_ERR_ALLOC") == 0);
    EXPECT(strcmp(str_status_name(STR_ERR_RANGE), "STR_ERR_RANGE") == 0);
    EXPECT(strcmp(str_status_name(STR_ERR_OVERFLOW), "STR_ERR_OVERFLOW") == 0);
    EXPECT(strcmp(str_status_name(STR_ERR_FMT), "STR_ERR_FMT") == 0);
    EXPECT(strcmp(str_status_name((str_status_t)TEST_UNKNOWN_STATUS), "STR_ERR_UNKNOWN") == 0);
}

static void test_view_bridges(void)
{
    str_t s = STR_EMPTY;
    const char raw[TEST_EMBEDDED_LEN] = {'a', '\0', 'b'};
    const char expected[] = {'a', '\0', 'b', 'a', '\0', 'a', '\0', 'b', 'a', '\0'};

    EXPECT(str_set_view(&s, str_view_from_n(raw, sizeof(raw))) == STR_OK);
    EXPECT(str_append_view(&s, str_view_from_n(raw, TEST_SLICE_OFF)) == STR_OK);
    EXPECT(str_insert_view(&s, str_len(&s), str_view(&s)) == STR_OK);
    EXPECT(str_len(&s) == sizeof(expected));
    EXPECT(memcmp(str_cstr(&s), expected, sizeof(expected)) == 0);
    str_deinit(&s);
}

static void test_view_queries(void)
{
    int order = TEST_OUTPUT_SENTINEL;
    const char low[] = {(char)TEST_UNSIGNED_LOW};
    const char high[] = {(char)TEST_UNSIGNED_HIGH};
    str_view_t value = str_view_from_cstr("abcdef");

    EXPECT(str_view_compare(&order, str_view_from_n(low, sizeof(low)),
                            str_view_from_n(high, sizeof(high))) == STR_OK);
    EXPECT(order < 0);
    EXPECT(str_view_compare(&order, str_view_from_cstr("abc"), str_view_from_cstr("abc")) ==
           STR_OK);
    EXPECT(order == 0);
    EXPECT(str_view_compare(&order, str_view_from_cstr("abcd"), str_view_from_cstr("abc")) ==
           STR_OK);
    EXPECT(order > 0);
    EXPECT(str_view_starts_with(value, str_view_from_cstr("abc")));
    EXPECT(str_view_ends_with(value, str_view_from_cstr("def")));
    EXPECT(str_view_starts_with(value, str_view_from_n(NULL, 0)));
    EXPECT(str_view_ends_with(str_view_from_n(NULL, 0), str_view_from_n(NULL, 0)));
}

static void test_invalid_views(void)
{
    str_view_t invalid = str_view_from_n(NULL, TEST_SPLIT_TRUNC);
    str_view_t parts[TEST_SPLIT_CAP] = {0};
    str_split_out_t out = {.parts = parts, .cap = TEST_SPLIT_CAP, .count = TEST_OUTPUT_SENTINEL};
    size_t idx = TEST_OUTPUT_SENTINEL;
    int order = TEST_OUTPUT_SENTINEL;

    EXPECT(!str_view_is_valid(invalid));
    EXPECT(!str_view_equals(invalid, invalid));
    EXPECT(!str_view_starts_with(invalid, str_view_from_n(NULL, 0)));
    EXPECT(!str_view_ends_with(invalid, str_view_from_n(NULL, 0)));
    EXPECT(str_view_find(invalid, &idx, str_view_from_n(NULL, 0)) == STR_ERR_ARG);
    EXPECT(idx == TEST_OUTPUT_SENTINEL);
    EXPECT(str_view_rfind(invalid, &idx, str_view_from_n(NULL, 0)) == STR_ERR_ARG);
    EXPECT(idx == TEST_OUTPUT_SENTINEL);
    EXPECT(str_view_compare(&order, invalid, str_view_from_n(NULL, 0)) == STR_ERR_ARG);
    EXPECT(order == TEST_OUTPUT_SENTINEL);
    EXPECT(str_split_view(invalid, &out, ',') == STR_ERR_ARG);
    EXPECT(out.count == TEST_OUTPUT_SENTINEL);
    EXPECT(str_view_trim(invalid).len == 0);
}

static void test_capacity_resize_shrink(void)
{
    str_t s = STR_EMPTY;

    EXPECT(str_capacity(NULL) == 0);
    EXPECT(str_capacity(&s) == 0);
    EXPECT(str_reserve(&s, TEST_INSERT_RESERVE) == STR_OK);
    EXPECT(str_capacity(&s) >= TEST_INSERT_RESERVE);
    EXPECT(str_set(&s, "abc") == STR_OK);
    EXPECT(str_shrink_to_fit(&s) == STR_OK);
    EXPECT(str_capacity(&s) == str_len(&s));
    EXPECT(str_resize(&s, TEST_RESIZE_LEN, 'x') == STR_OK);
    EXPECT(str_len(&s) == TEST_RESIZE_LEN);
    EXPECT(str_cstr(&s)[TEST_RESIZE_LEN - 1] == 'x');
    EXPECT(str_resize(&s, TEST_SLICE_OFF, 'z') == STR_OK);
    EXPECT(str_equals_cstr(&s, "ab"));
    str_clear(&s);
    EXPECT(str_capacity(&s) >= TEST_RESIZE_LEN);
    EXPECT(str_shrink_to_fit(&s) == STR_OK);
    EXPECT(str_capacity(&s) == 0);
    str_deinit(&s);
}

#ifdef STR_TEST
static void test_shrink_oom_preserves(void)
{
    str_t s = STR_EMPTY;

    EXPECT(str_set(&s, "short") == STR_OK);
    size_t old_capacity = str_capacity(&s);
    str_test_fail_alloc_after(0);
    EXPECT(str_shrink_to_fit(&s) == STR_ERR_ALLOC);
    EXPECT(str_equals_cstr(&s, "short"));
    EXPECT(str_capacity(&s) == old_capacity);
    test_restore_alloc();
    str_deinit(&s);
}
#endif

static void test_replace_view(void)
{
    str_t s = STR_EMPTY;

    EXPECT(str_set(&s, "abcdef") == STR_OK);
    EXPECT(str_replace_view(&s, TEST_SLICE_OFF, TEST_SLICE_OFF, str_view_from_cstr("XY")) ==
           STR_OK);
    EXPECT(str_equals_cstr(&s, "abXYef"));
    EXPECT(str_replace_view(&s, TEST_SLICE_OFF, TEST_SLICE_OFF, str_view_from_cstr("12345")) ==
           STR_OK);
    EXPECT(str_equals_cstr(&s, "ab12345ef"));
    EXPECT(str_replace_view(&s, TEST_SLICE_OFF, TEST_SLICE_LEN + TEST_SLICE_OFF,
                            str_view_from_cstr("Z")) == STR_OK);
    EXPECT(str_equals_cstr(&s, "abZef"));
    str_deinit(&s);

    EXPECT(str_set(&s, "abcdef") == STR_OK);
    str_view_t replacement = str_view_from_n(str_cstr(&s) + 1, TEST_SPLIT_CAP);
    EXPECT(str_replace_view(&s, TEST_SLICE_OFF, TEST_SPLIT_TRUNC, replacement) == STR_OK);
    EXPECT(str_equals_cstr(&s, "abbcdedef"));
    str_deinit(&s);
}

#ifdef STR_TEST
static void test_replace_oom_preserves(void)
{
    static const char long_replacement[] = "0123456789abcdef0123456789abcdef";
    str_t s = STR_EMPTY;

    EXPECT(str_set(&s, "abcdef") == STR_OK);
    str_view_t replacement = str_view_from_n(str_cstr(&s), TEST_SPLIT_CAP);
    str_test_fail_alloc_after(0);
    EXPECT(str_replace_view(&s, TEST_SLICE_OFF, TEST_SPLIT_TRUNC, replacement) == STR_ERR_ALLOC);
    EXPECT(str_equals_cstr(&s, "abcdef"));
    EXPECT(str_status(&s) == STR_ERR_ALLOC);
    test_restore_alloc();

    str_clear_error(&s);
    str_test_fail_alloc_after(0);
    EXPECT(str_replace_view(&s, TEST_SLICE_OFF, TEST_SPLIT_TRUNC,
                            str_view_from_cstr(long_replacement)) == STR_ERR_ALLOC);
    EXPECT(str_equals_cstr(&s, "abcdef"));
    EXPECT(str_status(&s) == STR_ERR_ALLOC);
    test_restore_alloc();
    str_deinit(&s);
}
#endif

static bool test_insert_overlap_matrix_result(bool reserve)
{
    static const char base[] = "0123456789abcde";
    char expected[TEST_INSERT_STORAGE];

    for (size_t src_off = 0; src_off <= TEST_INSERT_BASE_LEN; src_off++) {
        size_t src_limit = TEST_INSERT_BASE_LEN + 1 - src_off;
        for (size_t src_len = 0; src_len <= src_limit; src_len++) {
            for (size_t idx = 0; idx <= TEST_INSERT_BASE_LEN; idx++) {
                str_t s = STR_EMPTY;
                if (str_set_n(&s, base, TEST_INSERT_BASE_LEN) != STR_OK)
                    return false;
                if (reserve && str_reserve(&s, TEST_INSERT_RESERVE) != STR_OK)
                    return false;

                memcpy(expected, base, idx);
                memcpy(expected + idx, base + src_off, src_len);
                memcpy(expected + idx + src_len, base + idx, TEST_INSERT_BASE_LEN - idx);

                const char *src = str_cstr(&s) + src_off;
                if (str_insert_n(&s, idx, src, src_len) != STR_OK)
                    return false;
                size_t expected_len = TEST_INSERT_BASE_LEN + src_len;
                const char *actual = str_cstr(&s);
                if (str_len(&s) != expected_len)
                    return false;
                if (memcmp(actual, expected, expected_len) != 0)
                    return false;
                if (actual[expected_len] != '\0')
                    return false;
                str_deinit(&s);
            }
        }
    }
    return true;
}

static void test_insert_overlap_matrix(void)
{
    EXPECT(test_insert_overlap_matrix_result(false));
    EXPECT(test_insert_overlap_matrix_result(true));
}

static void test_internal_slack_rejected(void)
{
    str_t s = STR_EMPTY;

    EXPECT(str_reserve(&s, TEST_INSERT_STORAGE) == STR_OK);
    EXPECT(str_set(&s, "abcdefghijk") == STR_OK);
    EXPECT(str_set(&s, "abc") == STR_OK);
    const char *slack = s.buf + TEST_RESERVE_ARG;
    EXPECT(str_append_n(&s, slack, TEST_SPLIT_TRUNC) == STR_ERR_ARG);
    EXPECT(str_equals_cstr(&s, "abc"));

    str_clear_error(&s);
    EXPECT(str_append(&s, slack) == STR_ERR_ARG);
    EXPECT(str_equals_cstr(&s, "abc"));

    str_clear_error(&s);
    EXPECT(test_append_vfmt(&s, slack) == STR_ERR_ARG);
    EXPECT(str_equals_cstr(&s, "abc"));
    EXPECT(str_status(&s) == STR_ERR_ARG);

    str_clear_error(&s);
    const char *terminator = s.buf + s.len;
    EXPECT(test_append_vfmt(&s, terminator) == STR_OK);
    EXPECT(str_equals_cstr(&s, "abc"));

    EXPECT(str_replace_view(&s, 0, TEST_SPLIT_TRUNC, str_view_from_n(slack, 0)) == STR_OK);
    EXPECT(str_equals_cstr(&s, "bc"));
    str_deinit(&s);

    str_t fresh = STR_EMPTY;
    EXPECT(str_reserve(&fresh, TEST_INSERT_STORAGE) == STR_OK);
    EXPECT(str_set(&fresh, "abc") == STR_OK);
    const char *uninitialized_slack = fresh.buf + TEST_RESERVE_ARG;
    EXPECT(test_append_vfmt(&fresh, uninitialized_slack) == STR_ERR_ARG);
    EXPECT(str_equals_cstr(&fresh, "abc"));
    str_deinit(&fresh);
}

#ifdef STR_TEST
static void test_copy_failures(void)
{
    str_t src = STR_EMPTY;
    str_t dst = STR_EMPTY;

    EXPECT(str_set(&src, "0123456789abcdef") == STR_OK);
    EXPECT(str_set(&dst, "keep") == STR_OK);
    str_test_fail_alloc_after(0);
    EXPECT(str_copy(&dst, &src) == STR_ERR_ALLOC);
    EXPECT(str_equals_cstr(&dst, "keep"));
    EXPECT(str_status(&dst) == STR_ERR_ALLOC);
    EXPECT(str_copy(&dst, &dst) == STR_ERR_ALLOC);
    test_restore_alloc();
    str_deinit(&dst);

    EXPECT(str_append_n(&src, "x", SIZE_MAX) == STR_ERR_OVERFLOW);
    EXPECT(str_copy(&dst, &src) == STR_OK);
    EXPECT(str_equals_cstr(&dst, "0123456789abcdef"));
    str_deinit(&dst);
    str_deinit(&src);
}

static void test_format_oom_preserves(void)
{
    str_t s = STR_EMPTY;

    EXPECT(str_set(&s, "0123456789abcde") == STR_OK);
    str_test_fail_alloc_after(0);
    EXPECT(str_append_fmt(&s, "%s", "value") == STR_ERR_ALLOC);
    EXPECT(str_equals_cstr(&s, "0123456789abcde"));
    test_restore_alloc();

    str_clear_error(&s);
    str_test_fail_alloc_after(0);
    EXPECT(str_append_fmt(&s, "%300s", "x") == STR_ERR_ALLOC);
    EXPECT(str_equals_cstr(&s, "0123456789abcde"));
    test_restore_alloc();

    str_clear_error(&s);
    str_test_fail_alloc_after(1);
    EXPECT(str_append_fmt(&s, "%300s", "x") == STR_ERR_ALLOC);
    EXPECT(str_equals_cstr(&s, "0123456789abcde"));
    test_restore_alloc();
    str_deinit(&s);
}

static void test_detach_empty_oom(void)
{
    str_t s = STR_EMPTY;

    str_test_fail_alloc_after(0);
    EXPECT(str_detach(&s) == NULL);
    EXPECT(str_status(&s) == STR_ERR_ALLOC);
    EXPECT(str_is_empty(&s));
    test_restore_alloc();
    str_deinit(&s);
}
#endif

static void test_append_vfmt_api(void)
{
    str_t s = STR_EMPTY;

    EXPECT(test_append_vfmt(&s, "%s:%d", "value", TEST_SPLIT_CAP) == STR_OK);
    EXPECT(str_equals_cstr(&s, "value:4"));
    str_deinit(&s);
}

static void test_split_edges(void)
{
    const char binary[] = {'a', '\0', 'b', '\0'};
    str_view_t parts[TEST_SPLIT_CAP] = {0};
    str_split_out_t out = {.parts = parts, .cap = TEST_SPLIT_CAP, .count = 0};

    EXPECT(str_split_view(str_view_from_n(NULL, 0), &out, ',') == STR_OK);
    EXPECT(out.count == 1);
    EXPECT(out.parts[0].len == 0);
    EXPECT(str_split_view(str_view_from_cstr(",a,"), &out, ',') == STR_OK);
    EXPECT(out.count == TEST_EMBEDDED_LEN);
    EXPECT(out.parts[0].len == 0);
    EXPECT(out.parts[1].len == 1);
    EXPECT(out.parts[2].len == 0);

    EXPECT(str_split_view(str_view_from_n(binary, sizeof(binary)), &out, '\0') == STR_OK);
    EXPECT(out.count == TEST_EMBEDDED_LEN);
    EXPECT(out.parts[0].len == 1 && out.parts[0].ptr[0] == 'a');
    EXPECT(out.parts[1].len == 1 && out.parts[1].ptr[0] == 'b');
    EXPECT(out.parts[2].len == 0);

    out.parts = NULL;
    out.cap = 0;
    EXPECT(str_split_view(str_view_from_cstr("a,b,c"), &out, ',') == STR_OK);
    EXPECT(out.count == TEST_EMBEDDED_LEN);
}

static void test_detach_owned(void)
{
    str_t s = STR_EMPTY;

    EXPECT(str_set(&s, "owned") == STR_OK);
    char *owned = str_detach(&s);
    EXPECT(owned != NULL);
    EXPECT(strcmp(owned, "owned") == 0);
    EXPECT(str_is_empty(&s));
    EXPECT(str_capacity(&s) == 0);
    free(owned);
    str_deinit(&s);
}

static void test_search_exhaustive(void)
{
    EXPECT(test_search_exhaustive_result());
    EXPECT(test_search_random_result());
}

static void test_run_core(void)
{
    test_zero_init_cstr();
    test_append_grows();
    test_embedded_nul();
    test_append_overlap();
    test_cstr_self_overlap();
    test_insert_overlap();
    test_insert_overlap_crosses_gap();
    test_insert_own_terminator();
#ifdef STR_TEST
    test_oom_preserves();
#endif
    test_overflow_size();
    test_deinit_twice();
    test_move();
    test_copy();
    test_copy_obeys_sticky_error();
    test_find();
    test_slice();
    test_split();
    test_append_fmt();
    test_append_fmt_self_alias();
    test_detach_empty();
    test_null_mutator();
    test_invalid_sources();
    test_null_outputs();
    test_view_trim();
    test_clear();
    test_remove_insert();
    test_init_explicit();
}

static void test_run_extended(void)
{
    test_status_names();
    test_view_bridges();
    test_view_queries();
    test_invalid_views();
    test_capacity_resize_shrink();
#ifdef STR_TEST
    test_shrink_oom_preserves();
#endif
    test_replace_view();
#ifdef STR_TEST
    test_replace_oom_preserves();
#endif
    test_insert_overlap_matrix();
    test_internal_slack_rejected();
#ifdef STR_TEST
    test_copy_failures();
    test_format_oom_preserves();
    test_detach_empty_oom();
#endif
    test_append_vfmt_api();
    test_split_edges();
    test_detach_owned();
    test_search_exhaustive();
}

int main(void)
{
    test_run_core();
    test_run_extended();
    printf("ok %d\n", test_ctx.run);
    return 0;
}
