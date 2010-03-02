CFLAGS+= $(shell pkg-config --cflags xinerama glib-2.0 2> /dev/null || echo -I/usr/X11R6/include -I/usr/local/include)
LDFLAGS+= $(shell pkg-config --libs xinerama glib-2.0 2> /dev/null || echo -L/usr/X11R6/lib -L/usr/local/lib -lX11 -lXtst -lXinerama -lXext -lglib)

#CFLAGS+=-g
OTHERFILES=README CHANGELIST COPYRIGHT \
           keynavrc Makefile

MICROVERSION?=00

.PHONY: all

all: keynav

clean:
	rm *.o || true;
	$(MAKE) -C xdotool clean || true

# We'll try to detect 'libxdo' and use it if we find it.
# otherwise, build monolithic.
keynav: keynav.o
	@set -x; \
	if $(LD) -lxdo > /dev/null 2>&1 ; then \
		$(CC) keynav.o -o $@ $(LDFLAGS) -lxdo; \
	else \
		$(MAKE) keynav.static; \
	fi

.PHONY: keynav.static
keynav.static: keynav.o xdo.o
	$(CC) xdo.o keynav.o -o keynav `pkg-config --libs xext xtst` $(LDFLAGS)

xdo.o:
	$(MAKE) -C xdotool xdo.o
	cp xdotool/xdo.o .

package: clean
	NAME=keynav-`date +%Y%m%d`.$(MICROVERSION); \
	mkdir $${NAME}; \
	rsync --exclude '.*' -av *.c $(OTHERFILES) xdotool $${NAME}/; \
	tar -zcf $${NAME}.tar.gz $${NAME}/; \
	rm -rf $${NAME}/

