CC ?= cc
CLANG ?= clang
CLANG_FORMAT ?= clang-format
TEST_CPPFLAGS = -Iinclude
TEST_CFLAGS = -std=c11 -pedantic-errors -Wall -Wextra -Werror -Wconversion -Wshadow \
	-Wformat=2 -Wno-format-nonliteral -Wnull-dereference -Wstrict-prototypes \
	-Wmissing-prototypes
TEST_COMPILE = $(CC) $(CPPFLAGS) $(TEST_CPPFLAGS) $(CFLAGS) $(TEST_CFLAGS)

SELF_SRC = tests/test_test.c tests/test_more.c
SELF_BIN = tests/test_test
ASAN_BIN = tests/test_test_asan
ISO_BIN = tests/test_test_iso
RELEASE_BIN = tests/test_test_release
META_SRC = tests/meta_fail.c
META_BIN = tests/meta_fail
HDR = include/test.h
FORMAT_FILES = $(wildcard include/*.h tests/*.c)

.PHONY: test asan iso release meta format check clean

test: $(SELF_BIN)
	./$(SELF_BIN)

$(SELF_BIN): $(SELF_SRC) $(HDR) Makefile
	$(TEST_COMPILE) $(LDFLAGS) -o $@ $(SELF_SRC) $(LDLIBS)

asan: $(ASAN_BIN)
	./$(ASAN_BIN)

$(ASAN_BIN): $(SELF_SRC) $(HDR) Makefile
	$(TEST_COMPILE) -O1 -g -fno-omit-frame-pointer \
		-fsanitize=address,undefined -fno-sanitize-recover=all $(LDFLAGS) \
		-o $@ $(SELF_SRC) $(LDLIBS)

iso: $(ISO_BIN)
	./$(ISO_BIN)

$(ISO_BIN): $(SELF_SRC) $(HDR) Makefile
	$(TEST_COMPILE) -O2 $(LDFLAGS) -o $@ $(SELF_SRC) $(LDLIBS)

release: $(RELEASE_BIN)
	./$(RELEASE_BIN)

$(RELEASE_BIN): $(SELF_SRC) $(HDR) Makefile
	$(TEST_COMPILE) -O3 -DNDEBUG -D_FORTIFY_SOURCE=3 $(LDFLAGS) \
		-o $@ $(SELF_SRC) $(LDLIBS)

# Failure-detection proof: the deliberately-failing binary MUST exit
# nonzero and report exactly three failures. If it exits 0, failure
# detection is broken and this target fails.
meta: $(META_BIN)
	@out=$$(./$(META_BIN)); status=$$?; \
	echo "$$out"; \
	if [ $$status -eq 0 ]; then echo "META FAIL: failing suite exited 0"; exit 1; fi; \
	echo "$$out" | grep -q "3 failed" || { echo "META FAIL: wrong failure count"; exit 1; }; \
	echo "$$out" | grep -q "TEARDOWN-RAN" || { echo "META FAIL: teardown skipped after fatal assert"; exit 1; }; \
	echo "meta: failure detection + teardown-after-fatal verified (exit $$status)"

$(META_BIN): $(META_SRC) $(HDR) Makefile
	$(TEST_COMPILE) $(LDFLAGS) -o $@ $(META_SRC) $(LDLIBS)

format:
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_FILES)

check: format test asan iso release meta

clean:
	rm -f $(SELF_BIN) $(ASAN_BIN) $(ISO_BIN) $(RELEASE_BIN) $(META_BIN)
