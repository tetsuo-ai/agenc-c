/*
 * test_more.c: a second translation unit of tests, linked into the same
 * binary as test_test.c. It does not define TEST_IMPLEMENTATION or main.
 * Its presence proves the linker-section registration discovers tests
 * across translation units, which every multi-file suite relies on.
 */

#include "test.h"

TEST(second_tu, is_discovered)
{
    ASSERT_EQ(10 + 10, 20);
}

TEST(second_tu, also_runs)
{
    EXPECT_TRUE(1);
}
