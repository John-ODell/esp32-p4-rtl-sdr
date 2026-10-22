# Host test of the audio demodulator: make -C tools/demod_test
CFLAGS = -O2 -Wall -Wextra -I../../main
demod_test: demod_test.c ../../main/demod.c ../../main/demod.h ../../main/dsp.c ../../main/dsp.h ../../main/rds.c ../../main/rds.h
	$(CC) $(CFLAGS) -o $@ demod_test.c ../../main/demod.c ../../main/dsp.c ../../main/rds.c -lm
	./demod_test
