# `str`

> Predictable, growable strings for C11, where failed writes leave good data untouched.

![C11](https://img.shields.io/badge/C-C11-00599C?logo=c&logoColor=white)
![Third-party dependencies: none](https://img.shields.io/badge/third--party_dependencies-none-2ea44f)
![Checked with ASan and UBSan](https://img.shields.io/badge/checked-ASan_%7C_UBSan-8a2be2)
![License: MIT](https://img.shields.io/badge/license-MIT-yellow.svg)

`str` is a small byte-string library built for the parts of C string handling that tend to go
wrong: allocation failure, overlapping writes, partial mutation, embedded NUL bytes, and lost
error returns. Values live on the stack, buffers grow on demand, and every owned string remains
NUL-terminated for read-only C-string APIs.

## Why `str`?

| Design choice | What it gives you |
| --- | --- |
| Failure-atomic mutation | A failed write preserves the bytes, length, capacity, and ownership that were already valid. |
| Sticky errors | Chain several mutators, then check once; later writes become no-ops after the first failure. |
| Overlap-safe writes | Set, append, insert, and replace may read from the destination string itself. |
| Binary-safe views | Work with borrowed spans and embedded NUL bytes without allocating or scanning for a terminator. |
| Linear-time search | First and last substring search use constant extra space and linear worst-case time. |
| Stack-owned values | `str_t` needs no separate object allocation and has explicit copy, move, detach, and cleanup operations. |

There are no third-party dependencies. The implementation uses C11 and the standard C library.

## Quick start

Copy [`include/str.h`](include/str.h) and [`src/str.c`](src/str.c) into your project, then compile
them with your application:

```sh
cc -std=c11 -Wall -Wextra -Iinclude app.c src/str.c -o app
```

A complete program:

```c
#include <stdio.h>

#include "str.h"

int main(void)
{
    str_t message = STR_EMPTY;

    str_append(&message, "hello");
    str_append_fmt(&message, ", %s", "world");
    if (str_failed(&message)) {
        fprintf(stderr, "%s\n", str_status_name(str_status(&message)));
        str_deinit(&message);
        return 1;
    }

    puts(str_cstr(&message));
    str_deinit(&message);
    return 0;
}
```

```text
hello, world
```

Run `make demo` for a printable walkthrough of editing, self-appends, search, views, splitting,
ownership transfer, sticky-error recovery, embedded NUL bytes, and capacity management.

## The programming model

### Own strings with `str_t`

Initialize a string with `STR_EMPTY`, zero-initialization, or one call to `str_init` on
uninitialized storage. Release it with `str_deinit`:

```c
str_t name = STR_EMPTY;

if (str_set(&name, "Ada") != STR_OK) {
    str_deinit(&name);
    return 1;
}

printf("%s is %zu bytes\n", str_cstr(&name), str_len(&name));
str_deinit(&name);
```

`str_t` is visible so it can live on the stack, but its fields are read-only to callers. Never
assign one `str_t` to another: use `str_copy` for a deep copy or `str_move` to transfer ownership.

### Borrow bytes with `str_view_t`

A view is a non-owning `{ pointer, length }` pair. It can describe a slice, binary input, or a C
string without allocating:

```c
str_view_t padded = str_view_from_cstr("  hello world  ");
str_view_t trimmed = str_view_trim(padded);

printf("%.*s\n", (int)trimmed.len, trimmed.ptr);
```

Views borrow storage. Any successful mutation, reserve, shrink, move, detach, or deinit may
invalidate views and pointers into that string.

### Chain mutations with sticky errors

The first mutator failure is stored on the string. Further mutators return that status without
changing the content, so related writes can share one check:

```c
str_append(&message, "request=");
str_append(&message, request_id);
str_append_char(&message, '\n');

if (str_failed(&message)) {
    fprintf(stderr, "build failed: %s\n", str_status_name(str_status(&message)));
}
```

Use `str_clear_error` to retry while preserving the content, or `str_clear` to empty the string
and clear its status. Queries and cleanup remain available while an error is sticky.

### Read from the destination safely

Self-sources are supported through the string's existing terminator, even when growth moves the
buffer:

```c
str_t word = STR_EMPTY;

str_set(&word, "echo");
str_append_n(&word, str_cstr(&word), str_len(&word));
if (str_failed(&word)) {
    str_deinit(&word);
    return 1;
}

puts(str_cstr(&word)); /* echoecho */

str_deinit(&word);
```

Spare capacity is not initialized content and cannot be used as a source.

## API at a glance

| Area | Operations |
| --- | --- |
| Lifecycle | `str_init`, `str_deinit`, `str_clear`, `str_clear_error` |
| Ownership | `str_copy`, `str_move`, `str_detach` |
| Build | `str_set*`, `str_append*`, `str_append_char`, `str_append_fmt`, `str_append_vfmt` |
| Edit | `str_insert*`, `str_remove`, `str_replace_view`, `str_resize` |
| Capacity | `str_reserve`, `str_shrink_to_fit`, `str_capacity` |
| Observe | `str_cstr`, `str_len`, `str_is_empty`, `str_status`, `str_ok`, `str_failed` |
| Compare | `str_equals*`, `str_starts_with`, `str_ends_with`, `str_view_compare` |
| Search | `str_find`, `str_find_char`, `str_view_find`, `str_view_rfind`, `STR_NPOS` |
| Views | `str_view`, `str_slice`, `str_view_from_*`, `str_view_trim`, `str_view_*` queries |
| Split | `str_split_view` with caller-owned output storage |

The public header is the complete API reference; every function documents its inputs, ownership,
invalidation rules, and failure modes in [`include/str.h`](include/str.h).

## Status values

| Status | Meaning |
| --- | --- |
| `STR_OK` | Success |
| `STR_ERR_ARG` | Invalid object, pointer, view, or other argument |
| `STR_ERR_ALLOC` | Allocation failed |
| `STR_ERR_RANGE` | A requested span lies outside the current content |
| `STR_ERR_OVERFLOW` | A size calculation cannot be represented by `size_t` |
| `STR_ERR_FMT` | Formatted output could not be produced consistently |

`str_status_name` returns the symbolic name of any status value.

## Important contracts

- Lengths, indices, and capacities count bytes, not UTF-8 code points. The library does not decode,
  validate, or normalize Unicode.
- `str_cstr` always returns a terminated pointer, but embedded NUL bytes may appear before the
  final terminator. Use `str_len` and length-aware I/O for binary content.
- `str_cstr`, `str_len`, and `str_is_empty` accept `NULL` and observe it as empty. Mutators reject
  `NULL` with `STR_ERR_ARG`.
- A failed mutator preserves content and allocation state; the sticky status is the intentional
  state change.
- Formatted append stages its output before mutation. A format string and read-only pointer
  arguments may borrow from the destination; a `%n` destination must not overlap it.
- Use a fixed format string for untrusted input, such as `str_append_fmt(&s, "%s", input)`.
- Independent strings may be used concurrently. Synchronize access when threads share a string or
  a view into it and any thread may mutate the owner.

## Build and verify

Requirements: a C11 compiler, `make`, and `clang-format`. Sanitizer support is required for the
sanitizer target.

| Command | Purpose |
| --- | --- |
| `make demo` | Build and run the public-API walkthrough |
| `make test` | Run strict-warning, deterministic, exhaustive, and seeded tests |
| `make asan` | Run the suite with AddressSanitizer and UndefinedBehaviorSanitizer |
| `make iso` | Run the suite with the strictly conforming `STR_STRICT_ISO_OVERLAP` scan |
| `make release` | Run an optimized `NDEBUG`/FORTIFY build |
| `make format` | Check the repository's clang-format rules |
| `make check` | Run formatting, test, sanitizer, and release checks |
| `make clean` | Remove generated binaries |

The test suite covers allocation-failure injection, mutation atomicity, overlap matrices,
lifecycle behavior, binary data, formatting, views, and exhaustive plus seeded search cases.

## Contributing

Keep changes C11-compatible, format with the repository's [`.clang-format`](.clang-format), and
run `make check` before submitting a change. New behavior should include focused regression tests
that exercise both success and failure paths.

## License

Licensed under the [MIT License](LICENSE). Copyright (c) 2026 TETSUO.AI INC.
