# C Libraries: what to build, in build order

Shared C libraries meant to be dropped into every C codebase we start.
A string library already exists, so it is not on the list; everything
below should integrate with it (consume and produce its slice/view type
instead of inventing a second one).

Ordering rule: each library may only depend on libraries above it.

Folder naming follows the existing string library (agenc-str): one
agenc-* sub-folder per library, listed with each entry below.

## 1. Arena allocator + allocator interface (agenc-arena)

The base layer. A region allocator with reset semantics, plus a small
allocator vtable (alloc/realloc/free function pointers and a context
pointer) that every later library takes as a parameter instead of
calling malloc.

Includes: growing arena, fixed-buffer backend, temp/scratch scopes
(begin/end mark), a libc-malloc backend for the vtable.

Why first: once this exists, every other library is written against it
and gets no-heap and embedded use for free. Retrofitting it later means
rewriting every API.

Done when: all later libraries allocate only through the vtable, and a
fixed-buffer arena can run the test suite with the heap disabled.

## 2. Test harness (agenc-test)

Single header: TEST macro, assertion macros that print both values on
failure, a runner with filtering, exit code for CI.

Why second: everything after this gets tests from day one, in the same
style, in every repo. Keep it dependency-free (not even the arena) so
it can test the arena itself.

## 3. Error handling (agenc-err)

A small error type (numeric code plus optional context), a stable code
space, and propagate/cleanup macros so goto-err blocks are uniform.
Generalize the modelvet 34-code ABI approach: codes are stable numbers
fit for an ABI boundary, strings are for humans only.

Why third: every library below returns these. Library code never calls
exit or abort.

## 4. Bounds-checked binary reader/writer (agenc-bin)

A cursor over a byte slice: read_u8/u16/u32/u64 (le/be), read_bytes,
peek, skip, align. All reads fail softly (cursor enters a failed state,
returns zeroes) and never touch memory past the end. Writer mirrors it.

Depends on: the agenc-str slice type, error codes.

Note (do not forget): str_view_t already gives us the slice type this
plan calls for. The agenc-bin reader takes str_view_t (or a byte-view
twin of it, uint8_t-based, convertible both ways) rather than inventing
a new span struct. That keeps the whole family interoperable: anything
str can view, bin can parse, and ds_map_ can key on.

Why fourth: this is the core of parsing any untrusted input (GGUF,
file headers, network frames) and the thing we least want hand-rolled
per format. Ship a fuzz-entry-point convention with it so every parser
built on it gets a fuzz target almost free.

## 5. Hash functions + PRNG (agenc-hash)

One fast non-crypto hash (xxhash-style), SipHash with a per-process
random key for hashmaps that see untrusted keys (HashDoS resistance),
and a PCG-style PRNG.

Why before containers: the hashmap needs these; done once, reused
forever.

## 6. Containers (agenc-ds)

Exactly three to start: a dynamic array (typed, macro-based or
stb-style), an open-addressing hashmap keyed on slices, and an
intrusive doubly linked list. These cover 95% of real use; resist
adding more until something is missed three times.

Depends on: arena, hash, error codes.

## 7. Logging (agenc-log)

Leveled logging that writes to a caller-supplied sink function, never
directly to stderr. Compile-time minimum level so release builds can
drop debug logging entirely. One install call, no other global state.

## 8. OS/platform shim (agenc-os)

read-entire-file, mmap wrapper, path join/normalize, monotonic clock,
directory iteration, thread/mutex wrappers over pthreads and win32.

Why late: boring but load-bearing; by now the APIs above tell us
exactly what shapes it needs to return (slices, arena-owned buffers,
error codes).

## 9. CLI argument parser + config reader (agenc-cli)

Small declarative flag parser, and a strict fail-closed JSON-subset
reader built on the binary/text cursor. Allocation-controlled, hostile
input assumed.

Why last: it is a consumer of everything above and forces nothing on
the lower layers.

## Conventions (apply to every library)

Set by agenc-str, which is the reference implementation for style:

- Layout: include/<name>.h, src/<name>.c, tests/, Makefile, README.md,
  LICENSE (MIT), .clang-format copied from agenc-str. C11, no
  third-party dependencies. Each library is a standalone folder in the
  tetsuo-ai/agenc-c monorepo; the root LICENSE and .clang-format are
  canonical and every library carries a byte-identical copy (enforced by
  the root make conventions gate). Per-library read-only mirror repos
  can be added later without restructuring anything.
- Adoption is "copy the header and the .c file", never "adopt a build
  system". The Makefile exists only to build tests and demos.
- Symbol prefix is the short library name (str_, arena_, err_, bin_,
  hash_, log_, os_, cli_; containers use ds_vec_/ds_map_/ds_list_).
  The agenc- prefix is for folder/repo names only.
- Status/error style follows str: a <name>_status_t enum returned from
  fallible calls, sticky error state on stateful objects so mutators
  can be chained and checked once, failure-atomic mutation (a failed
  call leaves existing data valid), str_status_name-style string lookup.
- Tests build in plain, ASan/UBSan, ISO-pedantic, and release variants
  like agenc-str's tests/ folder.
- All allocation through a passed-in allocator. No hidden malloc.
- No global mutable state, no threads spawned, no signal handlers.
- All inputs are pointer+length. All parsers assume hostile input.
- Errors are return values. No exit/abort in library code; asserts
  compile out of release builds.
- Every library ships with tests (harness #2) and, where it parses
  input, a fuzz target (convention from #4).
