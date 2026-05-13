LIBRT = `test -f /usr/lib/librt.a && printf -- -lrt`

CC = clang
CFLAGS = -g -Wall -Werror

all: reliable

.c.o:
	$(CC) $(CFLAGS) -c $<

rlib.o reliable.o: rlib.h

reliable: reliable.o rlib.o
	$(CC) $(CFLAGS) -o $@ reliable.o rlib.o $(LIBS) $(LIBRT)

.PHONY: clean
clean:
	@find . \( -name '*~' -o -name '*.o' -o -name '*.hi' \) \
		-print0 > .clean~
	@xargs -0 echo rm -f -- < .clean~
	@xargs -0 rm -f -- < .clean~
	rm -f reliable
