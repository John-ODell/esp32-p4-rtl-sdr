# Host test of the ADS-B decoder: make -C tools/adsb_test
CFLAGS = -O2 -Wall -Wextra -I../../main
adsb_test: adsb_test.c ../../main/adsb.c ../../main/adsb.h
	$(CC) $(CFLAGS) -o $@ adsb_test.c ../../main/adsb.c -lm
	./adsb_test
