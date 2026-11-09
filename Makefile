.POSIX:

CC = clang
CFLAGS = -std=c17 -pedantic -Wall -Wextra -Werror -D_DARWIN_C_SOURCE -O2
PREFIX = /usr/local

SRC = src/device.c src/exfat.c src/main.c src/report.c src/trim.c src/verify.c src/volume.c src/walk.c
HDR = src/device.h src/exfat.h src/main.h src/report.h src/trim.h src/verify.h src/volume.h src/walk.h

all: exfatrim

exfatrim: $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(SRC) -o $@

test: all
	test/test.sh

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 exfatrim $(DESTDIR)$(PREFIX)/bin/exfatrim

clean:
	rm -rf exfatrim exfatrim.dSYM

.PHONY: all test install clean
