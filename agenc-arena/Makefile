CC ?= cc
CLANG ?= clang
CLANG_FORMAT ?= clang-format
VALGRIND ?= valgrind
ARENA_CPPFLAGS = -Iinclude
ARENA_CFLAGS = -std=c11 -pedantic-errors -Wall -Wextra -Werror -Wconversion -Wshadow \
	-Wformat=2 -Wnull-dereference -Wstrict-prototypes -Wmissing-prototypes
ARENA_COMPILE = $(CC) $(CPPFLAGS) $(ARENA_CPPFLAGS) $(CFLAGS) $(ARENA_CFLAGS)

SRC = src/arena.c
HDRS = include/alloc.h include/arena.h
TEST_SRC = tests/test_arena.c
TEST_BIN = tests/test_arena
ASAN_BIN = tests/test_arena_asan
ISO_BIN = tests/test_arena_iso
RELEASE_BIN = tests/test_arena_release
HEAPLESS_BIN = tests/test_arena_heapless
VG_BIN = tests/test_arena_vg
FUZZ_SRC = tests/fuzz_arena.c
FUZZ_BIN = tests/fuzz_arena
DEMO_SRC = main.c
DEMO_BIN = arena_demo
BENCH_SRC = bench/bench_arena.c
BENCH_BIN = bench/bench_arena
FORMAT_FILES = $(wildcard include/*.h src/*.c tests/*.c bench/*.c main.c)

.PHONY: test asan iso release heapless fuzz valgrind valgrind-heapless analyze format check \
	demo bench clean

test: $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_BIN): $(TEST_SRC) $(SRC) $(HDRS) Makefile
	$(ARENA_COMPILE) $(LDFLAGS) -o $@ $(TEST_SRC) $(SRC) $(LDLIBS)

asan: $(ASAN_BIN)
	./$(ASAN_BIN)

$(ASAN_BIN): $(TEST_SRC) $(SRC) $(HDRS) Makefile
	$(ARENA_COMPILE) -O1 -g -fno-omit-frame-pointer \
		-fsanitize=address,undefined -fno-sanitize-recover=all $(LDFLAGS) \
		-o $@ $(TEST_SRC) $(SRC) $(LDLIBS)

iso: $(ISO_BIN)
	./$(ISO_BIN)

$(ISO_BIN): $(TEST_SRC) $(SRC) $(HDRS) Makefile
	$(ARENA_COMPILE) -DARENA_STRICT_ISO -O2 $(LDFLAGS) -o $@ $(TEST_SRC) $(SRC) $(LDLIBS)

release: $(RELEASE_BIN)
	./$(RELEASE_BIN)

$(RELEASE_BIN): $(TEST_SRC) $(SRC) $(HDRS) Makefile
	$(ARENA_COMPILE) -O3 -DNDEBUG -D_FORTIFY_SOURCE=3 $(LDFLAGS) \
		-o $@ $(TEST_SRC) $(SRC) $(LDLIBS)

heapless: $(HEAPLESS_BIN)
	./$(HEAPLESS_BIN)

$(HEAPLESS_BIN): $(TEST_SRC) $(SRC) $(HDRS) Makefile
	$(ARENA_COMPILE) -DARENA_TEST_FIXED_ONLY $(LDFLAGS) -o $@ $(TEST_SRC) $(SRC) \
		$(LDLIBS)

fuzz: $(FUZZ_BIN)
	./$(FUZZ_BIN) -max_total_time=60 -rss_limit_mb=1024 tests/corpus

$(FUZZ_BIN): $(FUZZ_SRC) $(SRC) $(HDRS) Makefile
	$(CLANG) $(CPPFLAGS) $(ARENA_CPPFLAGS) $(CFLAGS) $(ARENA_CFLAGS) -O1 -g \
		-fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined \
		-fno-sanitize-recover=all $(LDFLAGS) -o $@ $(FUZZ_SRC) $(SRC) $(LDLIBS)

valgrind: $(VG_BIN)
	$(VALGRIND) --error-exitcode=1 --leak-check=full --errors-for-leak-kinds=all \
		--child-silent-after-fork=yes ./$(VG_BIN)

$(VG_BIN): $(TEST_SRC) $(SRC) $(HDRS) Makefile
	$(ARENA_COMPILE) -DARENA_ENABLE_VALGRIND -g $(LDFLAGS) -o $@ $(TEST_SRC) $(SRC) \
		$(LDLIBS)

demo: $(DEMO_BIN)
	./$(DEMO_BIN)

$(DEMO_BIN): $(DEMO_SRC) $(SRC) $(HDRS) Makefile
	$(ARENA_COMPILE) $(LDFLAGS) -o $@ $(DEMO_SRC) $(SRC) $(LDLIBS)

bench: $(BENCH_BIN)
	./$(BENCH_BIN)

$(BENCH_BIN): $(BENCH_SRC) $(SRC) $(HDRS) Makefile
	$(ARENA_COMPILE) -O2 -DNDEBUG $(LDFLAGS) -o $@ $(BENCH_SRC) $(SRC) $(LDLIBS)

valgrind-heapless: $(HEAPLESS_BIN)
	$(VALGRIND) --error-exitcode=1 --leak-check=full --errors-for-leak-kinds=all \
		--child-silent-after-fork=yes ./$(HEAPLESS_BIN)

analyze:
	gcc -fanalyzer $(ARENA_CPPFLAGS) $(ARENA_CFLAGS) -c $(SRC) -o /dev/null
	$(CLANG) --analyze $(ARENA_CPPFLAGS) $(ARENA_CFLAGS) $(SRC) -o /dev/null

format:
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_FILES)

check: format $(DEMO_BIN) test asan iso release heapless

clean:
	rm -f $(TEST_BIN) $(ASAN_BIN) $(ISO_BIN) $(RELEASE_BIN) $(HEAPLESS_BIN) \
		$(VG_BIN) $(FUZZ_BIN) $(DEMO_BIN) $(BENCH_BIN) tests/crash-*
