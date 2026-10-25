# Host test of the BMP decoder for SPLASH.BMP: make -C tools/bmp_test
CFLAGS = -O2 -Wall -Wextra -I../../main
bmp_test: bmp_test.c ../../main/bmp.c ../../main/bmp.h
	$(CC) $(CFLAGS) -o $@ bmp_test.c ../../main/bmp.c
	./bmp_test
