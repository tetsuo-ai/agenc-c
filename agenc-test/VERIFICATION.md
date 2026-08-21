# agenc-test verification plan

Verification is evidence. A claim of done comes with the ledger filled
in; a skipped check is reported as skipped, never as passing.

## Build matrix

The self-test source builds and runs in each variant, under gcc and
clang. `make check` runs the lot.

| Target | Configuration | Purpose |
| --- | --- | --- |
| make test | -std=c11 -pedantic-errors, the agenc-str warning set | primary self-test |
| make asan | -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all | memory and UB evidence |
| make iso | -O2 | optimized strict build |
| make release | -O3 -DNDEBUG -D_FORTIFY_SOURCE=3 | production optimization |
| make meta | the deliberately-failing binary | failure-detection and teardown-after-fatal proof |

## What the self-tests cover

- Every scalar assertion (EQ/NE/LT/LE/GT/GE, TRUE/FALSE) including a
  mixed-width size_t vs int comparison, which exercises the _Generic
  format selection.
- String (STR_EQ/NE), memory (MEM_EQ with a byte-diff report), float
  (NEAR with a tolerance), pointer (NULL/NOT_NULL/PTR_EQ) assertions.
- Message-attaching variants.
- Skip as a first-class outcome.
- The harness PRNG: two draws differ and a bounded draw stays in range;
  the seed is printed and settable.
- A fixture with setup and teardown.
- A death test: an abort is caught by the fork and reported as a pass.

## The meta proof (failure detection)

A separate binary (tests/meta_fail.c) holds deliberately failing tests
and is never part of the passing suite. The `make meta` target runs it
and asserts three things, so the target itself fails if any regress:

- the binary exits nonzero (a harness that always exits 0 is worthless);
- it reports exactly three failed tests;
- it prints the fixture teardown marker, proving teardown runs after a
  fatal assertion in the body.

This is the revert-sensitive proof of the two properties a test harness
must have: it detects failure, and its cleanup survives a fatal assert.

## Isolation and crash containment

Verified by hand this session and reproducible: a build with a
deliberately segfaulting test, run with `--isolate`, contains the crash,
reports it as `not ok ... crashed: signal 11`, continues to the next
test, and exits nonzero. Without `--isolate` the signal handler names
the crashing test on stderr before the process dies.

## Registration under dead-strip and multiple TUs

- Tests in a second translation unit are discovered: tests/test_more.c
  is linked into the self-test binary and its tests run, proving the
  section sweep spans translation units (every multi-file suite needs
  this).
- A build with `-ffunction-sections -fdata-sections -Wl,--gc-sections`
  still finds every test, proving the `used`/`retain` guard holds.

## Ledger, 2026-08-21

Toolchain: gcc 13.3.0, clang 18.1.3 on x86-64 Linux.

| Evidence | State |
| --- | --- |
| clang-format gate | passed |
| GCC c11 pedantic build + self-test | passed |
| Clang c11 pedantic build + self-test | passed |
| ASan + UBSan | passed, both compilers |
| ISO (-O2) build | passed, both compilers |
| Release NDEBUG FORTIFY | passed, both compilers |
| Meta: failure detection + teardown-after-fatal | passed, both compilers |
| --gc-sections registration survival | passed |
| --isolate crash containment | passed (manual, reproducible) |
| Death test (SIGABRT) | passed |
| Multi-TU registration | passed (tests/test_more.c is a second TU discovered by the section sweep) |
| MSVC and Mach-O registration paths | unavailable on this machine; code paths written, covered by the manual fallback and the ledger |
| 32-bit / big-endian | unavailable (no multilib libc here); the code takes no layout assumptions |
| Fuzzing | not applicable (a test harness parses no untrusted input; its own inputs are test declarations, not attacker data) |

## Notes

- In `--isolate` mode the per-assertion count reads 0 in the summary:
  assertion tallies live in the child and do not cross the fork; the
  pass/fail verdict comes from the child's exit status, which is the
  property that matters. Documented, not a defect.
- The harness allocates nothing on the test path (static registry, fixed
  diagnostic buffer, stack fixtures). The JUnit writer opens a file only
  when `--xml` is given.
