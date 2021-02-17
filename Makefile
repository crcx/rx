CFLAGS=-O2 -Wall -Wextra -pedantic -s -static

default:
	$(CC) $(CFLAGS) retro.c -o retro
	./retro -f retro.forth -f devices.retro -f pack.retro > bootstrap
	cat bootstrap >>retro
	rm bootstrap

clean:
	rm -f retro
