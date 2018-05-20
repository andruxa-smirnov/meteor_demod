VERSION=\"0.1a\"

export CFLAGS=-pipe -Wall -std=c99 -pedantic -D_XOPEN_SOURCE=700 -DVERSION=${VERSION}
export LDFLAGS=
PREFIX=/usr

.PHONY: install debug release clean src

default: debug

debug: CFLAGS+= -g -D__DEBUG
debug: src
release: CFLAGS+= -O2 -march=native
release: src strip

src:
	$(MAKE) -C $@

strip:
	$(MAKE) -C src strip

clean:
	$(MAKE) -C src clean