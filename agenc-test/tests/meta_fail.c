/*
 * meta_fail.c: deliberately failing tests, built as a separate binary.
 * It is not part of the passing suite. The Makefile meta target runs it
 * and asserts it reports the failures and exits nonzero, proving the
 * harness actually detects failure (a harness that always exits 0 is
 * worthless). It also proves fixture teardown runs after a fatal assert.
 * Expected: three failing tests, and the teardown marker printed.
 */

#include <stdio.h>
#include <string.h>

#define TEST_IMPLEMENTATION
#include "test.h"

TEST(meta, this_one_passes)
{
    ASSERT_EQ(1, 1);
}

TEST(meta, fatal_stops_the_body)
{
    ASSERT_EQ(2 + 2, 5); /* fatal: aborts here */
    ASSERT_TRUE(0);      /* must not run */
}

TEST(meta, nonfatal_records_both)
{
    EXPECT_EQ(1, 2); /* recorded, continues */
    EXPECT_TRUE(0);  /* also recorded */
}

/* A fixture whose teardown prints a marker. The body fails fatally; the
 * marker must still appear, proving teardown survives a fatal assert. */
struct td_fixture {
    int dummy;
};

TEST_F_SETUP(td_fixture)
{
    fixture->dummy = 1;
}

TEST_F_TEARDOWN(td_fixture)
{
    (void)fixture;
    fprintf(stdout, "# TEARDOWN-RAN\n");
}

TEST_F(td_fixture, fatal_then_teardown)
{
    ASSERT_EQ(fixture->dummy, 2); /* fatal: the body aborts here */
}

TEST_MAIN()
