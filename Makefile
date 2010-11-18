# Makefile

.PHONY: clean

CFLAGS=-Wall

fsbench: fsbench.o

fsbench.o: fsbench.c

clean:
	$(RM) *.o *~ fsbench
