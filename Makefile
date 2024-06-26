# CC, AR, RM defined in parent makefile
rm=rm -f

OBJECTS = bkRead7x.o bkAdd.o bkDelete.o bkExtract.o bkRead.o bkPath.o bkMangle.o bkWrite.o bkWrite7x.o bkTime.o bkSort.o bkError.o bkGet.o bkSet.o bkCache.o bkLink.o bkMisc.o bkIoWrappers.o

# -DDEBUG and -g only used during development
CFLAGS  += -Wall -pedantic -std=gnu99 -Wundef -Wcast-align -W -Wpointer-arith -Wwrite-strings -Wno-unused-parameter

# the _FILE_OFFSET_BITS=64 is to enable stat() for large files
CPPFLAGS = -D_FILE_OFFSET_BITS=64

ifdef WINDOWS_BUILD
  CPPFLAGS += -DWINDOWS_BUILD
endif

all:  bk.dll example skeleton addfileiso adddiriso

bk.a: $(OBJECTS)
	@echo 'Creating bk.a'
	@$(AR) -cr bk.a $(OBJECTS)

# static pattern rule
$(OBJECTS): %.o: %.c %.h Makefile bk.h bkInternal.h
	@echo 'Compiling' $<
	@$(CC) $< $(CFLAGS) $(CPPFLAGS) -c -o $@

example: example.c
	$(CC) $(CFLAGS) $(CPPFLAGS) example.c bk.a -o example.exe
        
skeleton: skeleton.c
	$(CC) $(CFLAGS) $(CPPFLAGS) skeleton.c bk.a -o skeleton.exe

addfileiso: addfileiso.c
	$(CC) $(CFLAGS) $(CPPFLAGS) addfileiso.c bk.a -o addfileiso.exe

adddiriso: adddiriso.c
	$(CC) $(CFLAGS) $(CPPFLAGS) adddiriso.c bk.a -o adddiriso.exe

bk.dll: bk.a
	@$(CC) -Zdll bk.a

clean: 
	$(RM) *.o bk.a *.exe bk.dll

