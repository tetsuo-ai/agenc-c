# agenc-test research survey

Findings behind the implementation. Three research passes on 2026-08-21
covered practitioner single-header C test frameworks, the modern C11
implementation techniques, and the testing methodology and output
protocols. Every design choice below traces to one of them.

## 1. Prior art

| Framework | Single-header | Registration | Value printing | Isolation | Allocates | Machine output |
| --- | --- | --- | --- | --- | --- | --- |
| greatest | yes | manual RUN | format-string / callback | none (setjmp opt) | no | none |
| utest.h | yes | constructor + realloc | _Generic format | none | yes | JUnit XML |
| tau | yes | constructor / .CRT$XCU | _Generic format | none | yes | XML |
| snow | yes | constructor | _Generic function | none | yes | none |
| munit | header + .c | manual arrays | typed per-macro | fork (Unix) | yes | none |
| Unity | 3 files | manual + Ruby generator | huge typed family | setjmp only | no | via Ceedling |
| cmocka | header + .c | manual arrays | intmax casts | setjmp + signals | yes | TAP + XML |
| Criterion | linked lib | linker-section table | stringify + runtime | process per test | yes | TAP + XML + JSON |
| Check | linked lib | manual wiring | typed macros | fork per test | yes | XML + TAP |

## 2. The three C handicaps every framework works around

C lacks the three C++ features the reference frameworks (GoogleTest,
Catch2) lean on:

- No operator overloading, so a value cannot be printed from its static
  type and `REQUIRE(a==b)` cannot decompose to `0 == 1`. Substitutes:
  C11 `_Generic` to pick a printf format, hardcoded typed macros, or a
  caller-supplied format string. Expression decomposition is impossible
  in C.
- No templates, so no type-parameterized tests. Substitute: index loops
  and comparator callbacks.
- No portable static-init registration. Substitutes: constructors
  (gcc/clang), the MSVC `.CRT$XCU` init group, linker-section tables, or
  manual listing. None is standard.

## 3. Decisions and their grounding

Registration: a linker-section descriptor table (Criterion's mechanism),
not the constructor-append model. Constructor registration (utest.h, tau,
snow) grows a heap array with realloc; a harness that tests allocators
must not allocate to register tests. The section table is laid out at
link time, walked between `__start_`/`__stop_` boundary symbols, with
`used` plus `retain` (gcc >= 11, clang >= 13) defeating `--gc-sections`.
This is the only mechanism that gives auto-registration with zero
allocation. MSVC uses bracket sections (`$a`/`$m`/`$z`) with
`__declspec(allocate)` and a `/include:` linker directive; Mach-O uses
`getsectiondata`. A manual fallback covers other toolchains.

Value printing: `_Generic` selects a printf conversion by operand type
(utest.h), with the `(v)-(v)` which is `ptrdiff_t` for pointers
collapsing every pointer type to `%p`. `size_t` cannot have its own
association (it is a typedef for a listed integer type, and duplicating
it is a constraint violation), so it falls through the underlying types.
String literals decay to `char *`, so a dedicated `ASSERT_STR_EQ` does
`strcmp`, never `_Generic`. NULL is not `_Generic`-safe, so pointer and
NULL checks are dedicated. `__typeof__` snapshots each operand once,
closing the multiple-evaluation hole that pure C11 cannot.

Assertion split: fatal `ASSERT_*` and non-fatal `EXPECT_*` (GoogleTest).
Fatal is the guard-assertion primitive that stops a test before a NULL
dereference; non-fatal lets one test report several failures, which
fights assertion roulette (van Deursen test-smell catalog). Both print
both operands, the single most-cited assertion usability property.

Fixtures: setup and teardown around each test, with teardown registered
so it runs even after a fatal assertion. A fatal assertion returns from
the test body and would skip trailing cleanup (the GoogleTest space-leak
caveat); putting the setjmp landing pad in the test wrapper and calling
teardown after it fixes this. Teardown is skipped when setup itself
failed, so it never sees a half-built fixture (the Four-Phase Test).

Isolation: default in-process, opt-in `--isolate` fork per test. Fork
contains a crash or a global-state mutation and attributes it to the
test (munit, Criterion, Check), which matters for allocators and parsers
and attacks the order-dependency flakiness that Luo et al. (FSE 2014)
found dominant. A signal handler names a crashing test even in-process,
using only async-signal-safe writes. Death tests (`TEST_SIGNAL`) fork
and pass on the expected terminating signal.

Randomness: a harness-owned PCG, seeded, printed, `--seed`-settable
(munit). This makes the seeded op-sequence tests the sibling libraries
already run reproducible, at zero dependency cost. No built-in
property-based engine: generators and shrinking are a separable concern
(theft), kept orthogonal.

Output: TAP version 14 by default (a streaming printf format: version
line, `1..N` plan, `ok`/`not ok N - desc`, `#` diagnostics, `Bail
out!`), plus JUnit XML behind `--xml` for CI annotations. The one
non-negotiable CI contract is the exit code, which GitHub Actions
consumes with no config and mutation tools key on.

Deliberately omitted: a property-based engine, a mock framework (systems
C libraries are state-verified, not interaction-verified), a built-in
mutation engine (the harness only needs the exit-code contract to
facilitate one), and enforced one-assertion-per-test (superseded by
one-behavior-per-test).

## Bibliography

Practitioner sources:
- greatest, https://github.com/silentbicycle/greatest
- utest.h, https://github.com/sheredom/utest.h
- munit, https://github.com/nemequ/munit
- Unity, https://github.com/ThrowTheSwitch/Unity
- cmocka, https://github.com/clibs/cmocka
- Criterion, https://github.com/Snaipe/Criterion
- tau, https://github.com/jasmcaus/tau
- snow, https://github.com/mortie/snow
- acutest, https://github.com/mity/acutest
- minunit, https://github.com/siu/minunit and Jera JTN002
  http://jera.com/techinfo/jtns/jtn002
- Check, https://github.com/libcheck/check

Techniques and standards:
- C11 _Generic, https://en.cppreference.com/c/language/generic; WG14
  N1930 (DR 481, string-literal type),
  https://www.open-std.org/jtc1/sc22/wg14/www/docs/n1930.htm
- Linker sections, dead-strip, retain: MaskRay,
  https://maskray.me/blog/2021-02-28-linker-garbage-collection
- utest.h WPO / MSVC sections,
  https://www.neilhenning.dev/posts/utest-h-supports-wpo/
- stb single-header convention,
  https://github.com/nothings/stb/blob/master/docs/stb_howto.txt
- TAP 14 spec, https://testanything.org/tap-version-14-specification.html
- JUnit XML, https://gaffer.sh/blog/junit-xml-format-guide/

Methodology:
- Meszaros, xUnit Test Patterns, http://xunitpatterns.com/
- GoogleTest Primer (fatal vs non-fatal, both-operands),
  http://google.github.io/googletest/primer.html
- Claessen and Hughes, QuickCheck (ICFP 2000),
  https://www.cse.chalmers.se/~rjmh/QuickCheck/
- theft, C property testing, https://github.com/silentbicycle/theft
- Just et al., mutants vs real faults (FSE 2014),
  https://dl.acm.org/doi/10.1145/2635868.2635929
- Luo et al., an empirical analysis of flaky tests (FSE 2014),
  https://www.researchgate.net/publication/301428664
- van Deursen et al., refactoring test code (test smells),
  https://www.researchgate.net/publication/2534882
