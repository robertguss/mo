# Builds and checks the Mo implementation (written in C, decision D207).
#
#   make              build build/<compiler>/debug/mo
#   make test         build and run the unit tests
#   make sanitize     run the tests under AddressSanitizer and UndefinedBehaviorSanitizer
#   make fuzz-smoke   run every fuzz target briefly (needs clang with libFuzzer)
#   make format       format the C sources with clang-format
#   make tidy         check that results of standard-library calls are used (needs clang-tidy)
#
# ci/check.sh runs every check; CI runs ci/check.sh. Choose a compiler with
# CC=gcc or CC=clang. Each compiler builds into its own directory, so switching
# never forces a full rebuild. Changing this Makefile rebuilds everything.

CC ?= cc
FUZZ_CC ?= clang
FUZZ_SECONDS ?= 20
BUILD := build/$(notdir $(firstword $(CC)))
FUZZ_BUILD := build/fuzz

CSTD := -std=c17
WARNINGS := -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion \
	-Wswitch-enum -Wimplicit-fallthrough -Wstrict-prototypes -Wmissing-prototypes -Wvla \
	-Wformat=2 -Wcast-qual -Wundef -Wnull-dereference
COMMON := $(CSTD) $(WARNINGS) -MMD -MP
DEBUG := -O0 -g
SANITIZE := -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all
FUZZ := -O1 -g -fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all

LIB_SOURCES := src/base.c src/cli.c
MAIN_SOURCE := src/main.c
TEST_SOURCES := $(wildcard tests/test_*.c)
FUZZ_SOURCES := $(wildcard fuzz/fuzz_*.c)
C_FILES := $(wildcard src/*.c src/*.h tests/*.c tests/*.h fuzz/*.c)
TIDY_FILES := $(wildcard src/*.c tests/*.c fuzz/*.c)

objects = $(patsubst %.c,$(1)/%.o,$(2))

DEBUG_TESTS := $(patsubst tests/%.c,$(BUILD)/debug/tests/%.test,$(TEST_SOURCES))
SANITIZE_TESTS := $(patsubst tests/%.c,$(BUILD)/sanitize/tests/%.test,$(TEST_SOURCES))
FUZZERS := $(patsubst fuzz/%.c,$(FUZZ_BUILD)/bin/%,$(FUZZ_SOURCES))

# Stop at the first sanitizer error. These are only defaults: options you have set in your
# environment, or on the make command line, win. Leak detection is not forced, because Apple
# clang's AddressSanitizer has no LeakSanitizer and aborts when asked for it; it is on by
# default on Linux.
ASAN_OPTIONS ?= abort_on_error=1
UBSAN_OPTIONS ?= halt_on_error=1:print_stacktrace=1
export ASAN_OPTIONS UBSAN_OPTIONS

.PHONY: all test sanitize fuzz fuzz-smoke format format-check tidy clean

all: $(BUILD)/debug/mo

test: $(DEBUG_TESTS) $(BUILD)/debug/mo
	@for t in $(DEBUG_TESTS); do $$t || exit 1; done
	@$(BUILD)/debug/mo --version >/dev/null

sanitize: $(SANITIZE_TESTS) $(BUILD)/sanitize/mo
	@for t in $(SANITIZE_TESTS); do $$t || exit 1; done
	@$(BUILD)/sanitize/mo >/dev/null && $(BUILD)/sanitize/mo --version >/dev/null

fuzz: $(FUZZERS)

# A crash artifact fails the run that found it. Start with none, so that one saved by an
# earlier run does not keep failing the runs after the bug is fixed.
fuzz-smoke: $(FUZZERS)
	@rm -rf $(FUZZ_BUILD)/artifacts
	@mkdir -p $(FUZZ_BUILD)/artifacts
	@for f in $(FUZZERS); do \
		name=$$(basename $$f); corpus=$(FUZZ_BUILD)/corpus/$$name; mkdir -p $$corpus; \
		if [ -d fuzz/corpus/$$name ]; then cp -R fuzz/corpus/$$name/. $$corpus/; fi; \
		log=$(FUZZ_BUILD)/$$name.log; echo "fuzzing $$name for $(FUZZ_SECONDS)s"; \
		$$f -max_total_time=$(FUZZ_SECONDS) -print_final_stats=1 \
			-artifact_prefix=$(FUZZ_BUILD)/artifacts/ $$corpus >$$log 2>&1; status=$$?; \
		grep -E '^(Done|stat::(number_of_executed_units|new_units_added|peak_rss_mb))' $$log; \
		if [ $$status -ne 0 ] || ls $(FUZZ_BUILD)/artifacts/* >/dev/null 2>&1; then \
			tail -n 40 $$log; echo "error: fuzzing $$name failed (exit status $$status)"; exit 1; fi; \
	done

format:
	clang-format -i $(C_FILES)

format-check:
	clang-format --dry-run --Werror $(C_FILES)

# cert-err33-c flags a call to a standard-library function whose result is ignored, such as
# fflush or fprintf: the functions on CERT rule ERR33-C's list, which does not include printf,
# puts or putchar. Casting the call to (void) ignores the result on purpose. Mo's own functions
# are covered by MO_MUST_USE. The flags are the debug build's, so clang-tidy sees the code as
# the compiler does (it ignores the -M options).
tidy:
	clang-tidy --quiet -checks='-*,cert-err33-c' -warnings-as-errors='*' -header-filter='.*' \
		$(TIDY_FILES) -- $(COMMON) $(DEBUG)

clean:
	rm -rf build

# Object files also depend on this Makefile, so a change to the flags rebuilds them. It comes
# after the source file because $< is the first prerequisite.
$(BUILD)/debug/%.o: %.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(COMMON) $(DEBUG) -c $< -o $@

$(BUILD)/sanitize/%.o: %.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(COMMON) $(SANITIZE) -c $< -o $@

$(FUZZ_BUILD)/%.o: %.c Makefile
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(COMMON) $(FUZZ) -c $< -o $@

$(BUILD)/debug/mo: $(call objects,$(BUILD)/debug,$(LIB_SOURCES) $(MAIN_SOURCE))
	$(CC) $(DEBUG) $^ -o $@

$(BUILD)/sanitize/mo: $(call objects,$(BUILD)/sanitize,$(LIB_SOURCES) $(MAIN_SOURCE))
	$(CC) $(SANITIZE) $^ -o $@

$(BUILD)/debug/tests/%.test: $(BUILD)/debug/tests/%.o $(call objects,$(BUILD)/debug,$(LIB_SOURCES))
	$(CC) $(DEBUG) $^ -o $@

$(BUILD)/sanitize/tests/%.test: $(BUILD)/sanitize/tests/%.o $(call objects,$(BUILD)/sanitize,$(LIB_SOURCES))
	$(CC) $(SANITIZE) $^ -o $@

$(FUZZ_BUILD)/bin/%: $(FUZZ_BUILD)/fuzz/%.o $(call objects,$(FUZZ_BUILD),$(LIB_SOURCES))
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(FUZZ) $^ -o $@

# Keep object files between builds, so only changed files recompile.
.SECONDARY:

-include $(shell find build -name '*.d' 2>/dev/null)
