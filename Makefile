PREFIX ?= /usr/local
CFLAGS=-O3 -static

default:
	$(CC) $(CFLAGS) src,vm.c -o rx
	./rx -f src,stdlib -f src,devices -f strip-commentary >bootstrap
	cat bootstrap >>rx
	rm bootstrap

install:
	install -m 755 -d -- $(DESTDIR)$(PREFIX)/bin
	install -c -m 755 rx $(DESTDIR)$(PREFIX)/bin/rx

clean:
	rm -f rx rx.core
