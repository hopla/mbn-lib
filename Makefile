CFLAGS  = #-Wall -Wextra -std=c89 -pedantic

all: main

main: main.c src/mbn.a
	gcc ${CFLAGS} main.c -o main -Isrc src/mbn.a -lpthread

src/mbn.a: force_look
	${MAKE} -C src/

clean:
	rm -f main
	${MAKE} -C src/ clean

force_look:

