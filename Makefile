CFLAGS+=-D_REENTRANT -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 -U_FORTIFY_SOURCE -Wall -g -ftree-vectorize -pipe

LDFLAGS+=-ldrm -pthread

INCLUDES+=-I/usr/include/libdrm -I./

TELETEXT_OFILES=teletext.o render.o buffer.o hamming.o demo.o

all: teletext

teletext: $(TELETEXT_OFILES)
	$(CC) -o $@ -Wl,--whole-archive $(TELETEXT_OFILES) $(LDFLAGS) -Wl,--no-whole-archive -rdynamic

%.o: %.c
	@rm -f $@ 
	$(CC) $(CFLAGS) $(INCLUDES) -g -c $< -o $@ -Wno-deprecated-declarations

clean:
	rm -f *.o teletext cea608 tvctl
