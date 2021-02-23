PREFIX ?= /usr/local

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
