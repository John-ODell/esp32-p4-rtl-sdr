# Host test of the tuner AGC: make -C tools/agc_test
CFLAGS = -O2 -Wall -Wextra -I../../main
agc_test: agc_test.c ../../main/agc.c ../../main/agc.h
	$(CC) $(CFLAGS) -o $@ agc_test.c ../../main/agc.c -lm
	./agc_test
