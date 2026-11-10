# ProcWarden build.
#   all    — release objects + binaries (binaries grow phase by phase)
#   debug  — ASan + UBSan, -O0
#   ubsan  — UBSan only, -O2 (fallback while the ASan runtime is broken on
#            macOS 26.5 — see docs/DEVIATIONS.md DEV-002)
#   tsan   — ThreadSanitizer, -O1
#   test   — builds and runs every tests/*.c with the release objects
#   clean  — removes build/

CC     := clang
CSTD   := -std=c11
WARN   := -Wall -Wextra -Werror
THR    := -pthread
INCS   := -Iinclude

RELFLAGS  := $(CSTD) $(WARN) $(THR) $(INCS) -O2
DBGFLAGS  := $(CSTD) $(WARN) $(THR) $(INCS) -O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer
TSANFLAGS := $(CSTD) $(WARN) $(THR) $(INCS) -O1 -g -fsanitize=thread -fno-omit-frame-pointer

UBSANFLAGS := $(CSTD) $(WARN) $(THR) $(INCS) -O2 -g -fsanitize=undefined -fno-sanitize-recover=undefined

FLAVOR ?= release
ifeq ($(FLAVOR),debug)
  CF := $(DBGFLAGS)
  OD := build/obj-debug
  TD := build/bin-debug
else ifeq ($(FLAVOR),ubsan)
  CF := $(UBSANFLAGS)
  OD := build/obj-ubsan
  TD := build/bin-ubsan
else ifeq ($(FLAVOR),tsan)
  CF := $(TSANFLAGS)
  OD := build/obj-tsan
  TD := build/bin-tsan
else
  CF := $(RELFLAGS)
  OD := build/obj
  TD := build/bin
endif

COMMON_SRC := $(wildcard src/common/*.c)
COMMON_OBJ := $(patsubst %.c,$(OD)/%.o,$(COMMON_SRC))
TEST_BINS  := $(patsubst tests/%.c,$(TD)/test-%,$(wildcard tests/*.c))
SIM_SRC    := src/sim/warden-sim.c src/sim/children.c

# BINS grows in later phases: warden, wardend.
BINS := $(TD)/warden-sim

.PHONY: all debug ubsan tsan test scenario clean compile

all:
	@$(MAKE) --no-print-directory compile

compile: $(COMMON_OBJ) $(TEST_BINS) $(BINS)
	@echo "build ok ($(FLAVOR))"

debug:
	@$(MAKE) --no-print-directory FLAVOR=debug compile

ubsan:
	@$(MAKE) --no-print-directory FLAVOR=ubsan compile

tsan:
	@$(MAKE) --no-print-directory FLAVOR=tsan compile

test:
	@$(MAKE) --no-print-directory compile
	@set -e; for t in $(TEST_BINS); do echo "== $$t"; "$$t"; done
	@echo "ALL TESTS PASSED"

clean:
	rm -rf build

$(OD)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CF) -c $< -o $@

$(TD)/test-%: tests/%.c $(COMMON_OBJ)
	@mkdir -p $(@D)
	$(CC) $(CF) $< $(COMMON_OBJ) -o $@
