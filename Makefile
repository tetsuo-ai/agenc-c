LIBS = agenc-str agenc-arena agenc-test

.PHONY: check conventions clean

check: conventions
	@set -e; for lib in $(LIBS); do echo "== $$lib =="; $(MAKE) -C $$lib check; done

conventions:
	@set -e; for lib in $(LIBS); do \
		cmp -s LICENSE $$lib/LICENSE || \
			{ echo "$$lib/LICENSE differs from the root copy"; exit 1; }; \
		cmp -s .clang-format $$lib/.clang-format || \
			{ echo "$$lib/.clang-format differs from the root copy"; exit 1; }; \
	done; echo "conventions: root copies match across: $(LIBS)"

clean:
	@for lib in $(LIBS); do $(MAKE) -C $$lib clean; done
