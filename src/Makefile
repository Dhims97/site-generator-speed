# Build the native sitegen binaries for the v2 entry.
#   make static    — both architectures, into ../bin/ (stripped)
#   make x86_64    — x86_64 only
#   make aarch64   — aarch64 only (needs gcc-aarch64-linux-gnu)
#   make sitegen   — native debug/dev binary in ./sitegen
#   make clean
CC = gcc
CFLAGS = -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter
SRCS = main.c common.c json.c slug.c md.c tpl.c build.c
HDRS = sitegen.h nfkd-table.inc

all: sitegen

sitegen: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

static: x86_64 aarch64

x86_64: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -static -o ../bin/sitegen-x86_64 $(SRCS)
	strip ../bin/sitegen-x86_64

aarch64: $(SRCS) $(HDRS)
	aarch64-linux-gnu-gcc $(CFLAGS) -static -o ../bin/sitegen-aarch64 $(SRCS)
	aarch64-linux-gnu-strip ../bin/sitegen-aarch64

clean:
	rm -f sitegen ../bin/sitegen-x86_64 ../bin/sitegen-aarch64

.PHONY: all static x86_64 aarch64 clean
