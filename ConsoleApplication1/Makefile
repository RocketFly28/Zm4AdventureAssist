# ===========================================================================
# Zm4AdventureAssist - native console build (GCC / MinGW)
#
#   mingw32-make              release CLI   -> bin/adventure.exe
#   mingw32-make precal       PRE_CAL CLI   -> bin/adventure_precal.exe
#   mingw32-make check        syntax check only (nothing is linked or run)
#   mingw32-make clean        remove build/
#   mingw32-make distclean    remove build/ and bin/
#
# Layout
#   build/release/*.o   object files + auto generated .d dependency files
#   build/precal/*.o
#   bin/                final executables
#
# Notes
#   * The recipes are POSIX shell syntax, so run make from Git Bash / MSYS2
#     (a plain cmd.exe prompt would need sh.exe on PATH anyway for MinGW).
#   * Release binaries are statically linked by default so the .exe can be
#     handed to someone without the MinGW runtime DLLs. Set STATIC=0 to get
#     a normal dynamically linked binary.
#   * Do NOT run "mingw32-make precal && bin/adventure_precal table" casually:
#     generating the win-rate tables is a multi-minute heavy computation.
# ===========================================================================

# ---------------------------------------------------------------- toolchain
# make ships a built-in default of CC=cc, which does not exist on Windows, so
# replace it only when the user has not chosen a compiler themselves.
ifeq ($(origin CC),default)
  CC := gcc
endif

ifeq ($(OS),Windows_NT)
  EXE := .exe
else
  EXE :=
endif

# ---------------------------------------------------------------- knobs
# Override on the command line, e.g.  mingw32-make OPT=-O3 NATIVE=1
OPT     ?= -O2
NATIVE  ?=
STATIC  ?= 1
OMP     ?= -fopenmp

# ---------------------------------------------------------------- flags
# -Wno-missing-braces: the two pasted win-rate tables are written as flat rows,
# which GCC flags even though the layout is exactly what we want.
WARN := -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-missing-braces
BASE := -std=gnu11 -fcommon -I. $(WARN) $(OPT) $(if $(NATIVE),-march=native,)

REL_CFLAGS  := $(BASE) -DNDEBUG $(OMP)
CAL_CFLAGS  := $(BASE) -DPRE_CAL $(OMP)

LDLIBS := $(OMP) -lm

ifeq ($(STATIC),1)
  LDFLAGS += -static -static-libgcc
endif

ifeq ($(OS),Windows_NT)
  # Reproducible linking: without this the PE header carries a build timestamp,
  # so every rebuid produces different bytes. Since bin/adventure.exe is
  # committed, that would make the tracked binary look modified after any
  # local "make". Dropping the timestamp keeps rebuilds bit-identical.
  LDFLAGS += -Wl,--no-insert-timestamp
endif

# ---------------------------------------------------------------- files
KERNEL := Adventure.c dpCal.c hash.c
FRONT  := cli.c main.c
SRCS   := $(KERNEL) $(FRONT)
HDRS   := common.h Adventure.h dpCal.h hash.h cli.h

BUILD  := build
BINDIR := bin

REL_OBJS := $(SRCS:%.c=$(BUILD)/release/%.o)
CAL_OBJS := $(SRCS:%.c=$(BUILD)/precal/%.o)

REL_BIN := $(BINDIR)/adventure$(EXE)
CAL_BIN := $(BINDIR)/adventure_precal$(EXE)

# ---------------------------------------------------------------- targets
.PHONY: all precal check clean distclean help

all: $(REL_BIN)
precal: $(CAL_BIN)

$(REL_BIN): $(REL_OBJS) | $(BINDIR)
	@echo "  LD    $@"
	@$(CC) $(REL_OBJS) -o $@ $(LDFLAGS) $(LDLIBS)

$(CAL_BIN): $(CAL_OBJS) | $(BINDIR)
	@echo "  LD    $@"
	@$(CC) $(CAL_OBJS) -o $@ $(LDFLAGS) $(LDLIBS)

$(BUILD)/release/%.o: %.c $(HDRS) | $(BUILD)/release
	@echo "  CC    $<"
	@$(CC) $(REL_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/precal/%.o: %.c $(HDRS) | $(BUILD)/precal
	@echo "  CC    $< (PRE_CAL)"
	@$(CC) $(CAL_CFLAGS) -MMD -MP -c $< -o $@

$(BINDIR) $(BUILD)/release $(BUILD)/precal:
	@mkdir -p $@

# Syntax-only pass over every translation unit, in both build configurations.
# Compiles nothing to disk and never executes the solver, so it is safe to run
# while a long precomputation is in progress.
check:
	@set -e; for cfg in "$(BASE) $(OMP)" "$(BASE) -DPRE_CAL $(OMP)"; do \
	  echo "  --- $$cfg"; \
	  for f in $(SRCS); do \
	    echo "  SYN   $$f"; \
	    $(CC) $$cfg -fsyntax-only $$f; \
	  done; \
	done
	@echo "  OK    all translation units parsed"

clean:
	@rm -rf $(BUILD)
	@echo "  clean $(BUILD)/"

distclean: clean
	@rm -rf $(BINDIR)
	@echo "  clean $(BINDIR)/"

help:
	@echo "Zm4AdventureAssist native build"
	@echo "  mingw32-make           release CLI   -> $(REL_BIN)"
	@echo "  mingw32-make precal    PRE_CAL CLI   -> $(CAL_BIN)"
	@echo "  mingw32-make check     syntax check only"
	@echo "  mingw32-make clean     remove $(BUILD)/"
	@echo "  mingw32-make distclean remove $(BUILD)/ and $(BINDIR)/"
	@echo ""
	@echo "  variables: OPT=-O3  NATIVE=1  STATIC=0  CC=clang"

# Auto generated header dependencies (present only after a first compile).
-include $(REL_OBJS:.o=.d) $(CAL_OBJS:.o=.d)
