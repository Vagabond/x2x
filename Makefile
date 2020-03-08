CC=gcc
CFLAGS=-I.
LDFLAGS=-lX11 -lXtst
DEPS = format.h
OBJ = lawyerese.o format.o x2x.o 

%.o: %.c $(DEPS)
		$(CC) -c -fPIE -o $@ $< $(CFLAGS)

x2x: $(OBJ)
		gcc -o $@ $^ $(CFLAGS) $(LDFLAGS)
