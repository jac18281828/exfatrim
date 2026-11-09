.POSIX:

CC = clang
CFLAGS = -std=c17 -pedantic -Wall -Wextra -Werror -D_DARWIN_C_SOURCE -O2
PREFIX = /usr/local
CLANG_FORMAT = clang-format
FORMAT_VERSION = 22.1.8

SRC = src/device.c src/exfat.c src/main.c src/report.c src/trim.c src/verify.c src/volume.c src/walk.c
UNIT_SRC = src/device.c src/exfat.c src/verify.c src/walk.c
HDR = src/device.h src/exfat.h src/report.h src/trim.h src/verify.h src/volume.h src/walk.h
FORMAT_FILES = $(SRC) $(HDR) test/unit.c

all: exfatrim

exfatrim: $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(SRC) -o $@

test/unit: test/unit.c $(UNIT_SRC) src/device.h src/exfat.h src/verify.h src/walk.h
	$(CC) $(CFLAGS) test/unit.c $(UNIT_SRC) -o $@

test: test/unit
	test/unit

integration: all
	test/test.sh

check: format-check test integration
	shellcheck test/test.sh
	python3 -I -m py_compile test/*.py

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 exfatrim $(DESTDIR)$(PREFIX)/bin/exfatrim

format:
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

format-check:
	@$(CLANG_FORMAT) --version | grep -Eq 'version 22\.1\.8( |$$)' || \
		{ echo "clang-format $(FORMAT_VERSION) required" >&2; exit 1; }
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_FILES)

clean:
	rm -rf exfatrim exfatrim.dSYM test/unit test/unit.dSYM

.PHONY: all test integration check install format format-check clean
