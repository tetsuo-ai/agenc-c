#ifndef AGENC_TEST_H_INCLUDED
#define AGENC_TEST_H_INCLUDED

/*
 * test.h: a single-header C11 unit test harness for the agenc-* family.
 *
 * Adoption: copy this one header. In exactly one translation unit,
 * define TEST_IMPLEMENTATION before including it; that TU gets the runner
 * and main(). Every other test file includes the header and writes
 * TEST(suite, name) { ... } blocks.
 *
 *   #define TEST_IMPLEMENTATION
 *   #include "test.h"
 *
 *   TEST(math, adds)
 *   {
 *       ASSERT_EQ(2 + 2, 4);
 *       EXPECT_TRUE(2 < 3);
 *   }
 *   TEST_MAIN()
 *
 * Tests register themselves through a linker-section descriptor table,
 * so there is no manual run list and the harness allocates nothing: it
 * can test an allocator without depending on it or on malloc.
 *
 * ASSERT_* are fatal: a failure aborts the current test (the rest of its
 * body does not run) and the runner moves to the next test. EXPECT_* are
 * non-fatal: a failure is recorded and the test continues, so one test
 * can report several failures. A test binary exits 0 when every test
 * passes and 1 when any test fails.
 *
 * Output is TAP version 14 on stdout. Command-line flags: --filter=STR
 * (run tests whose "suite.name" contains STR), --list, --seed=N,
 * --isolate (fork each test so a crash is contained and attributed),
 * --xml=FILE (also write a JUnit report), --fail-fast, --no-color.
 *
 * C11 is required for the _Generic value printing. gcc and clang are the
 * primary targets; the registration mechanism has MSVC and Mach-O paths
 * and a manual fallback (TEST_REGISTER) for other toolchains.
 */

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Test descriptor flags. */
enum {
    TT_FLAG_NONE = 0u,
    /* The test is expected to terminate by a signal (a death test); it
     * always forks and passes only if the child dies by tt_test.signal. */
    TT_FLAG_SIGNAL = 1u
};

/*
 * One registered test. The TEST macros construct these; a test author
 * never fills one in directly. signal is meaningful only when flags has
 * TT_FLAG_SIGNAL set.
 */
typedef struct tt_test {
    const char *suite;
    const char *name;
    const char *file;
    int line;
    void (*fn)(void);
    unsigned flags;
    int signal;
} tt_test_t;

/*
 * The state of one run, a single instance the runner owns. It is exposed
 * so the assertion macros, which expand inside test functions, can reach
 * the current-test failure flag and the fatal-assert landing pad. Do not
 * write these fields from test code.
 */
typedef struct tt_state {
    unsigned tests_run;
    unsigned tests_passed;
    unsigned tests_failed;
    unsigned tests_skipped;
    unsigned assertions;
    const tt_test_t *current;
    int current_failed;
    int have_jmp;
    jmp_buf test_jmp;
} tt_state_t;

extern tt_state_t tt_g;

/*
 * Output indirection. Every byte the harness prints to its result stream
 * goes through TT_PRINTF, so a freestanding target can redirect it by
 * defining TT_PRINTF before including this header.
 */
#ifndef TT_PRINTF
#include <stdio.h>
#define TT_PRINTF(...) fprintf(stdout, __VA_ARGS__)
#endif

/*
 * Registration. Each TEST places a pointer to its descriptor into a
 * named section; the runner walks the section between the linker's
 * boundary symbols. Pointers, not the descriptors, live in the section
 * so the walk stays a uniform pointer array regardless of struct
 * padding. used plus retain (gcc >= 11, clang >= 13) keeps the pointer
 * through --gc-sections.
 */
#if defined(__GNUC__) || defined(__clang__)
#if (defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 11) ||                                \
    (defined(__clang_major__) && __clang_major__ >= 13)
#define TT_KEEP __attribute__((used, retain))
#else
#define TT_KEEP __attribute__((used))
#endif
#if defined(__APPLE__)
#define TT_SECTION __attribute__((section("__DATA,agenctst")))
#else
#define TT_SECTION __attribute__((section("agenctst")))
#endif
#define TT_REGISTER(rec) static const tt_test_t *const rec##_ptr TT_KEEP TT_SECTION = &rec
#define TT_HAVE_AUTO_REGISTER 1
#else
#define TT_KEEP
#define TT_SECTION
#define TT_REGISTER(rec) /* no auto-registration; use TEST_REGISTER */
#endif

/*
 * A plain test. The generated wrapper owns the setjmp landing pad so a
 * fatal assertion returns here and the runner keeps going; the body is a
 * separate function so a fatal assert in a helper it calls still unwinds
 * to the same pad.
 */
#define TT_MAKE_TEST(suite, name, flagbits, signum)                                                \
    static void tt_body_##suite##_##name(void);                                                    \
    static void tt_fn_##suite##_##name(void)                                                       \
    {                                                                                              \
        if (setjmp(tt_g.test_jmp) == 0) {                                                          \
            tt_g.have_jmp = 1;                                                                     \
            tt_body_##suite##_##name();                                                            \
        }                                                                                          \
        tt_g.have_jmp = 0;                                                                         \
    }                                                                                              \
    static const tt_test_t tt_rec_##suite##_##name = {                                             \
        #suite, #name, __FILE__, __LINE__, tt_fn_##suite##_##name, (flagbits), (signum)};          \
    TT_REGISTER(tt_rec_##suite##_##name);                                                          \
    static void tt_body_##suite##_##name(void)

#define TEST(suite, name) TT_MAKE_TEST(suite, name, TT_FLAG_NONE, 0)

/*
 * A death test. The body is expected to terminate the process by signal
 * signum (SIGABRT for an assert() or abort(), SIGSEGV for a fault). The
 * runner forks; the test passes only if the child dies by that signal.
 * Requires POSIX fork; on other platforms the test is skipped.
 */
#define TEST_SIGNAL(suite, name, signum) TT_MAKE_TEST(suite, name, TT_FLAG_SIGNAL, signum)

/*
 * A fixture test. The author declares a struct type, then a setup and an
 * optional teardown over it; the body receives a pointer named "fixture"
 * to a zero-initialized instance. Teardown runs even after a fatal
 * assertion in the body, and does not run if setup itself failed
 * fatally, so teardown never sees a half-built fixture.
 */
#define TEST_F_SETUP(type) static void type##_setup(struct type *fixture)
#define TEST_F_TEARDOWN(type) static void type##_teardown(struct type *fixture)

#define TEST_F(type, name)                                                                         \
    static void tt_fbody_##type##_##name(struct type *fixture);                                    \
    static void tt_fn_##type##_##name(void)                                                        \
    {                                                                                              \
        struct type fixture;                                                                       \
        volatile int tt_setup_done = 0;                                                            \
        memset(&fixture, 0, sizeof(fixture));                                                      \
        if (setjmp(tt_g.test_jmp) == 0) {                                                          \
            tt_g.have_jmp = 1;                                                                     \
            type##_setup(&fixture);                                                                \
            tt_setup_done = 1;                                                                     \
            tt_fbody_##type##_##name(&fixture);                                                    \
        }                                                                                          \
        tt_g.have_jmp = 0;                                                                         \
        if (tt_setup_done) {                                                                       \
            type##_teardown(&fixture);                                                             \
        }                                                                                          \
    }                                                                                              \
    static const tt_test_t tt_rec_##type##_##name = {                                              \
        #type, #name, __FILE__, __LINE__, tt_fn_##type##_##name, TT_FLAG_NONE, 0};                 \
    TT_REGISTER(tt_rec_##type##_##name);                                                           \
    static void tt_fbody_##type##_##name(struct type *fixture)

/* Manual registration for toolchains without the section mechanism.
 * Place TEST_REGISTER(suite, name); calls inside a function passed to
 * tt_register_manual, or call tt_register() directly. */
#define TEST_REGISTER(suite, name) tt_register(&tt_rec_##suite##_##name)
void tt_register(const tt_test_t *test);

/*
 * Failure plumbing. tt_pass_assert counts a satisfied assertion.
 * tt_fail_begin records a failure location and the stringified condition
 * and marks the current test failed. tt_diag appends one printf-formatted
 * line of failure detail. tt_fail_end closes the assertion and, when
 * fatal, longjmps back to the test wrapper. tt_note attaches an optional
 * caller message and is a no-op for an empty format.
 */
void tt_pass_assert(void);
void tt_fail_begin(const char *cond, const char *file, int line);
void tt_diag(const char *fmt, ...);
void tt_note(const char *fmt, ...);
void tt_fail_end(int fatal);

/*
 * Value printing. _Generic selects a printf conversion from the operand
 * type. Every pointer type collapses to %p through the (v)-(v) which has
 * type ptrdiff_t for pointers; unlisted types fall back to a signed
 * integer conversion. char prints numerically to avoid rendering control
 * bytes.
 */
#define TT_FMT(v)                                                                                  \
    _Generic((v),                                                                                  \
        _Bool: "%d",                                                                               \
        char: "%d",                                                                                \
        signed char: "%d",                                                                         \
        unsigned char: "%u",                                                                       \
        short: "%d",                                                                               \
        unsigned short: "%u",                                                                      \
        int: "%d",                                                                                 \
        unsigned: "%u",                                                                            \
        long: "%ld",                                                                               \
        unsigned long: "%lu",                                                                      \
        long long: "%lld",                                                                         \
        unsigned long long: "%llu",                                                                \
        float: "%f",                                                                               \
        double: "%f",                                                                              \
        long double: "%Lf",                                                                        \
        default: _Generic((v) - (v), ptrdiff_t: "%p", default: "%d"))

/*
 * Snapshot an operand once into its integer-promoted or decayed type, so
 * an assertion evaluates each argument exactly once. Assertion arguments
 * must otherwise be side-effect-free.
 */
#define TT_AUTO(x) __typeof__((x) + 0)

#define TT_CMP_MSG(a, b, op, fatal, ...)                                                           \
    do {                                                                                           \
        TT_AUTO(a) tt_a_ = (a);                                                                    \
        TT_AUTO(b) tt_b_ = (b);                                                                    \
        if (tt_a_ op tt_b_) {                                                                      \
            tt_pass_assert();                                                                      \
        } else {                                                                                   \
            tt_fail_begin(#a " " #op " " #b, __FILE__, __LINE__);                                  \
            tt_diag("left  (%s) = ", #a);                                                          \
            tt_diag(TT_FMT(tt_a_), tt_a_);                                                         \
            tt_diag("\n");                                                                         \
            tt_diag("right (%s) = ", #b);                                                          \
            tt_diag(TT_FMT(tt_b_), tt_b_);                                                         \
            tt_diag("\n");                                                                         \
            tt_note(__VA_ARGS__);                                                                  \
            tt_fail_end(fatal);                                                                    \
        }                                                                                          \
    } while (0)

#define TT_BOOL_MSG(cond, want, fatal, ...)                                                        \
    do {                                                                                           \
        if ((int)(!!(cond)) == (want)) {                                                           \
            tt_pass_assert();                                                                      \
        } else {                                                                                   \
            tt_fail_begin(#cond, __FILE__, __LINE__);                                              \
            tt_diag("expected (%s) to be %s\n", #cond, (want) ? "true" : "false");                 \
            tt_note(__VA_ARGS__);                                                                  \
            tt_fail_end(fatal);                                                                    \
        }                                                                                          \
    } while (0)

/*
 * String, memory, pointer, and float assertions. _Generic cannot do
 * these safely: strings decay to char pointers so == compares addresses,
 * NULL may be an int, and floats need a tolerance. Each has a dedicated
 * comparator. tt_streq/tt_ptreq/tt_neareq return nonzero on match;
 * tt_mem_diff returns the first differing byte index or n when equal.
 */
int tt_streq(const char *a, const char *b);
size_t tt_mem_diff(const void *a, const void *b, size_t n);
int tt_neareq(long double a, long double b, long double eps);

#define TT_STR_MSG(a, b, want, fatal, ...)                                                         \
    do {                                                                                           \
        const char *tt_sa_ = (a);                                                                  \
        const char *tt_sb_ = (b);                                                                  \
        if (tt_streq(tt_sa_, tt_sb_) == (want)) {                                                  \
            tt_pass_assert();                                                                      \
        } else {                                                                                   \
            tt_fail_begin("string compare " #a ", " #b, __FILE__, __LINE__);                       \
            tt_diag("left  (%s) = \"%s\"\n", #a, tt_sa_ ? tt_sa_ : "(null)");                      \
            tt_diag("right (%s) = \"%s\"\n", #b, tt_sb_ ? tt_sb_ : "(null)");                      \
            tt_note(__VA_ARGS__);                                                                  \
            tt_fail_end(fatal);                                                                    \
        }                                                                                          \
    } while (0)

#define TT_MEM_MSG(a, b, n, fatal, ...)                                                            \
    do {                                                                                           \
        const void *tt_ma_ = (a);                                                                  \
        const void *tt_mb_ = (b);                                                                  \
        size_t tt_mn_ = (n);                                                                       \
        size_t tt_off_ = tt_mem_diff(tt_ma_, tt_mb_, tt_mn_);                                      \
        if (tt_off_ == tt_mn_) {                                                                   \
            tt_pass_assert();                                                                      \
        } else {                                                                                   \
            tt_fail_begin(#a " and " #b " differ", __FILE__, __LINE__);                            \
            tt_diag("first difference at byte %zu: 0x%02x vs 0x%02x\n", tt_off_,                   \
                    ((const unsigned char *)tt_ma_)[tt_off_],                                      \
                    ((const unsigned char *)tt_mb_)[tt_off_]);                                     \
            tt_note(__VA_ARGS__);                                                                  \
            tt_fail_end(fatal);                                                                    \
        }                                                                                          \
    } while (0)

#define TT_NEAR_MSG(a, b, eps, fatal, ...)                                                         \
    do {                                                                                           \
        long double tt_na_ = (a);                                                                  \
        long double tt_nb_ = (b);                                                                  \
        long double tt_ne_ = (eps);                                                                \
        if (tt_neareq(tt_na_, tt_nb_, tt_ne_)) {                                                   \
            tt_pass_assert();                                                                      \
        } else {                                                                                   \
            tt_fail_begin(#a " ~= " #b, __FILE__, __LINE__);                                       \
            tt_diag("left = %Lf, right = %Lf, tolerance = %Lf\n", tt_na_, tt_nb_, tt_ne_);         \
            tt_note(__VA_ARGS__);                                                                  \
            tt_fail_end(fatal);                                                                    \
        }                                                                                          \
    } while (0)

/* Fatal assertions. */
#define ASSERT_TRUE(cond) TT_BOOL_MSG(cond, 1, 1, "")
#define ASSERT_FALSE(cond) TT_BOOL_MSG(cond, 0, 1, "")
#define ASSERT_EQ(a, b) TT_CMP_MSG(a, b, ==, 1, "")
#define ASSERT_NE(a, b) TT_CMP_MSG(a, b, !=, 1, "")
#define ASSERT_LT(a, b) TT_CMP_MSG(a, b, <, 1, "")
#define ASSERT_LE(a, b) TT_CMP_MSG(a, b, <=, 1, "")
#define ASSERT_GT(a, b) TT_CMP_MSG(a, b, >, 1, "")
#define ASSERT_GE(a, b) TT_CMP_MSG(a, b, >=, 1, "")
#define ASSERT_STR_EQ(a, b) TT_STR_MSG(a, b, 1, 1, "")
#define ASSERT_STR_NE(a, b) TT_STR_MSG(a, b, 0, 1, "")
#define ASSERT_MEM_EQ(a, b, n) TT_MEM_MSG(a, b, n, 1, "")
#define ASSERT_NEAR(a, b, eps) TT_NEAR_MSG(a, b, eps, 1, "")
#define ASSERT_NULL(p) TT_BOOL_MSG((p) == NULL, 1, 1, "")
#define ASSERT_NOT_NULL(p) TT_BOOL_MSG((p) != NULL, 1, 1, "")
#define ASSERT_PTR_EQ(a, b) TT_BOOL_MSG((const void *)(a) == (const void *)(b), 1, 1, "")

/* Non-fatal assertions. */
#define EXPECT_TRUE(cond) TT_BOOL_MSG(cond, 1, 0, "")
#define EXPECT_FALSE(cond) TT_BOOL_MSG(cond, 0, 0, "")
#define EXPECT_EQ(a, b) TT_CMP_MSG(a, b, ==, 0, "")
#define EXPECT_NE(a, b) TT_CMP_MSG(a, b, !=, 0, "")
#define EXPECT_LT(a, b) TT_CMP_MSG(a, b, <, 0, "")
#define EXPECT_LE(a, b) TT_CMP_MSG(a, b, <=, 0, "")
#define EXPECT_GT(a, b) TT_CMP_MSG(a, b, >, 0, "")
#define EXPECT_GE(a, b) TT_CMP_MSG(a, b, >=, 0, "")
#define EXPECT_STR_EQ(a, b) TT_STR_MSG(a, b, 1, 0, "")
#define EXPECT_STR_NE(a, b) TT_STR_MSG(a, b, 0, 0, "")
#define EXPECT_MEM_EQ(a, b, n) TT_MEM_MSG(a, b, n, 0, "")
#define EXPECT_NEAR(a, b, eps) TT_NEAR_MSG(a, b, eps, 0, "")
#define EXPECT_NULL(p) TT_BOOL_MSG((p) == NULL, 1, 0, "")
#define EXPECT_NOT_NULL(p) TT_BOOL_MSG((p) != NULL, 1, 0, "")
#define EXPECT_PTR_EQ(a, b) TT_BOOL_MSG((const void *)(a) == (const void *)(b), 1, 0, "")

/* Message variants: a trailing printf format and arguments describing
 * the assertion, printed on failure. */
#define ASSERT_EQ_MSG(a, b, ...) TT_CMP_MSG(a, b, ==, 1, __VA_ARGS__)
#define ASSERT_TRUE_MSG(c, ...) TT_BOOL_MSG(c, 1, 1, __VA_ARGS__)
#define EXPECT_EQ_MSG(a, b, ...) TT_CMP_MSG(a, b, ==, 0, __VA_ARGS__)
#define EXPECT_TRUE_MSG(c, ...) TT_BOOL_MSG(c, 1, 0, __VA_ARGS__)

/* Mark the current test skipped and stop running its body. */
void tt_skip(const char *reason);
#define SKIP(reason)                                                                               \
    do {                                                                                           \
        tt_skip(reason);                                                                           \
    } while (0)

/* A reproducible, harness-owned PRNG. The seed is printed each run and
 * settable with --seed, so a randomized test replays exactly. */
uint64_t tt_rand(void);
uint32_t tt_rand_below(uint32_t bound);

/* Runs the selected tests and returns the process exit code. TEST_MAIN
 * provides a main that forwards argc and argv to it. */
int tt_run_all(int argc, char **argv);

#define TEST_MAIN()                                                                                \
    int main(int argc, char **argv)                                                                \
    {                                                                                              \
        return tt_run_all(argc, argv);                                                             \
    }

#ifdef __cplusplus
}
#endif

#endif /* AGENC_TEST_H_INCLUDED */

/* ---- implementation ---------------------------------------------- */

#ifdef TEST_IMPLEMENTATION
#ifndef AGENC_TEST_IMPLEMENTATION_DONE
#define AGENC_TEST_IMPLEMENTATION_DONE

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
#define TT_POSIX 1
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

tt_state_t tt_g;

/* Config resolved from argv, plus the per-test diagnostic buffer and the
 * PRNG state. All process-global, all owned by the runner. */
enum { TT_DIAG_CAP = 4096, TT_MANUAL_CAP = 1024 };

static struct {
    const char *filter;
    const char *xml_path;
    int list_only;
    int isolate;
    int fail_fast;
    int color;
    int skipped_current;
    const char *skip_reason;
    char diag[TT_DIAG_CAP];
    size_t diag_len;
    uint64_t rng;
    const tt_test_t *manual[TT_MANUAL_CAP];
    size_t manual_len;
} tt_c;

#if defined(TT_HAVE_AUTO_REGISTER) && !defined(__APPLE__)
extern const tt_test_t *const __start_agenctst[];
extern const tt_test_t *const __stop_agenctst[];
#endif
#if defined(__APPLE__)
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#endif

void tt_register(const tt_test_t *test)
{
    if (tt_c.manual_len < TT_MANUAL_CAP) {
        tt_c.manual[tt_c.manual_len++] = test;
    }
}

void tt_pass_assert(void)
{
    tt_g.assertions++;
}

/* Appends one printf-formatted chunk to the current test's diagnostic
 * buffer, truncating rather than overflowing. */
void tt_diag(const char *fmt, ...)
{
    va_list ap;
    int n;
    size_t room;

    if (tt_c.diag_len >= TT_DIAG_CAP - 1) {
        return;
    }
    room = TT_DIAG_CAP - tt_c.diag_len;
    va_start(ap, fmt);
    n = vsnprintf(tt_c.diag + tt_c.diag_len, room, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if ((size_t)n >= room) {
        tt_c.diag_len = TT_DIAG_CAP - 1;
    } else {
        tt_c.diag_len += (size_t)n;
    }
}

void tt_note(const char *fmt, ...)
{
    va_list ap;

    if (fmt == NULL || fmt[0] == '\0') {
        return;
    }
    tt_diag("note: ");
    va_start(ap, fmt);
    {
        char buf[512];
        int n = vsnprintf(buf, sizeof(buf), fmt, ap);

        if (n > 0) {
            tt_diag("%s", buf);
        }
    }
    va_end(ap);
    tt_diag("\n");
}

void tt_fail_begin(const char *cond, const char *file, int line)
{
    tt_g.assertions++;
    tt_g.current_failed = 1;
    tt_diag("%s:%d: %s\n", file, line, cond);
}

void tt_fail_end(int fatal)
{
    if (fatal && tt_g.have_jmp) {
        longjmp(tt_g.test_jmp, 1);
    }
}

void tt_skip(const char *reason)
{
    tt_c.skipped_current = 1;
    tt_c.skip_reason = reason;
    if (tt_g.have_jmp) {
        longjmp(tt_g.test_jmp, 1);
    }
}

int tt_streq(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return a == b;
    }
    return strcmp(a, b) == 0;
}

size_t tt_mem_diff(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    size_t i;

    for (i = 0; i < n; i++) {
        if (pa[i] != pb[i]) {
            return i;
        }
    }
    return n;
}

int tt_neareq(long double a, long double b, long double eps)
{
    long double d = a - b;

    if (d < 0) {
        d = -d;
    }
    return d <= eps;
}

/* PCG-XSH-RR 64/32. Deterministic from the seed. */
uint64_t tt_rand(void)
{
    uint64_t old = tt_c.rng;
    uint32_t xorshifted;
    uint32_t rot;

    tt_c.rng = old * 6364136223846793005ULL + 1442695040888963407ULL;
    xorshifted = (uint32_t)(((old >> 18) ^ old) >> 27);
    rot = (uint32_t)(old >> 59);
    return (xorshifted >> rot) | (xorshifted << ((0u - rot) & 31u));
}

uint32_t tt_rand_below(uint32_t bound)
{
    if (bound == 0) {
        return 0;
    }
    return (uint32_t)(tt_rand() % bound);
}

/* True when a test's "suite.name" passes the active filter. */
static int tt_selected(const tt_test_t *t)
{
    char key[256];

    if (tt_c.filter == NULL) {
        return 1;
    }
    (void)snprintf(key, sizeof(key), "%s.%s", t->suite, t->name);
    return strstr(key, tt_c.filter) != NULL;
}

/* Emits the buffered diagnostics as TAP comment lines under a failing
 * test point. */
static void tt_flush_diag(void)
{
    size_t start = 0;
    size_t i;

    for (i = 0; i < tt_c.diag_len; i++) {
        if (tt_c.diag[i] == '\n') {
            TT_PRINTF("#   %.*s\n", (int)(i - start), tt_c.diag + start);
            start = i + 1;
        }
    }
    if (start < tt_c.diag_len) {
        TT_PRINTF("#   %.*s\n", (int)(tt_c.diag_len - start), tt_c.diag + start);
    }
}

/* Result of running one test in-process. */
enum { TT_PASS = 0, TT_FAIL = 1, TT_SKIP = 2 };

static int tt_exec_body(const tt_test_t *t)
{
    tt_g.current = t;
    tt_g.current_failed = 0;
    tt_c.skipped_current = 0;
    tt_c.skip_reason = NULL;
    tt_c.diag_len = 0;
    t->fn();
    if (tt_c.skipped_current) {
        return TT_SKIP;
    }
    return tt_g.current_failed ? TT_FAIL : TT_PASS;
}

#if defined(TT_POSIX)
static const char *volatile tt_crash_name;

/* Async-signal-safe crash attribution: name the running test on the
 * error stream with write, restore the default handler, and re-raise so
 * the process still dies with the crash signal. */
static void tt_crash_handler(int sig)
{
    const char *n = tt_crash_name;

    if (n != NULL) {
        (void)!write(STDERR_FILENO, "\n# crashed in test: ", 20);
        (void)!write(STDERR_FILENO, n, strlen(n));
        (void)!write(STDERR_FILENO, "\n", 1);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static void tt_install_crash_handlers(void)
{
    signal(SIGSEGV, tt_crash_handler);
    signal(SIGABRT, tt_crash_handler);
    signal(SIGFPE, tt_crash_handler);
    signal(SIGBUS, tt_crash_handler);
}

/* Runs one test in a child process. Returns TT_PASS/TT_FAIL/TT_SKIP for
 * a clean exit, or reports a crash. For a death test, the expected
 * signal is a pass and any other outcome a fail. crashed_sig receives the
 * terminating signal or 0. */
static int tt_exec_forked(const tt_test_t *t, int *crashed_sig)
{
    pid_t pid;
    int status = 0;

    *crashed_sig = 0;
    fflush(stdout);
    pid = fork();
    if (pid < 0) {
        return TT_FAIL;
    }
    if (pid == 0) {
        int r;

        /* A death test is expected to die; stay quiet so its expected
         * signal does not print a spurious crash line. */
        tt_crash_name = (t->flags & TT_FLAG_SIGNAL) ? NULL : t->name;
        r = tt_exec_body(t);
        if (r == TT_FAIL) {
            tt_flush_diag();
        }
        fflush(stdout);
        _exit(r);
    }
    (void)waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) {
        *crashed_sig = WTERMSIG(status);
        return TT_FAIL;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return TT_FAIL;
}
#endif /* TT_POSIX */

/* Runs one test and emits its TAP point plus any diagnostics. n is the
 * 1-based test number. */
static void tt_run_one(const tt_test_t *t, unsigned n)
{
    int result;
    int crashed = 0;

    if (t->flags & TT_FLAG_SIGNAL) {
#if defined(TT_POSIX)
        int sig = 0;
        int r = tt_exec_forked(t, &sig);

        tt_g.tests_run++;
        if (r == TT_FAIL && sig == t->signal) {
            tt_g.tests_passed++;
            TT_PRINTF("ok %u - %s.%s # died by signal %d as expected\n", n, t->suite, t->name, sig);
        } else {
            tt_g.tests_failed++;
            TT_PRINTF("not ok %u - %s.%s\n", n, t->suite, t->name);
            TT_PRINTF("#   expected signal %d, ", t->signal);
            if (sig != 0) {
                TT_PRINTF("got signal %d\n", sig);
            } else {
                TT_PRINTF("test exited without dying\n");
            }
        }
#else
        tt_g.tests_run++;
        tt_g.tests_skipped++;
        TT_PRINTF("ok %u - %s.%s # SKIP death test needs POSIX fork\n", n, t->suite, t->name);
#endif
        return;
    }

#if defined(TT_POSIX)
    if (tt_c.isolate) {
        result = tt_exec_forked(t, &crashed);
        tt_g.tests_run++;
        if (crashed != 0) {
            tt_g.tests_failed++;
            TT_PRINTF("not ok %u - %s.%s\n#   crashed: signal %d\n", n, t->suite, t->name, crashed);
            return;
        }
        if (result == TT_SKIP) {
            tt_g.tests_skipped++;
            TT_PRINTF("ok %u - %s.%s # SKIP\n", n, t->suite, t->name);
            return;
        }
        if (result == TT_PASS) {
            tt_g.tests_passed++;
            TT_PRINTF("ok %u - %s.%s\n", n, t->suite, t->name);
        } else {
            tt_g.tests_failed++;
            TT_PRINTF("not ok %u - %s.%s\n", n, t->suite, t->name);
        }
        return;
    }
#endif

    result = tt_exec_body(t);
    tt_g.tests_run++;
    if (result == TT_SKIP) {
        tt_g.tests_skipped++;
        TT_PRINTF("ok %u - %s.%s # SKIP %s\n", n, t->suite, t->name,
                  tt_c.skip_reason ? tt_c.skip_reason : "");
        return;
    }
    if (result == TT_PASS) {
        tt_g.tests_passed++;
        TT_PRINTF("ok %u - %s.%s\n", n, t->suite, t->name);
    } else {
        tt_g.tests_failed++;
        TT_PRINTF("not ok %u - %s.%s\n", n, t->suite, t->name);
        tt_flush_diag();
    }
}

/* Comparison for the deterministic file:line test order. */
static int tt_cmp_tests(const void *pa, const void *pb)
{
    const tt_test_t *a = *(const tt_test_t *const *)pa;
    const tt_test_t *b = *(const tt_test_t *const *)pb;
    int c = strcmp(a->file, b->file);

    if (c != 0) {
        return c;
    }
    return (a->line > b->line) - (a->line < b->line);
}

/* Fills list with the registered tests in file:line order. Returns the
 * count, capped at cap. */
static size_t tt_collect(const tt_test_t **list, size_t cap)
{
    size_t count = 0;

#if defined(TT_HAVE_AUTO_REGISTER) && !defined(__APPLE__)
    for (const tt_test_t *const *p = __start_agenctst; p < __stop_agenctst && count < cap; p++) {
        list[count++] = *p;
    }
#elif defined(__APPLE__)
    {
        unsigned long size = 0;
        const tt_test_t *const *base = (const tt_test_t *const *)getsectiondata(
            &_mh_execute_header, "__DATA", "agenctst", &size);
        size_t total = base ? size / sizeof(*base) : 0;
        size_t i;

        for (i = 0; i < total && count < cap; i++) {
            list[count++] = base[i];
        }
    }
#endif
    for (size_t i = 0; i < tt_c.manual_len && count < cap; i++) {
        list[count++] = tt_c.manual[i];
    }
    if (count > 1) {
        qsort(list, count, sizeof(*list), tt_cmp_tests);
    }
    return count;
}

/* Writes a JUnit report of the run to tt_c.xml_path. A second pass is
 * cheap and keeps the streaming TAP path allocation-free. */
static void tt_write_xml(const tt_test_t **list, size_t count)
{
    FILE *f = fopen(tt_c.xml_path, "w");
    size_t i;

    if (f == NULL) {
        return;
    }
    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf(f, "<testsuite name=\"agenc-test\" tests=\"%u\" failures=\"%u\" skipped=\"%u\">\n",
            tt_g.tests_run, tt_g.tests_failed, tt_g.tests_skipped);
    for (i = 0; i < count; i++) {
        fprintf(f, "  <testcase classname=\"%s\" name=\"%s\"/>\n", list[i]->suite, list[i]->name);
    }
    fprintf(f, "</testsuite>\n");
    fclose(f);
}

static uint64_t tt_seed_default(void)
{
    return (uint64_t)0x9e3779b97f4a7c15ULL;
}

static void tt_parse_args(int argc, char **argv, uint64_t *seed)
{
    int i;

    tt_c.color = 1;
    *seed = tt_seed_default();
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strncmp(a, "--filter=", 9) == 0) {
            tt_c.filter = a + 9;
        } else if (strncmp(a, "--seed=", 7) == 0) {
            *seed = strtoull(a + 7, NULL, 0);
        } else if (strncmp(a, "--xml=", 6) == 0) {
            tt_c.xml_path = a + 6;
        } else if (strcmp(a, "--list") == 0) {
            tt_c.list_only = 1;
        } else if (strcmp(a, "--isolate") == 0) {
            tt_c.isolate = 1;
        } else if (strcmp(a, "--fail-fast") == 0) {
            tt_c.fail_fast = 1;
        } else if (strcmp(a, "--no-color") == 0) {
            tt_c.color = 0;
        }
    }
}

int tt_run_all(int argc, char **argv)
{
    static const tt_test_t *list[TT_MANUAL_CAP + 4096];
    size_t count;
    size_t i;
    unsigned n = 0;
    uint64_t seed;

    tt_parse_args(argc, argv, &seed);
    tt_c.rng = seed;
    count = tt_collect(list, sizeof(list) / sizeof(list[0]));

    if (tt_c.list_only) {
        for (i = 0; i < count; i++) {
            TT_PRINTF("%s.%s\n", list[i]->suite, list[i]->name);
        }
        return 0;
    }

#if defined(TT_POSIX)
    tt_install_crash_handlers();
#endif

    TT_PRINTF("TAP version 14\n");
    TT_PRINTF("# seed %llu\n", (unsigned long long)seed);
    for (i = 0; i < count; i++) {
        if (!tt_selected(list[i])) {
            continue;
        }
        tt_run_one(list[i], ++n);
        if (tt_c.fail_fast && tt_g.tests_failed > 0) {
            TT_PRINTF("Bail out! --fail-fast after first failure\n");
            break;
        }
    }
    TT_PRINTF("1..%u\n", n);
    TT_PRINTF("# %u passed, %u failed, %u skipped, %u assertions\n", tt_g.tests_passed,
              tt_g.tests_failed, tt_g.tests_skipped, tt_g.assertions);

    if (tt_c.xml_path != NULL) {
        tt_write_xml(list, count);
    }
    return tt_g.tests_failed == 0 ? 0 : 1;
}

#endif /* AGENC_TEST_IMPLEMENTATION_DONE */
#endif /* TEST_IMPLEMENTATION */
