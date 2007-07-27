# CC, AR, RM defined in parent makefile

OBJECTS = bkRead7x.o bkAdd.o bkDelete.o bkExtract.o bkRead.o bkPath.o bkMangle.o bkWrite.o bkWrite7x.o bkTime.o bkSort.o bkError.o bkGet.o bkSet.o bkCache.o bkLink.o bkMisc.o

# -DDEBUG and -g only used during development
CFLAGS  += -Wall -pedantic -std=gnu99 -Wundef -Wcast-align -W -Wpointer-arith -Wwrite-strings -DDEBUG

# the _FILE_OFFSET_BITS=64 is to enable stat() for large files
CPPFLAGS = -D_FILE_OFFSET_BITS=64

bk.a: $(OBJECTS)
	$(AR) -cr bk.a $(OBJECTS)

# static pattern rule
$(OBJECTS): %.o: %.c Makefile bk.h bkInternal.h
	$(CC) $< $(CFLAGS) $(CPPFLAGS) -c -o $@

example: example.c
	$(CC) $(CFLAGS) $(CPPFLAGS) example.c bk.a -o example

clean: 
	$(RM) *.o bk.a

