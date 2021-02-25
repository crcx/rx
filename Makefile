PREFIX ?= /usr/local
CFLAGS=-O3 -static

default:
	cd sys && $(CC) $(CFLAGS) rx.c -o rx
	cd sys && ./rx -f retro.forth -f devices.retro -f strip-commentary > bootstrap
	cd sys && cat bootstrap >>rx
	cd sys && rm bootstrap

install:
	install -m 755 -d -- $(DESTDIR)$(PREFIX)/bin
	install -c -m 755 sys/rx $(DESTDIR)$(PREFIX)/bin/rx

clean:
	rm -f sys/rx sys/rx.core
