# cage - config-first agent for the terminal.
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
BIN     := $(BUILD)/cage

PKGS    := libuv libcurl ncursesw yyjson tree-sitter \
           tree-sitter-javascript tree-sitter-bash \
           tree-sitter-markdown tree-sitter-markdown-inline

TSJSON  := third_party/tree-sitter-json

SRCS    := $(shell find src -name '*.c' | sort)
OBJS    := $(patsubst src/%.c,$(BUILD)/obj/%.o,$(SRCS))
TSJSON_OBJ := $(BUILD)/obj/$(TSJSON)/src/parser.o
DEPS    := $(OBJS:.o=.d) $(TSJSON_OBJ:.o=.d)

BASE     := -std=$(STD) $(OPT) -pthread

CPPFLAGS += -D_GNU_SOURCE -Isrc -I$(TSJSON)/src -MMD -MP
CFLAGS   += $(BASE) $(WARN) $(shell pkg-config --cflags $(PKGS))
LDLIBS   += $(shell pkg-config --libs $(PKGS)) -lqjs -pthread

.PHONY: all deps catalog run clean
all: $(BIN)

# Regenerates the committed catalog from models.dev. Run at release time, then
# commit src/catalog.generated.c.
catalog:
	python3 tools/gen_catalog.py

$(BIN): $(OBJS) $(TSJSON_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

# Vendored grammars are generated code and are built without the project warnings.
$(TSJSON_OBJ): $(TSJSON)/src/parser.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(BASE) -c -o $@ $<

deps:
	@for p in $(PKGS); do \
	  if pkg-config --exists $$p; then \
	    printf 'ok       %s %s\n' "$$p" "$$(pkg-config --modversion $$p)"; \
	  else \
	    printf 'MISSING  %s\n' "$$p"; \
	  fi; \
	done
	@if [ -f /usr/include/quickjs.h ]; then echo 'ok       quickjs'; else echo 'MISSING  quickjs'; fi

run: all
	./$(BIN)

clean:
	rm -rf $(BUILD)

-include $(DEPS)
