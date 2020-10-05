CROSS_COMPILE ?=
CC = $(CROSS_COMPILE)gcc

all: clexample

%.o : %.c
	$(CC) -c $(CFLAGS) $(CPPFLAGS) $< -o $@

clexample: clexample.o
	$(CC) -L /usr/lib/vivante -o $@ clexample.o -lOpenCL

clean:
	rm -f clexample *.o

.PHONY: all clean
