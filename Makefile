# tiny-c-agent - config-first agent for the terminal.
#
#   make          build
#   make deps     report build dependencies
#   make run      build and run
#   make clean    remove build/

CC      ?= cc
STD     ?= gnu11
OPT     ?= -O2 -g
WARN    := -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wpointer-arith

BUILD   := build
BIN     := $(BUILD)/tiny

PKGS    := libcurl yyjson tree-sitter \
           tree-sitter-javascript tree-sitter-bash \
           tree-sitter-c tree-sitter-python \
           tree-sitter-markdown tree-sitter-markdown-inline

SRCS    := $(shell find src -name '*.c' | sort)
OBJS    := $(patsubst src/%.c,$(BUILD)/obj/%.o,$(SRCS))
DEPS    := $(OBJS:.o=.d)

# Grammars with no distro package are vendored and built into the binary.
VENDOR_SRCS := $(wildcard vendor/make/src/*.c) \
               $(wildcard vendor/typescript/typescript/src/*.c) \
               $(wildcard vendor/typescript/tsx/src/*.c)
VENDOR_OBJS := $(patsubst vendor/%.c,$(BUILD)/obj/vendor/%.o,$(VENDOR_SRCS))
OBJS        += $(VENDOR_OBJS)
VENDOR_CPPFLAGS := -D_GNU_SOURCE -std=$(STD) $(OPT) -pthread -w -fPIC -MMD -MP \
                   -Ivendor/make/src \
                   -Ivendor/typescript/typescript/src \
                   -Ivendor/typescript/tsx/src

BASE     := -std=$(STD) $(OPT) -pthread

CPPFLAGS += -D_GNU_SOURCE -Isrc -Ivendor/make -Ivendor/typescript -MMD -MP
CFLAGS   += $(BASE) $(WARN) $(shell pkg-config --cflags $(PKGS))
LDLIBS   += $(shell pkg-config --libs $(PKGS)) -pthread

.PHONY: all deps catalog queries run clean
all: $(BIN)

# Regenerates the committed catalog from models.dev. Run at release time, then
# commit src/catalog.generated.c.
catalog:
	python3 tools/gen_catalog.py

# Regenerates the committed highlight queries from vendor/queries. Run after
# editing a .scm file, then commit src/queries.generated.c.
queries:
	python3 tools/gen_queries.py

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD)/obj/vendor/%.o: vendor/%.c
	@mkdir -p $(dir $@)
	$(CC) $(VENDOR_CPPFLAGS) -c -o $@ $<

deps:
	@for p in $(PKGS); do \
	  if pkg-config --exists $$p; then \
	    printf 'ok       %s %s\n' "$$p" "$$(pkg-config --modversion $$p)"; \
	  else \
	    printf 'MISSING  %s\n' "$$p"; \
	  fi; \
	done

run: all
	./$(BIN)

clean:
	rm -rf $(BUILD)

-include $(DEPS)
