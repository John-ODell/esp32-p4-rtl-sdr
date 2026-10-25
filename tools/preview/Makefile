# Host build of the receiver screen for design work: make && ./preview out.png
D = ../../components/rpi_display
M = ../../main
CFLAGS = -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Istub -I$(D)/include -I$(D) -I$(M)
SRC = preview.c $(M)/view.c $(M)/bmp.c $(M)/panel.c $(M)/adsb.c $(M)/adsb_view.c $(M)/amap.c $(M)/scan_view.c $(M)/spectrum.c $(D)/rpi_canvas.c $(D)/rpi_text.c

preview: $(SRC) $(M)/view.h $(M)/panel.h $(M)/spectrum.h $(M)/adsb.h $(M)/adsb_view.h $(M)/amap.h $(M)/scan_view.h $(D)/include/rpi_canvas.h
	$(CC) $(CFLAGS) -o $@ $(SRC) -lz -lm
