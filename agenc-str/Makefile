CC ?= cc
CLANG_FORMAT ?= clang-format
STR_CPPFLAGS = -Iinclude
STR_CFLAGS = -std=c11 -pedantic-errors -Wall -Wextra -Werror -Wconversion -Wshadow \
	-Wformat=2 -Wno-format-nonliteral -Wnull-dereference -Wstrict-prototypes \
	-Wmissing-prototypes
STR_COMPILE = $(CC) $(CPPFLAGS) $(STR_CPPFLAGS) $(CFLAGS) $(STR_CFLAGS)

SRC = src/str.c
TEST_SRC = tests/test_str.c
TEST_BIN = tests/test_str
ASAN_BIN = tests/test_str_asan
ISO_BIN = tests/test_str_iso
RELEASE_BIN = tests/test_str_release
DEMO_SRC = main.c
DEMO_BIN = str_demo
FORMAT_FILES = include/str.h src/str.c tests/test_str.c main.c

.PHONY: test asan iso release format check demo clean

test: $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_BIN): $(TEST_SRC) $(SRC) include/str.h Makefile
	$(STR_COMPILE) -DSTR_TEST $(LDFLAGS) -o $@ $(TEST_SRC) $(SRC) $(LDLIBS)

asan: $(ASAN_BIN)
	./$(ASAN_BIN)

$(ASAN_BIN): $(TEST_SRC) $(SRC) include/str.h Makefile
	$(STR_COMPILE) -DSTR_TEST -O1 -g -fno-omit-frame-pointer \
		-fsanitize=address,undefined -fno-sanitize-recover=all $(LDFLAGS) \
		-o $@ $(TEST_SRC) $(SRC) $(LDLIBS)

iso: $(ISO_BIN)
	./$(ISO_BIN)

$(ISO_BIN): $(TEST_SRC) $(SRC) include/str.h Makefile
	$(STR_COMPILE) -DSTR_TEST -DSTR_STRICT_ISO_OVERLAP $(LDFLAGS) -o $@ $(TEST_SRC) $(SRC) \
		$(LDLIBS)

release: $(RELEASE_BIN)
	./$(RELEASE_BIN)

$(RELEASE_BIN): $(TEST_SRC) $(SRC) include/str.h Makefile
	$(STR_COMPILE) -DSTR_TEST -O3 -DNDEBUG -D_FORTIFY_SOURCE=3 $(LDFLAGS) \
		-o $@ $(TEST_SRC) $(SRC) $(LDLIBS)

demo: $(DEMO_BIN)
	./$(DEMO_BIN)

$(DEMO_BIN): $(DEMO_SRC) $(SRC) include/str.h Makefile
	$(STR_COMPILE) $(LDFLAGS) -o $@ $(DEMO_SRC) $(SRC) $(LDLIBS)

format:
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_FILES)

check: format $(DEMO_BIN) test asan iso release

clean:
	rm -f $(TEST_BIN) $(ASAN_BIN) $(ISO_BIN) $(RELEASE_BIN) $(DEMO_BIN)
