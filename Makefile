# issuewatch -- C11, single-threaded, libcurl + vendored yyjson.
# Warnings are bugs: `make debug` adds -Werror plus ASan/UBSan.

CC      ?= cc
STD      = -std=c11
WARN     = -Wall -Wextra -Wpedantic
CFLAGS  ?= -O2 -march=native
# Tests always run instrumented, so `debug` and `test` must agree exactly --
# any difference would rebuild the world between them via the flags stamp.
DEBUG_CFLAGS = -O0 -g3 -fsanitize=address,undefined -fno-omit-frame-pointer
CPPFLAGS = -Isrc -Ithird_party/yyjson -D_GNU_SOURCE
LDLIBS   = -lm

CURL_CFLAGS := $(shell pkg-config --cflags libcurl)
CURL_LIBS   := $(shell pkg-config --libs libcurl)

BIN      = issuewatch
SRCS     = $(sort $(wildcard src/*.c))
OBJS     = $(SRCS:.c=.o)
# Every test binary links the whole program except its entry point.
LIB_OBJS = $(filter-out src/main.o,$(OBJS))

YYJSON_OBJ = third_party/yyjson/yyjson.o

TEST_SRCS = $(sort $(wildcard tests/test_*.c))
TEST_BINS = $(TEST_SRCS:.c=)

DEPS = $(SRCS:.c=.d) $(TEST_SRCS:.c=.d)

.PHONY: all debug run test clean
.DEFAULT_GOAL := all

all: $(BIN)

debug: CFLAGS := $(DEBUG_CFLAGS)
debug: WARN += -Werror
debug: $(BIN) $(TEST_BINS)

#
# Flags stamp. make compares timestamps, not compiler flags, so without this a
# `make debug` followed by `make` would silently link sanitizer-instrumented
# objects into a release binary -- which fails at link time on yyjson.o, or
# worse, succeeds quietly for our own objects. Rewriting the stamp whenever the
# flags change makes every object depend on the flags that produced it.
#
#
# Recursively expanded (=, not :=) on purpose. A simply-expanded BUILD_ID is
# evaluated once at parse time using the GLOBAL CFLAGS, so the target-specific
# CFLAGS that `debug` and `test` set would never reach it, the stamp would be
# identical in every mode, and nothing would ever rebuild -- which is precisely
# the bug this stamp exists to prevent. Deferring expansion lets the value be
# computed inside the recipe, where target-specific flags are in effect.
#
BUILD_ID = $(CC)|$(STD)|$(CFLAGS)|$(WARN)|$(CPPFLAGS)

.PHONY: FORCE
FORCE:

.buildflags: FORCE
	@if [ ! -f $@ ] || [ "$$(cat $@)" != '$(BUILD_ID)' ]; then \
	    printf '%s' '$(BUILD_ID)' > $@; \
	fi

$(BIN): $(OBJS) $(YYJSON_OBJ)
	$(CC) $(STD) $(CFLAGS) $(WARN) -o $@ $^ $(CURL_LIBS) $(LDLIBS)

src/%.o: src/%.c .buildflags
	$(CC) $(STD) $(CFLAGS) $(WARN) $(CPPFLAGS) $(CURL_CFLAGS) -MMD -MP -c -o $@ $<

# Third-party: compiled without -Wpedantic/-Werror. We do not patch yyjson.
$(YYJSON_OBJ): third_party/yyjson/yyjson.c .buildflags
	$(CC) $(STD) $(CFLAGS) -w $(CPPFLAGS) -c -o $@ $<

tests/test_%: tests/test_%.c $(LIB_OBJS) $(YYJSON_OBJ) .buildflags
	$(CC) $(STD) $(CFLAGS) $(WARN) $(CPPFLAGS) $(CURL_CFLAGS) -MMD -MP \
	    -o $@ $< $(LIB_OBJS) $(YYJSON_OBJ) $(CURL_LIBS) $(LDLIBS)

# Fixtures are addressed relative to the repo root, so run from here.
# LSAN_OPTIONS: linking libcurl drags in libp11-kit, whose ELF constructor leaks
# 46 bytes before main() runs. See tests/lsan.supp -- it suppresses that and
# nothing of ours. UBSan is set to abort so a violation cannot pass silently.
# The parser handles untrusted network input, so tests are only meaningful
# instrumented. These flags match `debug` exactly so the documented
# `make debug && make test` does not rebuild anything in between.
test: CFLAGS := $(DEBUG_CFLAGS)
test: WARN += -Werror
test: export LSAN_OPTIONS = suppressions=tests/lsan.supp
test: export UBSAN_OPTIONS = print_stacktrace=1:halt_on_error=1
test: $(TEST_BINS)
	@fail=0; for t in $(TEST_BINS); do \
	    printf '== %s\n' "$$t"; \
	    ./$$t || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then echo "TESTS FAILED"; exit 1; fi; \
	echo "all tests passed"

run: $(BIN)
	./$(BIN) --dry-run

.PHONY: clean-obj
clean-obj:
	@$(RM) $(OBJS) $(YYJSON_OBJ) $(DEPS) $(TEST_BINS) $(BIN) .buildflags

clean: clean-obj

-include $(DEPS)
