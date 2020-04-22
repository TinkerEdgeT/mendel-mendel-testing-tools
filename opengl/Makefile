CROSS_COMPILE ?=
CC = $(CROSS_COMPILE)gcc

all: glesgears es2gears

%.o : %.c
	$(CC) -c -DGLES=1 $(CFLAGS) $(CPPFLAGS) $< -o $@

gles2_simple-egl.o: simple-egl.c
	$(CC) -c -DGLES=2 $(CFLAGS) $(CPPFLAGS) $< -o $@

glesgears: glesgears.o simple-egl.o
	$(CC) -o glesgears glesgears.o simple-egl.o -lGLESv1_CM -lm -lEGL -lwayland-client -lwayland-egl

es2gears: es2gears.o gles2_simple-egl.o
	$(CC) -o es2gears es2gears.o gles2_simple-egl.o -lGLESv2 -lm -lEGL -lwayland-client -lwayland-egl

clean:
	rm -f glesgears es2gears *.o

.PHONY: all clean
