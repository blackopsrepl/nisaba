CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -g -O2 -Iinclude -Isrc
LDFLAGS ?=

SRC     := $(wildcard src/*.c)
LIBSRC  := $(filter-out src/main.c,$(SRC))
LIBOBJ  := $(LIBSRC:.c=.o)
LIB     := libnisaba.a

.PHONY: all test clean

all: nisaba

$(LIB): $(LIBOBJ)
	$(AR) rcs $@ $^

nisaba: src/main.o $(LIB)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

tests/nisaba_tests: tests/nisaba_tests.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< $(LIB) $(LDFLAGS)

test: tests/nisaba_tests
	./tests/nisaba_tests

clean:
	rm -f src/*.o tests/*.o $(LIB) nisaba tests/nisaba_tests
