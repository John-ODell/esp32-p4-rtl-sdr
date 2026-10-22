# Host test of the audio demodulator: make -C tools/demod_test
CFLAGS = -O2 -Wall -Wextra -I../../main
demod_test: demod_test.c ../../main/demod.c ../../main/demod.h
	$(CC) $(CFLAGS) -o $@ demod_test.c ../../main/demod.c -lm
	./demod_test
