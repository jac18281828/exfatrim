CC = clang
CFLAGS = -O2 -Wall -Wextra -Werror
PREFIX ?= /usr/local

all: exfatrim

exfatrim: exfatrim.c
	$(CC) $(CFLAGS) exfatrim.c -o $@

test: all
	test/test.sh

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 exfatrim $(DESTDIR)$(PREFIX)/bin/exfatrim

clean:
	rm -rf exfatrim exfatrim.dSYM

.PHONY: all test install clean
