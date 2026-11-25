# Builds and runs Mo's executable Bison grammar (grammar/mo.y).
#
#   make -C grammar mo-grammar   build ../build/grammar/mo-grammar
#   make -C grammar grammar      build debug + sanitizer mo-grammar and check every corpus
#   make -C grammar clean        remove generated grammar build outputs

CC ?= cc
ROOT := ..
OUT := $(ROOT)/build/grammar
DEFS := $(ROOT)/crates/mo-def/defs

CSTD := -std=c17
INCLUDES := -I. -I$(OUT) -I$(DEFS)
WARNINGS := -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion \
	-Wswitch-enum -Wimplicit-fallthrough -Wstrict-prototypes -Wmissing-prototypes -Wvla \
	-Wformat=2 -Wcast-qual -Wundef -Wnull-dereference
DEBUG := -O0 -g
SANITIZE := -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all
PARSER_FLAGS := $(CSTD) $(INCLUDES) -Wall -Wextra -Werror -Wno-unused-but-set-variable \
	-include $(DEFS)/limits.h '-DYYMAXDEPTH=(4 * MO_LIMIT_FILE_SIZE)'

SOURCES := lexer.c ast.c support.c main.c
DEBUG_OBJS := $(patsubst %.c,$(OUT)/debug/%.o,$(SOURCES)) $(OUT)/debug/mo.tab.o
SAN_OBJS := $(patsubst %.c,$(OUT)/sanitize/%.o,$(SOURCES)) $(OUT)/sanitize/mo.tab.o

ASAN_OPTIONS ?= abort_on_error=1
UBSAN_OPTIONS ?= halt_on_error=1:print_stacktrace=1
export ASAN_OPTIONS UBSAN_OPTIONS

GRAMMAR_CHECK = cd $(ROOT) && \
	$(1) --fragment grammar/tests/good/*.mo && \
	$(1) --fragment --expect-error grammar/tests/bad/*.mo && \
	$(1) --expect-error grammar/tests/bad-files/*.mo && \
	$(1) examples/*.mo && \
	$(1) examples/*/*.mo && \
	$(1) lib/*.mo && \
	$(1) lib/*/*.mo && \
	$(1) tools/*.mo && \
	$(1) tools/*/*.mo && \
	$(1) services/*/*.mo && \
	$(1) docs/reference/README.md && \
	$(1) bench/v0/*/hidden/reference/*.mo bench/v0/*/hidden/broken/*.mo

.PHONY: all mo-grammar grammar clean

all: mo-grammar

mo-grammar: $(OUT)/mo-grammar

$(OUT)/mo.tab.c $(OUT)/mo.tab.h: mo.y
	@mkdir -p $(OUT)
	bison -Wcounterexamples -Werror --defines=$(OUT)/mo.tab.h -o $(OUT)/mo.tab.c mo.y

$(OUT)/debug/mo.tab.o: $(OUT)/mo.tab.c $(OUT)/mo.tab.h ast.h lexer.h support.h $(DEFS)/limits.h
	@mkdir -p $(dir $@)
	$(CC) $(PARSER_FLAGS) $(DEBUG) -c $< -o $@

$(OUT)/sanitize/mo.tab.o: $(OUT)/mo.tab.c $(OUT)/mo.tab.h ast.h lexer.h support.h $(DEFS)/limits.h
	@mkdir -p $(dir $@)
	$(CC) $(PARSER_FLAGS) $(SANITIZE) -c $< -o $@

$(OUT)/debug/%.o: %.c $(OUT)/mo.tab.h ast.h lexer.h support.h $(DEFS)/limits.h
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(INCLUDES) $(WARNINGS) $(DEBUG) -c $< -o $@

$(OUT)/sanitize/%.o: %.c $(OUT)/mo.tab.h ast.h lexer.h support.h $(DEFS)/limits.h
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(INCLUDES) $(WARNINGS) $(SANITIZE) -c $< -o $@

$(OUT)/mo-grammar: $(DEBUG_OBJS)
	$(CC) $(DEBUG) $^ -o $@

$(OUT)/mo-grammar-sanitize: $(SAN_OBJS)
	$(CC) $(SANITIZE) $^ -o $@

grammar: $(OUT)/mo-grammar $(OUT)/mo-grammar-sanitize
	@$(call GRAMMAR_CHECK,build/grammar/mo-grammar)
	@($(call GRAMMAR_CHECK,build/grammar/mo-grammar-sanitize)) >/dev/null

clean:
	rm -rf $(OUT)
