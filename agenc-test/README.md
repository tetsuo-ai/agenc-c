# `test`

> A single-header C11 unit test harness: auto-registered tests, typed
> value-printing assertions, fixtures, fork isolation, and TAP output,
> with no dependencies and no allocation.

![C11](https://img.shields.io/badge/C-C11-00599C?logo=c&logoColor=white)
![Third-party dependencies: none](https://img.shields.io/badge/third--party_dependencies-none-2ea44f)
![Checked with ASan and UBSan](https://img.shields.io/badge/checked-ASan_%7C_UBSan-8a2be2)
![License: MIT](https://img.shields.io/badge/license-MIT-yellow.svg)

`test` is the test harness for the agenc-* C family. Tests register
themselves at link time, so there is no run list to maintain, and the
harness allocates nothing: it can test an allocator without depending on
it or on malloc. Assertions come in fatal and non-fatal flavors, print
both operands with the right format for their type, and a test binary
exits nonzero if anything failed.

## Quick start

Copy [`include/test.h`](include/test.h) into your project. In one file,
define the implementation and a main; write tests anywhere:

```c
#define TEST_IMPLEMENTATION
#include "test.h"

TEST(math, adds)
{
    ASSERT_EQ(2 + 2, 4);
    EXPECT_TRUE(2 < 3);
}

TEST_MAIN()
```

```sh
cc -std=c11 -Iinclude app_test.c -o app_test && ./app_test
```

```text
TAP version 14
# seed 11400714819323198485
ok 1 - math.adds
1..1
# 1 passed, 0 failed, 0 skipped, 2 assertions
```

Tests in several files link into one binary; only one file defines
`TEST_IMPLEMENTATION` and `TEST_MAIN`.

## The model

### Declaring tests

`TEST(suite, name) { ... }` defines a test. It registers itself through a
linker-section table the runner walks at startup, in file and line order.
No `RUN_TEST` calls, no generator, no allocation.

On toolchains without the section mechanism, register manually with
`TEST_REGISTER(suite, name);` from a setup function and drop the default
main.

### Assertions

`ASSERT_*` are fatal: a failure prints its detail, aborts the current
test, and the runner continues with the next test. `EXPECT_*` are
non-fatal: a failure is recorded and the test keeps going, so one test
can report several failures at once.

| Assertion | Checks |
| --- | --- |
| `ASSERT_TRUE(c)` / `ASSERT_FALSE(c)` | boolean condition |
| `ASSERT_EQ/NE/LT/LE/GT/GE(a, b)` | scalar comparison, both values printed |
| `ASSERT_STR_EQ/NE(a, b)` | `strcmp`, both strings printed |
| `ASSERT_MEM_EQ(a, b, n)` | `memcmp`, first differing byte printed |
| `ASSERT_NEAR(a, b, eps)` | float within a tolerance |
| `ASSERT_NULL/NOT_NULL(p)` | pointer against NULL |
| `ASSERT_PTR_EQ(a, b)` | pointer identity |

Every `ASSERT_*` has an `EXPECT_*` twin. The comparison macros print both
operands with the format their type calls for, chosen at compile time
with `_Generic`. Scalar assertion arguments must be side-effect-free (the
macro reads each once through `__typeof__`). Add context with the message
variants, for example `ASSERT_EQ_MSG(got, want, "case %d", i)`.

### Fixtures

Declare a struct, a setup, and an optional teardown over it; each test
gets a zero-initialized instance named `fixture`. Teardown runs even
after a fatal assertion in the body, and is skipped if setup itself
failed, so it never sees a half-built fixture.

```c
struct db { conn_t *c; };
TEST_F_SETUP(db)    { fixture->c = conn_open(); }
TEST_F_TEARDOWN(db) { conn_close(fixture->c); }
TEST_F(db, queries) { ASSERT_NOT_NULL(fixture->c); }
```

### Isolation and death tests

By default tests run in-process. Pass `--isolate` to fork each test: a
crash or global-state mutation is contained, attributed to the test, and
the run continues. A signal handler names a crashing test even without
`--isolate`.

`TEST_SIGNAL(suite, name, SIGABRT) { ... }` is a death test: it always
forks and passes only if the body terminates by that signal. This is how
you test that a contract violation aborts.

### Reproducible randomness

`tt_rand()` and `tt_rand_below(bound)` are a harness-owned PRNG. The seed
is printed on every run and set with `--seed=N`, so a randomized test
replays exactly.

## Command line

| Flag | Effect |
| --- | --- |
| `--filter=STR` | run tests whose `suite.name` contains STR |
| `--list` | print test names and exit |
| `--seed=N` | fix the PRNG seed |
| `--isolate` | fork each test (crash containment) |
| `--xml=FILE` | also write a JUnit report |
| `--fail-fast` | stop after the first failing test |
| `--no-color` | disable color |

Output is TAP version 14 on stdout, consumed by `prove` and CI. Exit code
is 0 when every test passes, 1 otherwise.

## Build and verify

| Command | Purpose |
| --- | --- |
| `make test` | build and run the self-tests |
| `make asan` | run under ASan and UBSan |
| `make iso` | run the optimized strict-conformance build |
| `make release` | run the optimized NDEBUG build |
| `make meta` | prove the harness detects failure and runs teardown after a fatal assert |
| `make check` | format, then all of the above |

## License

Licensed under the [MIT License](LICENSE). Copyright (c) 2026 TETSUO.AI INC.
