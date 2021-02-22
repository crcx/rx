PREFIX ?= /usr/local
DATADIR ?= $(PREFIX)/share/RETRO12
DOCSDIR ?= $(PREFIX)/share/doc/RETRO12
EXAMPLESDIR ?= $(PREFIX)/share/examples/RETRO12
MANDIR ?= $(PREFIX)/man/man1

default:
	$(CC) $(CFLAGS) rx.c -o rx
	./rx -f retro.forth -f devices.retro -f pack.retro > bootstrap
	cat bootstrap >>rx
	rm bootstrap

install: default
	install -m 755 -d -- $(DESTDIR)$(PREFIX)/bin
	install -c -m 755 rx $(DESTDIR)$(PREFIX)/bin/rx

clean:
	rm -f rx
