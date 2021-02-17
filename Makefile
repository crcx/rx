CFLAGS=-O2 -Wall -Wextra -pedantic

default:
	$(CC) $(CFLAGS) retro.c -o rx
	./rx -f retro.forth -f devices.retro -f pack.retro > bootstrap
	cat bootstrap >>rx
	rm bootstrap

clean:
	rm -f rx
