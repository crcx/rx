default:
	$(CC) $(CFLAGS) rx.c -o rx
	./rx -f retro.forth -f devices.retro -f pack.retro > bootstrap
	cat bootstrap >>rx
	rm bootstrap

clean:
	rm -f rx
