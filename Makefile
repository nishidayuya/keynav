CFLAGS=`pkg-config --cflags x11 xtst`
LDFLAGS=`pkg-config --libs x11 xtst`

OTHERFILES=README CHANGELIST COPYRIGHT \
           keynavrc Makefile

all: keynav

clean:
	rm *.o || true;
	make -C xdotool clean || true

keynav: xdo.o keynav.o
	gcc $(LDFLAGS) xdo.o keynav.o -o $@

xdo.o:
	make -C xdotool xdo.o
	cp xdotool/xdo.o .

package: clean
	NAME=keynav-`date +%Y%m%d`; \
	mkdir $${NAME}; \
	rsync --exclude '.*' -av *.c $(OTHERFILES) xdotool $${NAME}/; \
	tar -zcf $${NAME}.tar.gz $${NAME}/; \
	rm -rf $${NAME}/

