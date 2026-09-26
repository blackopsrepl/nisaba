# Nisaba Makefile
# C11 build system with colorized output, strict lint, and install targets.

# ============== Colors & Symbols ==============
GREEN   := \033[92m
EMERALD := \033[38;2;16;185;129m
CYAN    := \033[96m
YELLOW  := \033[93m
RED     := \033[91m
GRAY    := \033[90m
BOLD    := \033[1m
RESET   := \033[0m

CHECK    := ✓
CROSS    := ✗
ARROW    := ▸
PROGRESS := →

# ============== Project Metadata ==============
VERSION := $(shell cat VERSION 2>/dev/null || echo 0.0.0-dev)

# ============== Toolchain ==============
CC      ?= cc
AR      ?= ar
LDFLAGS ?=
CFLAGS  ?=

BASE_CFLAGS ?= -std=c11 -Wall -Wextra -Werror -Iinclude -Isrc
OPTFLAGS    ?= -O2 -g
DEPFLAGS     = -MMD -MP

# Strict gate: the shipped warning set must stay clean (see `make lint`).
LINT_CFLAGS ?= -Wpedantic -Wshadow -Wstrict-prototypes -Wold-style-definition \
               -Wmissing-prototypes -Wcast-align -Wwrite-strings -Wundef
# Style-opinionated bugprone checks are excluded on purpose: adjacent size_t
# parameters and assignment-in-if (the varint decode idiom) are house style.
TIDY_CHECKS  = -*,clang-analyzer-*,-clang-analyzer-security.insecureAPI.*,\
bugprone-*,-bugprone-easily-swappable-parameters,-bugprone-assignment-in-if-condition,\
-bugprone-reserved-identifier,-bugprone-implicit-widening-of-multiplication-result,\
-bugprone-narrowing-conversions

# ============== Layout ==============
SRC     := $(wildcard src/*.c)
LIBSRC  := $(filter-out src/main.c,$(SRC))
LIBOBJ  := $(LIBSRC:.c=.o)
LIB     := libnisaba.a
BIN     := nisaba
TESTBIN := tests/nisaba_tests
DEPS    := $(LIBOBJ:.o=.d) src/main.d

PREFIX ?= $(HOME)/.local
BINDIR  = $(PREFIX)/bin
LIBDIR  = $(PREFIX)/lib
INCDIR  = $(PREFIX)/include

# ============== Phony Targets ==============
.PHONY: banner help build test asan valgrind lint ci-local install uninstall \
        version clean

.DEFAULT_GOAL := build

# ============== Banner ==============
banner:
	@printf "$(EMERALD)$(BOLD)"
	@printf " _   _ ___ ____    _    ____    _    \n"
	@printf "| \\ | |_ _/ ___|  / \\  | __ )  / \\   \n"
	@printf "|  \\| || |\\___ \\ / _ \\ |  _ \\ / _ \\  \n"
	@printf "| |\\  || | ___) / ___ \\| |_) / ___ \\ \n"
	@printf "|_| \\_|___|____/_/   \\_\\____/_/   \\_\\\n"
	@printf "$(RESET)"
	@printf "  $(GRAY)v$(VERSION)$(RESET) $(EMERALD)The tablet remembers.$(RESET)\n\n"

# ============== Build ==============

build: banner $(BIN)
	@printf "$(EMERALD)$(PROGRESS) lib + CLI up to date$(RESET)\n\n"

$(LIB): $(LIBOBJ)
	@printf "$(CYAN)$(ARROW)$(RESET) archiving $(BOLD)$@$(RESET)\n"
	@$(AR) rcs $@ $^

$(BIN): src/main.o $(LIB)
	@printf "$(CYAN)$(ARROW)$(RESET) linking $(BOLD)$@$(RESET)\n"
	@$(CC) $(BASE_CFLAGS) $(OPTFLAGS) $(CFLAGS) -o $@ src/main.o $(LIB) $(LDFLAGS)

%.o: %.c
	@printf "$(CYAN)$(ARROW)$(RESET) compiling $(BOLD)$<$(RESET)\n"
	@$(CC) $(BASE_CFLAGS) $(OPTFLAGS) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

# ============== Test ==============

test: banner $(TESTBIN)
	@printf "$(CYAN)$(BOLD)╔══════════════════════════════╗$(RESET)\n"
	@printf "$(CYAN)$(BOLD)║         Test Suite           ║$(RESET)\n"
	@printf "$(CYAN)$(BOLD)╚══════════════════════════════╝$(RESET)\n\n"
	@./$(TESTBIN) && \
		printf "$(GREEN)$(CHECK) All tests passed$(RESET)\n\n" || \
		(printf "$(RED)$(CROSS) Tests failed$(RESET)\n\n" && exit 1)

$(TESTBIN): tests/nisaba_tests.c $(LIB)
	@printf "$(CYAN)$(ARROW)$(RESET) linking $(BOLD)$@$(RESET)\n"
	@$(CC) $(BASE_CFLAGS) $(OPTFLAGS) $(CFLAGS) -o $@ tests/nisaba_tests.c $(LIB) $(LDFLAGS)

# ============== Sanitizers & Valgrind ==============

asan: banner
	@printf "$(CYAN)$(ARROW)$(RESET) building + running tests under ASan + UBSan...\n"
	@mkdir -p build/asan
	@$(CC) $(BASE_CFLAGS) -O1 -g -fsanitize=address,undefined \
		-o build/asan/nisaba_tests tests/nisaba_tests.c $(LIBSRC) $(LDFLAGS)
	@./build/asan/nisaba_tests && \
		printf "$(GREEN)$(CHECK) Sanitizers clean$(RESET)\n\n" || \
		(printf "$(RED)$(CROSS) Sanitizer findings$(RESET)\n\n" && exit 1)

valgrind: banner $(TESTBIN)
	@printf "$(CYAN)$(ARROW)$(RESET) running tests under valgrind...\n"
	@valgrind --leak-check=full --error-exitcode=99 ./$(TESTBIN) >/dev/null && \
		printf "$(GREEN)$(CHECK) Valgrind clean$(RESET)\n\n" || \
		(printf "$(RED)$(CROSS) Valgrind findings$(RESET)\n\n" && exit 1)

# ============== Lint ==============

lint: banner
	@printf "$(CYAN)$(BOLD)╔══════════════════════════════╗$(RESET)\n"
	@printf "$(CYAN)$(BOLD)║         Lint Gate            ║$(RESET)\n"
	@printf "$(CYAN)$(BOLD)╚══════════════════════════════╝$(RESET)\n\n"
	@printf "$(PROGRESS) Strict compile ($(LINT_CFLAGS))...\n"
	@for f in $(SRC) tests/nisaba_tests.c; do \
		$(CC) $(BASE_CFLAGS) $(OPTFLAGS) $(LINT_CFLAGS) -fsyntax-only $$f || exit 1; \
	done
	@printf "$(GREEN)$(CHECK) Strict compile clean$(RESET)\n"
	@if ! command -v clang-tidy >/dev/null 2>&1; then \
		printf "$(RED)$(CROSS) clang-tidy is required for lint and was not found$(RESET)\n"; \
		exit 1; \
	fi
	@printf "$(PROGRESS) clang-tidy (clang-analyzer + bugprone)...\n"
	@clang-tidy --quiet -warnings-as-errors='*' -checks='$(TIDY_CHECKS)' \
		$(SRC) tests/nisaba_tests.c -- -std=c11 -Iinclude -Isrc
	@printf "$(GREEN)$(CHECK) clang-tidy clean$(RESET)\n\n"

# ============== Local CI ==============

ci-local: banner
	@printf "$(CYAN)$(BOLD)╔══════════════════════════════════════════╗$(RESET)\n"
	@printf "$(CYAN)$(BOLD)║           Local CI Simulation            ║$(RESET)\n"
	@printf "$(CYAN)$(BOLD)╚══════════════════════════════════════════╝$(RESET)\n\n"
	@$(MAKE) lint --no-print-directory
	@printf "$(PROGRESS) Step 2/4: Build...\n"
	@$(MAKE) $(BIN) --no-print-directory
	@printf "$(GREEN)$(CHECK) Build passed$(RESET)\n"
	@printf "$(PROGRESS) Step 3/4: Tests...\n"
	@$(MAKE) test --no-print-directory
	@printf "$(PROGRESS) Step 4/4: Sanitizers...\n"
	@$(MAKE) asan --no-print-directory
	@printf "\n$(GREEN)$(BOLD)$(CHECK) CI SIMULATION PASSED$(RESET)\n\n"

# ============== Install ==============

install: banner $(BIN) $(LIB)
	@printf "$(CYAN)$(ARROW)$(RESET) installing to $(BOLD)$(PREFIX)$(RESET)\n"
	@install -d $(BINDIR) $(LIBDIR) $(INCDIR)
	@install -m 755 $(BIN) $(BINDIR)/$(BIN)
	@install -m 644 $(LIB) $(LIBDIR)/$(LIB)
	@install -m 644 include/nisaba.h $(INCDIR)/nisaba.h
	@printf "$(GREEN)$(CHECK) $(BINDIR)/$(BIN)$(RESET)\n"
	@printf "$(GREEN)$(CHECK) $(LIBDIR)/$(LIB)$(RESET)\n"
	@printf "$(GREEN)$(CHECK) $(INCDIR)/nisaba.h$(RESET)\n\n"

uninstall: banner
	@printf "$(CYAN)$(ARROW)$(RESET) removing from $(BOLD)$(PREFIX)$(RESET)\n"
	@rm -f $(BINDIR)/$(BIN) $(LIBDIR)/$(LIB) $(INCDIR)/nisaba.h
	@printf "$(GREEN)$(CHECK) uninstall complete$(RESET)\n\n"

# ============== Version & Clean ==============

version:
	@printf "$(CYAN)Current version:$(RESET) $(YELLOW)$(BOLD)$(VERSION)$(RESET)\n"

clean:
	@printf "$(ARROW) Cleaning build artifacts...\n"
	@rm -f $(LIBOBJ) src/main.o src/main.d $(DEPS) $(LIB) $(BIN) $(TESTBIN) tests/*.d
	@rm -rf build
	@printf "$(GREEN)$(CHECK) Clean complete$(RESET)\n"

# ============== Help ==============

help: banner
	@printf "$(CYAN)$(BOLD)Build Commands:$(RESET)\n"
	@printf "  $(GREEN)make build$(RESET)        - Build libnisaba.a and the nisaba CLI (default)\n"
	@printf "  $(GREEN)make install$(RESET)      - Install bin, lib, and header to $(BOLD)$(PREFIX)$(RESET)\n"
	@printf "  $(GREEN)make uninstall$(RESET)    - Remove installed bin, lib, and header\n"
	@printf "  $(GREEN)make clean$(RESET)        - Remove all build artifacts\n"
	@printf "  $(GREEN)make version$(RESET)      - Show the version from VERSION\n"
	@printf "\n"
	@printf "$(CYAN)$(BOLD)Test Commands:$(RESET)\n"
	@printf "  $(GREEN)make test$(RESET)         - Build and run the test suite\n"
	@printf "  $(GREEN)make asan$(RESET)         - Run tests under ASan + UBSan\n"
	@printf "  $(GREEN)make valgrind$(RESET)     - Run tests under valgrind (leak check)\n"
	@printf "\n"
	@printf "$(CYAN)$(BOLD)Quality:$(RESET)\n"
	@printf "  $(GREEN)make lint$(RESET)         - Strict compile + clang-tidy (hard gate)\n"
	@printf "  $(GREEN)make ci-local$(RESET)     - lint + build + test + asan\n"
	@printf "\n"
	@printf "$(GRAY)Install prefix override: make install PREFIX=/usr/local$(RESET)\n"
	@printf "\n"

-include $(DEPS)
