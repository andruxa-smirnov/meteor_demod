export CFLAGS=-pipe -Wall -std=c99 -pedantic -D_XOPEN_SOURCE=700
export LDFLAGS=
PREFIX=/usr

.PHONY: install debug release clean src

default: debug

debug: CFLAGS+= -g
debug: src
release: CFLAGS+= -O2 -march=native
release: strip

src: 
	$(MAKE) -C $@

clean:
	$(MAKE) -C src clean
