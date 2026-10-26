# Builds and checks the Mo implementation (written in C, decision D207).
#
#   make              build build/<compiler>/debug/mo
#   make test         build and run the unit tests
#   make sanitize     run the tests under AddressSanitizer and UndefinedBehaviorSanitizer
#   make host-rename-stress  stress both resolvers against concurrent renames, also sanitized
#   make fuzz-smoke   run every fuzz target briefly (needs clang with libFuzzer)
#   make format       format the C sources with clang-format
#   make deps         check that each module includes only what architecture section 2 allows
#   make library-map  check that docs/design/library.md has a row for every line of the tables
#                     prims.def and hostops.def, and names only their lines
#   make tidy         check that results of standard-library calls are used (needs clang-tidy)
#   make grammar      check that Mo code parses with the executable grammar (needs bison):
#                     the grammar's own tests, examples/ and the reference's examples
#   make formal       build Mo Core in Lean (formal/) and check every proof (needs elan)
#   make cbmc         prove the bytecode reader, the arena, the verifier's transfer
#                     functions, the primitives' arithmetic and UTF-8, lexical path
#                     containment and resolution, reference counts and hash tables (needs CBMC)
#
# ci/check.sh runs every check. Choose a compiler with
# CC=gcc or CC=clang. Each compiler builds into its own directory, so switching
# never forces a full rebuild. Changing this Makefile rebuilds everything.

CC ?= cc
FUZZ_CC ?= clang
FUZZ_SECONDS ?= 5
BUILD := build/$(notdir $(firstword $(CC)))
FUZZ_BUILD := build/fuzz

CSTD := -std=c17
# Headers are included by their path in src/, which starts with their module, such as
# "base/base.h".
INCLUDES := -Isrc -Ibuild/generated
ifeq ($(shell uname -s),Darwin)
INCLUDES += -D_DARWIN_C_SOURCE
endif
THREADS := -pthread
WARNINGS := -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion \
	-Wswitch-enum -Wimplicit-fallthrough -Wstrict-prototypes -Wmissing-prototypes -Wvla \
	-Wformat=2 -Wcast-qual -Wundef -Wnull-dereference
COMMON := $(CSTD) $(INCLUDES) $(WARNINGS) $(THREADS) -MMD -MP
DEBUG := -O0 -g $(THREADS)
SANITIZE := -O1 -g $(THREADS) -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all
FUZZ := -O1 -g $(THREADS) -fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all
LDLIBS += -lm

# Each directory in src/ is a module (architecture section 2), and every C file in it is built.
# src/tools/main.c is the mo command's main function, so only mo links it.
MAIN_SOURCE := src/tools/main.c
LIB_SOURCES := $(filter-out $(MAIN_SOURCE),$(sort $(wildcard src/*/*.c)))
BASE_SOURCES := $(filter src/base/%,$(LIB_SOURCES))
# build/sources lists the C files of src/, and is rewritten only when one is added or removed:
# removing a file changes no other prerequisite of the programs that linked it.
SOURCE_LIST := build/sources
TEST_SOURCES := $(wildcard tests/test_*.c)
FUZZ_SOURCES := $(wildcard fuzz/fuzz_*.c)
C_FILES := $(wildcard src/*/*.c src/*/*.h src/core/*.def tests/*.c tests/*.h fuzz/*.c grammar/*.c grammar/*.h ci/*.c cbmc/*.c)
TIDY_FILES := $(wildcard src/*/*.c tests/*.c fuzz/*.c grammar/*.c ci/*.c)

# The executable grammar (grammar/): Bison builds its parser from grammar/mo.y. The code Bison
# writes doesn't meet Mo's own warning flags, so it gets the plain ones. Of src/, the grammar
# uses only base.
GRAMMAR_GENERATED := build/grammar
GRAMMAR_SOURCES := grammar/lexer.c grammar/ast.c
GRAMMAR_INCLUDES := -Igrammar -I$(GRAMMAR_GENERATED)
# LR stack height can follow flat prefix chains, not source nesting. Its default
# 10,000 states reject valid source; bound it by the input budget instead.
# Bison sets yynerrs and does not read it when the grammar does not count errors.
GRAMMAR_PARSER_WARNINGS := $(CSTD) $(INCLUDES) -Wall -Wextra -Werror -MMD -MP \
	-Wno-unused-but-set-variable \
	-include src/base/limits.h '-DYYMAXDEPTH=(4 * MO_LIMIT_FILE_SIZE)'
grammar_objects = $(patsubst grammar/%.c,$(1)/grammar/%.o,$(GRAMMAR_SOURCES)) \
	$(1)/grammar/mo.tab.o $(call objects,$(1),$(BASE_SOURCES))
GRAMMAR_CHECK = $(1) --fragment grammar/tests/good/*.mo && \
	$(1) --fragment --expect-error grammar/tests/bad/*.mo && \
	$(1) --expect-error grammar/tests/bad-files/*.mo && \
	$(1) examples/*.mo && \
	$(1) docs/reference/README.md

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

.PHONY: all test sanitize test-executables sanitize-executables fuzz fuzz-smoke format format-check deps library-map tidy grammar formal cbmc \
	host-rename-stress clean FORCE

all: $(BUILD)/debug/mo

# Ordinary prerequisites keep an up-to-date build usable without Python.
# Mutually exclusive stamps also detect the optional guide appearing/disappearing,
# even if a newly added guide has an older timestamp than the generated header.
GUIDE_SOURCE := $(wildcard docs/agent-guide.md)
GUIDE_STATE := $(if $(GUIDE_SOURCE),present,absent)
GUIDE_STAMP := build/generated/tools/guide-$(GUIDE_STATE)
# A missing stamp forces this invocation, even on a coarse-timestamp filesystem;
# after creation it is an ordinary prerequisite, not a per-invocation generator.
.PHONY: $(if $(wildcard $(GUIDE_STAMP)),,$(GUIDE_STAMP))
$(GUIDE_STAMP):
	@mkdir -p $(dir $@)
	@rm -f build/generated/tools/guide-present build/generated/tools/guide-absent
	@touch $@

build/generated/tools/embedded.h: ci/embed_docs.py docs/reference/README.md $(GUIDE_SOURCE) $(GUIDE_STAMP)
	python3 ci/embed_docs.py $@
	@touch $@

# Depend directly on the state too: a same-tick header rewrite must still
# recompile the document consumer when the guide appears or disappears.
$(BUILD)/debug/src/tools/execute.o $(BUILD)/sanitize/src/tools/execute.o \
$(FUZZ_BUILD)/src/tools/execute.o: build/generated/tools/embedded.h $(GUIDE_STAMP)

# The test executables without running them, for ci/check.sh's drivers group.
test-executables: $(DEBUG_TESTS) $(BUILD)/debug/mo
sanitize-executables: $(SANITIZE_TESTS) $(BUILD)/sanitize/mo

# Each test runs as its own target, so make -j runs them side by side (they share
# no files: each makes its own temporary folder).
test: $(DEBUG_TESTS:=.run) $(BUILD)/debug/mo
	@$(BUILD)/debug/mo --version >/dev/null

sanitize: $(SANITIZE_TESTS:=.run) $(BUILD)/sanitize/mo
	@$(BUILD)/sanitize/mo --version >/dev/null

.PHONY: $(DEBUG_TESTS:=.run) $(SANITIZE_TESTS:=.run)
$(DEBUG_TESTS:=.run) $(SANITIZE_TESTS:=.run): %.run: %
	@$<

# Timing-dependent stress stays a separate target, not a unit-test gate.
host-rename-stress: $(BUILD)/debug/tests/host_rename_stress.test $(BUILD)/sanitize/tests/host_rename_stress.test
	$(BUILD)/debug/tests/host_rename_stress.test
	$(BUILD)/sanitize/tests/host_rename_stress.test

# The grammar checks run on the debug build, then again under the sanitizers.
grammar: $(BUILD)/debug/mo-grammar $(BUILD)/sanitize/mo-grammar
	@$(call GRAMMAR_CHECK,$(BUILD)/debug/mo-grammar)
	@($(call GRAMMAR_CHECK,$(BUILD)/sanitize/mo-grammar)) >/dev/null

# Mo Core in Lean: lake builds the model, its twelve sample programs and their checks, and
# every proof, plus the mocore oracle. formal/lean-toolchain names the Lean version,
# which elan fetches on first use.
formal:
	cd formal && lake build MoCore mocore
	sh ci/test_mocore.sh

# CBMC proofs (cbmc/README.md): the bytecode reader on every input of up to MO_CBMC_INPUT bytes,
# the arena on every size, the verifier's transfer functions on any one instruction, the
# primitives' arithmetic and UTF-8 on every operand, lexical path containment, bounded path
# resolution, reference-count updates and hash-table probing.
cbmc:
	bash cbmc/prove.sh

fuzz: $(FUZZERS)

# A crash artifact fails the run that found it. Start with none, so that one saved by an
# earlier run does not keep failing the runs after the bug is fixed.
fuzz-smoke: $(FUZZERS) $(BUILD)/debug/tests/test_bytecode_verify.test $(BUILD)/debug/tests/test_bytecode_text.test $(BUILD)/debug/tests/test_vm.test
	@rm -rf $(FUZZ_BUILD)/artifacts
	@mkdir -p $(FUZZ_BUILD)/artifacts
	@mkdir -p $(FUZZ_BUILD)/bytecode-fixtures
	$(BUILD)/debug/tests/test_bytecode_verify.test --corpus $(FUZZ_BUILD)/bytecode-fixtures
	$(BUILD)/debug/tests/test_bytecode_text.test --corpus $(FUZZ_BUILD)/bytecode-fixtures
	@mkdir -p $(FUZZ_BUILD)/vm-fixtures
	$(BUILD)/debug/tests/test_vm.test --corpus $(FUZZ_BUILD)/vm-fixtures
	python3 ci/check_primitives.py --corpus $(FUZZ_BUILD)/corpus/fuzz_prim
	python3 ci/check_unicode16.py --corpus $(FUZZ_BUILD)/corpus/fuzz_prim
	@for f in $(FUZZERS); do \
		base=$$(basename $$f); variants=small; \
		if [ "$$base" = fuzz_bytecode_io ]; then variants="small large"; fi; \
		for variant in $$variants; do \
			name=$$base; options=; \
			if [ "$$base" = fuzz_bytecode_io ]; then options="-max_len=4096 -timeout=15"; fi; \
			if [ "$$base" = fuzz_bytecode_text ]; then options="-max_len=16777216 -timeout=30"; fi; \
			if [ "$$base" = fuzz_vm ] || [ "$$base" = fuzz_bytecode_verify ]; then options="-max_len=65536 -timeout=15"; fi; \
			if [ "$$variant" = large ]; then \
				name=$${base}_large; options="-max_len=134217728 -timeout=30"; fi; \
			corpus=$(FUZZ_BUILD)/corpus/$$name; \
			if [ "$$base" = fuzz_core_text ] || [ "$$base" = fuzz_core_check ] || [ "$$base" = fuzz_codegen ]; then rm -rf $$corpus; fi; \
			mkdir -p $$corpus; \
			if [ -d fuzz/corpus/$$name ]; then cp -R fuzz/corpus/$$name/. $$corpus/; fi; \
			if [ "$$base" = fuzz_bytecode_io ]; then cp $(FUZZ_BUILD)/bytecode-fixtures/*.bc $$corpus/; fi; \
			if [ "$$base" = fuzz_bytecode_text ]; then cp $(FUZZ_BUILD)/bytecode-fixtures/* $$corpus/; fi; \
			if [ "$$base" = fuzz_vm ] || [ "$$base" = fuzz_bytecode_verify ]; then cp $(FUZZ_BUILD)/vm-fixtures/*.bc $(FUZZ_BUILD)/bytecode-fixtures/*.bc $$corpus/; fi; \
			if [ "$$name" = fuzz_core_text ] || [ "$$name" = fuzz_core_check ] || [ "$$name" = fuzz_codegen ]; then \
				cp tests/core/*.core fuzz/corpus/fuzz_core_text/*.core $$corpus/; fi; \
			if [ "$$name" = fuzz_check ]; then cp tests/check/diagnostics/e*.mo $$corpus/; fi; \
			if [ "$$name" = fuzz_bodies ]; then cp tests/check/diagnostics/e*.mo examples/*.mo $$corpus/; fi; \
			if [ "$$name" = fuzz_bodies ]; then \
				awk 'BEGIN { print "module Main\nfn run(a: Bool, b: Bool, c: Bool, d: Range, e: Int, f: Int, g: Int, h: fn(Bool) : Int) : Bool"; for (i = 0; i < 255; i++) printf "a implies b or c and not d == e .. f + g * -h("; printf "a"; for (i = 0; i < 255; i++) printf ")"; print "\nend" }' >$$corpus/max-nesting.mo; \
				awk 'BEGIN { print "module Main\nfn duplicate(value: T) : (T, T)\n(value, value)\nend\nfn ignore(_value: T)\nend\nfn run()\n_v0 = 1"; for (i = 1; i <= 250; i++) printf "_v%d = duplicate(_v%d)\n", i, i-1; for (i = 0; i < 40000; i++) print "ignore(_v250)"; print "end" }' >$$corpus/work-budget.mo; \
			fi; \
			if [ "$$name" = fuzz_program ]; then \
				awk 'BEGIN { print "module Main\nfn run(a: Bool, b: Bool, c: Bool, d: Range, e: Int, f: Int, g: Int, h: fn(Bool) : Int) : Bool"; for (i = 0; i < 255; i++) printf "a implies b or c and not d == e .. f + g * -h("; printf "a"; for (i = 0; i < 255; i++) printf ")"; print "\nend\nfn main()\nend" }' >$$corpus/max-nesting.mo; \
			fi; \
			if [ "$$name" = fuzz_syntax ]; then cp grammar/tests/good/*.mo grammar/tests/bad/*.mo examples/*.mo $$corpus/; fi; \
			if [ "$$name" = fuzz_grammar ]; then \
				cp grammar/tests/good/*.mo examples/*.mo $$corpus/; \
				cp -R fuzz/corpus/fuzz_grammar/. $$corpus/; \
				if ! cmp -s $$corpus/job-queue.mo fuzz/corpus/fuzz_grammar/job-queue.mo || \
					[ ! -f $$corpus/expenses.mo ] || [ ! -f $$corpus/layout.mo ]; then \
					echo "error: fuzz_grammar smoke corpus lost unique seeds or live sources"; exit 1; fi; \
			fi; \
			if [ "$$name" = fuzz_format ]; then \
				mkdir -p $$corpus/syntax; \
				cp grammar/tests/good/*.mo examples/*.mo $$corpus/; \
				cp fuzz/corpus/fuzz_syntax/* $$corpus/syntax/; \
			fi; \
			if [ "$$name" = fuzz_syntax ]; then \
				awk 'BEGIN { printf "x = 0"; for (i = 0; i < 30000; i++) printf " +\n1"; print "" }' >$$corpus/flat-binary.mo; \
				awk 'BEGIN { printf "x = "; for (i = 0; i < 12000; i++) printf "not "; print "true" }' >$$corpus/flat-prefix.mo; \
			fi; \
			log=$(FUZZ_BUILD)/$$name.log; echo "fuzzing $$name for $(FUZZ_SECONDS)s"; \
			dictionary=; if [ -f fuzz/$$name.dict ]; then dictionary=-dict=fuzz/$$name.dict; fi; \
			$$f $$options $$dictionary -max_total_time=$(FUZZ_SECONDS) -print_final_stats=1 \
				-artifact_prefix=$(FUZZ_BUILD)/artifacts/ $$corpus >$$log 2>&1; status=$$?; \
			grep -E '^(Done|stat::(number_of_executed_units|new_units_added|peak_rss_mb))' $$log; \
			if [ $$status -ne 0 ] || ls $(FUZZ_BUILD)/artifacts/* >/dev/null 2>&1; then \
				tail -n 40 $$log; echo "error: fuzzing $$name failed (exit status $$status)"; exit 1; fi; \
		done; \
	done

format:
	clang-format -i $(C_FILES)

format-check:
	clang-format --dry-run --Werror $(C_FILES)

# The module rules of architecture section 2, which ci/deps.sh keeps in a table. Its test runs
# first, to show that the check catches each kind of include it must refuse.
deps:
	sh ci/test_deps.sh
	sh ci/deps.sh src

# docs/design/library.md against the tables it maps, src/base/prims.def and src/base/hostops.def,
# which ci/library_map.sh reads. Its test runs first, to show that the check catches each kind of
# disagreement it must refuse.
library-map: $(BUILD)/debug/module-path
	MO_MODULE_PATH="$(abspath $(BUILD)/debug/module-path)" sh ci/test_library_map.sh
	MO_MODULE_PATH="$(abspath $(BUILD)/debug/module-path)" sh ci/library_map.sh

# cert-err33-c flags a call to a standard-library function whose result is ignored, such as
# fflush or fprintf: the functions on CERT rule ERR33-C's list, which does not include printf,
# puts or putchar. Casting the call to (void) ignores the result on purpose. Mo's own functions
# are covered by MO_MUST_USE. The flags are the debug build's, so clang-tidy sees the code as
# the compiler does (it ignores the -M options).
tidy: $(GRAMMAR_GENERATED)/mo.tab.h build/generated/tools/embedded.h
	clang-tidy --quiet -checks='-*,cert-err33-c' -warnings-as-errors='*' -header-filter='^(src|tests|fuzz|grammar)/' \
		$(TIDY_FILES) -- $(COMMON) $(DEBUG) $(GRAMMAR_INCLUDES)

clean:
	rm -rf build

# Object files also depend on this Makefile, so a change to the flags rebuilds them, and on the
# list of C files in src/, so adding or removing one rebuilds everything, and no program keeps a
# file that is gone. They come after the source file because $< is the first prerequisite.
$(BUILD)/debug/%.o: %.c Makefile $(SOURCE_LIST)
	@mkdir -p $(dir $@)
	$(CC) $(COMMON) $(DEBUG) -c $< -o $@

$(BUILD)/sanitize/%.o: %.c Makefile $(SOURCE_LIST)
	@mkdir -p $(dir $@)
	$(CC) $(COMMON) $(SANITIZE) -c $< -o $@

$(FUZZ_BUILD)/%.o: %.c Makefile $(SOURCE_LIST)
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(COMMON) $(FUZZ) -c $< -o $@

$(SOURCE_LIST): FORCE
	@mkdir -p $(dir $@)
	@echo '$(LIB_SOURCES)' | cmp -s - $@ || echo '$(LIB_SOURCES)' >$@

$(BUILD)/debug/mo: $(call objects,$(BUILD)/debug,$(LIB_SOURCES) $(MAIN_SOURCE))
	$(CC) $(DEBUG) $^ -o $@ $(LDLIBS)

$(BUILD)/debug/module-path: $(BUILD)/debug/ci/module_path.o $(call objects,$(BUILD)/debug,$(BASE_SOURCES))
	$(CC) $(DEBUG) $^ -o $@

$(BUILD)/sanitize/mo: $(call objects,$(BUILD)/sanitize,$(LIB_SOURCES) $(MAIN_SOURCE))
	$(CC) $(SANITIZE) $^ -o $@ $(LDLIBS)

$(BUILD)/debug/tests/%.test: $(BUILD)/debug/tests/%.o $(call objects,$(BUILD)/debug,$(LIB_SOURCES))
	$(CC) $(DEBUG) $^ -o $@ $(LDLIBS)

$(BUILD)/sanitize/tests/%.test: $(BUILD)/sanitize/tests/%.o $(call objects,$(BUILD)/sanitize,$(LIB_SOURCES))
	$(CC) $(SANITIZE) $^ -o $@ $(LDLIBS)

# Only the compiler-fault test substitutes these two module boundaries.
COMPILER_FAULT_SOURCES := src/core/check.c src/codegen/compile.c
COMPILER_FAULT_RENAMES := -Dmo_arena_allocate_array=mo_compiler_fault_allocate_array \
	-Dmo_bytecode_write=mo_compiler_fault_bytecode_write
$(BUILD)/debug/compiler-faults/%.o: src/%.c Makefile $(SOURCE_LIST)
	@mkdir -p $(dir $@)
	$(CC) $(COMMON) $(DEBUG) $(COMPILER_FAULT_RENAMES) -c $< -o $@

$(BUILD)/sanitize/compiler-faults/%.o: src/%.c Makefile $(SOURCE_LIST)
	@mkdir -p $(dir $@)
	$(CC) $(COMMON) $(SANITIZE) $(COMPILER_FAULT_RENAMES) -c $< -o $@

$(BUILD)/debug/tests/test_compiler_faults.test: $(BUILD)/debug/tests/test_compiler_faults.o $(call objects,$(BUILD)/debug,$(filter-out $(COMPILER_FAULT_SOURCES),$(LIB_SOURCES))) $(patsubst src/%.c,$(BUILD)/debug/compiler-faults/%.o,$(COMPILER_FAULT_SOURCES))
	$(CC) $(DEBUG) $^ -o $@ $(LDLIBS)

$(BUILD)/sanitize/tests/test_compiler_faults.test: $(BUILD)/sanitize/tests/test_compiler_faults.o $(call objects,$(BUILD)/sanitize,$(filter-out $(COMPILER_FAULT_SOURCES),$(LIB_SOURCES))) $(patsubst src/%.c,$(BUILD)/sanitize/compiler-faults/%.o,$(COMPILER_FAULT_SOURCES))
	$(CC) $(SANITIZE) $^ -o $@ $(LDLIBS)

# Test-only stack failure at the command's POSIX edge, using the same portable
# compile-time renaming as the VM faults. Production links ordinary execute.o.
$(BUILD)/debug/cli-faults/execute.o: src/tools/execute.c Makefile $(SOURCE_LIST) build/generated/tools/embedded.h $(GUIDE_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(COMMON) $(DEBUG) -Dmo_with_stack=mo_cli_fault_stack -c $< -o $@

$(BUILD)/sanitize/cli-faults/execute.o: src/tools/execute.c Makefile $(SOURCE_LIST) build/generated/tools/embedded.h $(GUIDE_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(COMMON) $(SANITIZE) -Dmo_with_stack=mo_cli_fault_stack -c $< -o $@

$(BUILD)/debug/tests/test_cli_faults.test: $(BUILD)/debug/tests/test_cli_faults.o $(call objects,$(BUILD)/debug,$(filter-out src/tools/execute.c,$(LIB_SOURCES))) $(BUILD)/debug/cli-faults/execute.o
	$(CC) $(DEBUG) $^ -o $@ $(LDLIBS)

$(BUILD)/sanitize/tests/test_cli_faults.test: $(BUILD)/sanitize/tests/test_cli_faults.o $(call objects,$(BUILD)/sanitize,$(filter-out src/tools/execute.c,$(LIB_SOURCES))) $(BUILD)/sanitize/cli-faults/execute.o
	$(CC) $(SANITIZE) $^ -o $@ $(LDLIBS)

# The promise clock uses the same portable POSIX-edge substitution as above.
# No production injection flags, environment variables or linker interposition.
$(BUILD)/debug/cli-clock/deadline.o: src/tools/deadline.c Makefile $(SOURCE_LIST)
	@mkdir -p $(dir $@)
	$(CC) $(COMMON) $(DEBUG) -Dclock_gettime=mo_cli_deadline_clock -c $< -o $@

$(BUILD)/sanitize/cli-clock/deadline.o: src/tools/deadline.c Makefile $(SOURCE_LIST)
	@mkdir -p $(dir $@)
	$(CC) $(COMMON) $(SANITIZE) -Dclock_gettime=mo_cli_deadline_clock -c $< -o $@

$(BUILD)/debug/tests/test_deadline.test: $(BUILD)/debug/tests/test_deadline.o $(call objects,$(BUILD)/debug,$(filter-out src/tools/deadline.c,$(LIB_SOURCES))) $(BUILD)/debug/cli-clock/deadline.o
	$(CC) $(DEBUG) $^ -o $@ $(LDLIBS)

$(BUILD)/sanitize/tests/test_deadline.test: $(BUILD)/sanitize/tests/test_deadline.o $(call objects,$(BUILD)/sanitize,$(filter-out src/tools/deadline.c,$(LIB_SOURCES))) $(BUILD)/sanitize/cli-clock/deadline.o
	$(CC) $(SANITIZE) $^ -o $@ $(LDLIBS)

$(BUILD)/debug/lower-source: $(BUILD)/debug/ci/lower_source.o $(call objects,$(BUILD)/debug,$(LIB_SOURCES))
	$(CC) $(DEBUG) $^ -o $@ $(LDLIBS)

$(BUILD)/sanitize/lower-source: $(BUILD)/sanitize/ci/lower_source.o $(call objects,$(BUILD)/sanitize,$(LIB_SOURCES))
	$(CC) $(SANITIZE) $^ -o $@ $(LDLIBS)

# Run fixtures exercise real Clock dispatch with the World clock supplied at
# the syscall boundary. Only this executable renames the clock call.
$(BUILD)/debug/vm-run/host.o: src/host/host.c Makefile $(SOURCE_LIST)
	@mkdir -p $(dir $@)
	$(CC) $(COMMON) $(DEBUG) -Dclock_gettime=mo_vm_run_clock -c $< -o $@

$(BUILD)/sanitize/vm-run/host.o: src/host/host.c Makefile $(SOURCE_LIST)
	@mkdir -p $(dir $@)
	$(CC) $(COMMON) $(SANITIZE) -Dclock_gettime=mo_vm_run_clock -c $< -o $@

$(BUILD)/debug/tests/test_vm.test: $(BUILD)/debug/tests/test_vm.o $(call objects,$(BUILD)/debug,$(filter-out src/host/host.c,$(LIB_SOURCES))) $(BUILD)/debug/vm-run/host.o
	$(CC) $(DEBUG) $^ -o $@ $(LDLIBS)

$(BUILD)/sanitize/tests/test_vm.test: $(BUILD)/sanitize/tests/test_vm.o $(call objects,$(BUILD)/sanitize,$(filter-out src/host/host.c,$(LIB_SOURCES))) $(BUILD)/sanitize/vm-run/host.o
	$(CC) $(SANITIZE) $^ -o $@ $(LDLIBS)

# Fault injection only in VM initialization, reporting and heap allocation.
# Compile-time renaming also works with Apple's linker.
VM_FAULT_SOURCES := src/heap/heap.c src/vm/image.c src/vm/defect.c
VM_FAULT_RENAMES := -Dcalloc=mo_vm_fault_calloc -Dgetentropy=mo_vm_fault_entropy \
	-Dmo_image_registers_readable=mo_vm_fault_readable
$(BUILD)/debug/vm-faults/%.o: src/%.c Makefile $(SOURCE_LIST)
	@mkdir -p $(dir $@)
	$(CC) $(COMMON) $(DEBUG) $(VM_FAULT_RENAMES) -c $< -o $@

$(BUILD)/sanitize/vm-faults/%.o: src/%.c Makefile $(SOURCE_LIST)
	@mkdir -p $(dir $@)
	$(CC) $(COMMON) $(SANITIZE) $(VM_FAULT_RENAMES) -c $< -o $@

$(BUILD)/debug/tests/test_vm_faults.test: $(BUILD)/debug/tests/test_vm_faults.o $(call objects,$(BUILD)/debug,$(filter-out $(VM_FAULT_SOURCES),$(LIB_SOURCES))) $(patsubst src/%.c,$(BUILD)/debug/vm-faults/%.o,$(VM_FAULT_SOURCES))
	$(CC) $(DEBUG) $^ -o $@ $(LDLIBS)

$(BUILD)/sanitize/tests/test_vm_faults.test: $(BUILD)/sanitize/tests/test_vm_faults.o $(call objects,$(BUILD)/sanitize,$(filter-out $(VM_FAULT_SOURCES),$(LIB_SOURCES))) $(patsubst src/%.c,$(BUILD)/sanitize/vm-faults/%.o,$(VM_FAULT_SOURCES))
	$(CC) $(SANITIZE) $^ -o $@ $(LDLIBS)

# Only this test substitutes the resolver's native calls and lowers host read
# limits. Heap builders and every other executable retain the production limits.
HOST_RESOLVER_SOURCES := src/host/path.c src/host/console.c src/host/real-files.c src/host/host.c
HOST_RESOLVER_OPTIONS := -include tests/host_small_limits.h -Dreadlinkat=mo_host_resolver_readlink \
	-Dsyscall=mo_host_resolver_syscall -Dopen=mo_host_resolver_open \
	-Dsetrlimit=mo_host_resolver_setrlimit
$(BUILD)/debug/host-resolver/%.o: src/%.c tests/host_small_limits.h Makefile $(SOURCE_LIST)
	@mkdir -p $(dir $@)
	$(CC) $(COMMON) $(DEBUG) $(HOST_RESOLVER_OPTIONS) -c $< -o $@

$(BUILD)/sanitize/host-resolver/%.o: src/%.c tests/host_small_limits.h Makefile $(SOURCE_LIST)
	@mkdir -p $(dir $@)
	$(CC) $(COMMON) $(SANITIZE) $(HOST_RESOLVER_OPTIONS) -c $< -o $@

$(BUILD)/debug/tests/test_host_resolver.test: $(BUILD)/debug/tests/test_host_resolver.o $(call objects,$(BUILD)/debug,$(filter-out $(HOST_RESOLVER_SOURCES),$(LIB_SOURCES))) $(patsubst src/%.c,$(BUILD)/debug/host-resolver/%.o,$(HOST_RESOLVER_SOURCES))
	$(CC) $(DEBUG) $^ -o $@ $(LDLIBS)

$(BUILD)/sanitize/tests/test_host_resolver.test: $(BUILD)/sanitize/tests/test_host_resolver.o $(call objects,$(BUILD)/sanitize,$(filter-out $(HOST_RESOLVER_SOURCES),$(LIB_SOURCES))) $(patsubst src/%.c,$(BUILD)/sanitize/host-resolver/%.o,$(HOST_RESOLVER_SOURCES))
	$(CC) $(SANITIZE) $^ -o $@ $(LDLIBS)

# As above, substitute only the translation units owning native host calls.
# Neither the ordinary host objects nor the fixture's heap use these wrappers.
HOST_FAULT_SOURCES := src/host/host.c src/host/random.c src/host/console.c src/host/real-files.c
HOST_FAULT_RENAMES := -Dclock_gettime=mo_host_fault_clock -Dgetentropy=mo_host_fault_entropy \
	-Dread=mo_host_fault_read -Dwrite=mo_host_fault_write -Dwritev=mo_host_fault_writev \
	-Dpoll=mo_host_fault_poll -Dftruncate=mo_host_fault_truncate
$(BUILD)/debug/host-faults/%.o: src/%.c Makefile $(SOURCE_LIST)
	@mkdir -p $(dir $@)
	$(CC) $(COMMON) $(DEBUG) $(HOST_FAULT_RENAMES) -c $< -o $@

$(BUILD)/sanitize/host-faults/%.o: src/%.c Makefile $(SOURCE_LIST)
	@mkdir -p $(dir $@)
	$(CC) $(COMMON) $(SANITIZE) $(HOST_FAULT_RENAMES) -c $< -o $@

$(BUILD)/debug/tests/test_host_faults.test: $(BUILD)/debug/tests/test_host_faults.o $(call objects,$(BUILD)/debug,$(filter-out $(HOST_FAULT_SOURCES),$(LIB_SOURCES))) $(patsubst src/%.c,$(BUILD)/debug/host-faults/%.o,$(HOST_FAULT_SOURCES))
	$(CC) $(DEBUG) $^ -o $@ $(LDLIBS)

$(BUILD)/sanitize/tests/test_host_faults.test: $(BUILD)/sanitize/tests/test_host_faults.o $(call objects,$(BUILD)/sanitize,$(filter-out $(HOST_FAULT_SOURCES),$(LIB_SOURCES))) $(patsubst src/%.c,$(BUILD)/sanitize/host-faults/%.o,$(HOST_FAULT_SOURCES))
	$(CC) $(SANITIZE) $^ -o $@ $(LDLIBS)

$(FUZZ_BUILD)/bin/%: $(FUZZ_BUILD)/fuzz/%.o $(call objects,$(FUZZ_BUILD),$(LIB_SOURCES))
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(FUZZ) $^ -o $@ $(LDLIBS)

$(FUZZ_BUILD)/bin/fuzz_grammar: $(FUZZ_BUILD)/fuzz/fuzz_grammar.o $(call grammar_objects,$(FUZZ_BUILD))
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(FUZZ) $^ -o $@

$(FUZZ_BUILD)/bin/fuzz_syntax: $(FUZZ_BUILD)/fuzz/fuzz_syntax.o $(sort $(call grammar_objects,$(FUZZ_BUILD)) $(call objects,$(FUZZ_BUILD),$(LIB_SOURCES)))
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(FUZZ) $^ -o $@ $(LDLIBS)

$(FUZZ_BUILD)/fuzz/fuzz_syntax.o: fuzz/fuzz_syntax.c $(GRAMMAR_GENERATED)/mo.tab.h Makefile
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(COMMON) $(FUZZ) $(GRAMMAR_INCLUDES) -c $< -o $@

$(FUZZ_BUILD)/fuzz/fuzz_grammar.o: fuzz/fuzz_grammar.c $(GRAMMAR_GENERATED)/mo.tab.h Makefile
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(COMMON) $(FUZZ) $(GRAMMAR_INCLUDES) -c $< -o $@

# The executable grammar
$(GRAMMAR_GENERATED)/mo.tab.c: grammar/mo.y Makefile
	@mkdir -p $(dir $@)
	bison -Wall -Werror --header=$(GRAMMAR_GENERATED)/mo.tab.h -o $@ $<

$(GRAMMAR_GENERATED)/mo.tab.h: $(GRAMMAR_GENERATED)/mo.tab.c

$(BUILD)/debug/mo-grammar: $(BUILD)/debug/grammar/main.o $(call grammar_objects,$(BUILD)/debug)
	$(CC) $(DEBUG) $^ -o $@

$(BUILD)/sanitize/mo-grammar: $(BUILD)/sanitize/grammar/main.o $(call grammar_objects,$(BUILD)/sanitize)
	$(CC) $(SANITIZE) $^ -o $@

$(BUILD)/debug/grammar/%.o: grammar/%.c $(GRAMMAR_GENERATED)/mo.tab.h Makefile
	@mkdir -p $(dir $@)
	$(CC) $(COMMON) $(DEBUG) $(GRAMMAR_INCLUDES) -c $< -o $@

$(BUILD)/sanitize/grammar/%.o: grammar/%.c $(GRAMMAR_GENERATED)/mo.tab.h Makefile
	@mkdir -p $(dir $@)
	$(CC) $(COMMON) $(SANITIZE) $(GRAMMAR_INCLUDES) -c $< -o $@

$(FUZZ_BUILD)/grammar/%.o: grammar/%.c $(GRAMMAR_GENERATED)/mo.tab.h Makefile
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(COMMON) $(FUZZ) $(GRAMMAR_INCLUDES) -c $< -o $@

$(BUILD)/debug/grammar/mo.tab.o: $(GRAMMAR_GENERATED)/mo.tab.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(GRAMMAR_PARSER_WARNINGS) $(DEBUG) $(GRAMMAR_INCLUDES) -c $< -o $@

$(BUILD)/sanitize/grammar/mo.tab.o: $(GRAMMAR_GENERATED)/mo.tab.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(GRAMMAR_PARSER_WARNINGS) $(SANITIZE) $(GRAMMAR_INCLUDES) -c $< -o $@

$(FUZZ_BUILD)/grammar/mo.tab.o: $(GRAMMAR_GENERATED)/mo.tab.c Makefile
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(GRAMMAR_PARSER_WARNINGS) $(FUZZ) $(GRAMMAR_INCLUDES) -c $< -o $@

# Keep object files between builds, so only changed files recompile.
.SECONDARY:

-include $(shell find build -name '*.d' 2>/dev/null)
