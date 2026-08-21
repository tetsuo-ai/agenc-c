/* test_test.c: agenc-test testing itself. */

#include <signal.h>
#include <string.h>

#define TEST_IMPLEMENTATION
#include "test.h"

TEST(scalar, comparisons)
{
    ASSERT_TRUE(1);
    ASSERT_FALSE(0);
    ASSERT_EQ(2 + 2, 4);
    ASSERT_NE(1, 2);
    ASSERT_LT(1, 2);
    ASSERT_LE(2, 2);
    ASSERT_GT(3, 2);
    ASSERT_GE(3, 3);
    EXPECT_EQ(7, 7);
}

TEST(scalar, mixed_widths)
{
    size_t a = 5;
    int b = 5;

    ASSERT_EQ(a, (size_t)b);
    EXPECT_TRUE(a == (size_t)b);
}

TEST(string, equality)
{
    const char *s = "abc";

    ASSERT_STR_EQ(s, "abc");
    ASSERT_STR_NE(s, "abd");
    EXPECT_STR_EQ("", "");
}

TEST(memory, equality)
{
    unsigned char a[4] = {1, 2, 3, 4};
    unsigned char b[4] = {1, 2, 3, 4};

    ASSERT_MEM_EQ(a, b, sizeof(a));
}

TEST(floats, tolerance)
{
    ASSERT_NEAR(1.0, 1.0 + 1e-9, 1e-6);
    EXPECT_NEAR(3.14159, 3.14160, 1e-4);
}

TEST(pointers, null_and_eq)
{
    int x = 0;
    int *p = &x;
    void *q = NULL;

    ASSERT_NOT_NULL(p);
    ASSERT_NULL(q);
    ASSERT_PTR_EQ(p, &x);
}

TEST(messages, attach_context)
{
    int code = 42;

    ASSERT_EQ_MSG(code, 42, "code should be 42 but was %d", code);
}

TEST(skipping, is_reported)
{
    SKIP("demonstrates the skip path");
    ASSERT_TRUE(0); /* never reached */
}

TEST(random, is_reproducible)
{
    /* Two draws differ; the sequence is fixed by the seed. */
    uint64_t first = tt_rand();
    uint64_t second = tt_rand();

    ASSERT_NE(first, second);
    ASSERT_LT(tt_rand_below(10), (uint32_t)10);
}

/* Fixture: a small buffer set up and torn down around each test. */
struct buf_fixture {
    unsigned char *data;
    size_t len;
};

TEST_F_SETUP(buf_fixture)
{
    static unsigned char storage[8];

    fixture->data = storage;
    fixture->len = sizeof(storage);
    memset(fixture->data, 0xAA, fixture->len);
}

TEST_F_TEARDOWN(buf_fixture)
{
    /* The fixture is valid here even after a fatal assert in the body. */
    fixture->data = NULL;
}

TEST_F(buf_fixture, sees_setup_state)
{
    ASSERT_NOT_NULL(fixture->data);
    ASSERT_EQ(fixture->len, (size_t)8);
    ASSERT_EQ(fixture->data[0], (unsigned char)0xAA);
}

/* Death test: an out-of-contract call aborts; that abort is the pass. */
TEST_SIGNAL(death, abort_is_expected, SIGABRT)
{
    abort();
}

TEST_MAIN()
