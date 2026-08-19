CC ?= cc
CLANG_FORMAT ?= clang-format
CPPFLAGS ?= -Iinclude
CFLAGS ?= -std=c11 -pedantic-errors -Wall -Wextra -Werror -Wconversion -Wshadow \
	-Wformat=2 -Wno-format-nonliteral -Wnull-dereference -Wstrict-prototypes \
	-Wmissing-prototypes

SRC = src/str.c
TEST_SRC = tests/test_str.c
TEST_BIN = tests/test_str
ASAN_BIN = tests/test_str_asan
RELEASE_BIN = tests/test_str_release
DEMO_SRC = main.c
DEMO_BIN = str_demo
FORMAT_FILES = include/str.h src/str.c tests/test_str.c main.c

.PHONY: test asan release format check demo clean

test: $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_BIN): $(TEST_SRC) $(SRC) include/str.h Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) -DSTR_TEST -o $@ $(TEST_SRC) $(SRC)

asan: $(ASAN_BIN)
	./$(ASAN_BIN)

$(ASAN_BIN): $(TEST_SRC) $(SRC) include/str.h Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) -DSTR_TEST -O1 -g -fno-omit-frame-pointer \
		-fsanitize=address,undefined -fno-sanitize-recover=all -o $@ $(TEST_SRC) $(SRC)

release: $(RELEASE_BIN)
	./$(RELEASE_BIN)

$(RELEASE_BIN): $(TEST_SRC) $(SRC) include/str.h Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) -DSTR_TEST -O3 -DNDEBUG -D_FORTIFY_SOURCE=3 \
		-o $@ $(TEST_SRC) $(SRC)

demo: $(DEMO_BIN)
	./$(DEMO_BIN)

$(DEMO_BIN): $(DEMO_SRC) $(SRC) include/str.h Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(DEMO_SRC) $(SRC)

format:
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_FILES)

check: format test asan release

clean:
	rm -f $(TEST_BIN) $(ASAN_BIN) $(RELEASE_BIN) $(DEMO_BIN)
