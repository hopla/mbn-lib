
include Makefile.inc

.PHONY: all clean distclean lib error conf doc


TARGETS=lib main



all: lib
	${CC} ${CFLAGS} main.c -Isrc -Lsrc -lmbn ${LFLAGS} -o main

main: all
gateway: all

lib: force_look
	${MAKE} -C src/

clean:
	${MAKE} -C src/ clean
	${MAKE} -C doc/ clean
	rm -f main

distclean: clean
	rm Makefile.inc

Makefile.inc: Makefile.inc.in
	@if [ "`uname`" = 'Linux' ]; then\
	  echo "Detected target: linux";\
	  sed 's/PLATFORM=/PLATFORM=linux/' Makefile.inc.in |\
	  sed 's/#IF_ETHERNET=/IF_ETHERNET=if_eth_linux/' |\
	  sed 's/#IF_TCP=/IF_TCP=if_tcp/' > Makefile.inc;\
	else\
	  echo "Detected target: mingw";\
	  sed 's/PLATFORM=/PLATFORM=mingw/' Makefile.inc.in |\
	  sed 's/#IF_ETHERNET=/IF_ETHERNET=if_eth_wpcap/' |\
	  sed 's/#IF_TCP=/IF_TCP=if_tcp/' > Makefile.inc;\
	fi
	@echo "Autogenerated Makefile.inc, you may want to make some modifications"
	@false

conf: Makefile.inc
config: Makefile.inc

doc:
	${MAKE} -C doc/

force_look:

