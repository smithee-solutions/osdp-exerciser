CC = clang
CFLAGS = -c -g -I../include -Wall -Werror
LINK = clang
LDFLAGS = -g

all:	osdp-reader

clean:
	rm -f a.out core *.o osdp-reader

osdp-reader:	osdp-reader.o Makefile
	${LINK} -o osdp-reader osdp-reader.o ../opt/osdp/lib/libosdp-exerciser.a ${LDFLAGS}

osdp-reader.o:	osdp-reader.c
	${CC} ${CFLAGS} osdp-reader.c


