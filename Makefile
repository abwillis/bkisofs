# CC, AR, RM defined in parent makefile

OBJECTS = bkRead7x.o bkAdd.o bkDelete.o bkExtract.o bkRead.o bkPath.o bkMangle.o bkWrite.o bkWrite7x.o bkTime.o bkSort.o bkError.o bkGet.o bkSet.o bkCache.o bkLink.o bkMisc.o bkIoWrappers.o

# -DDEBUG and -g only used during development
CFLAGS += -Wall -pedantic -std=gnu99 -Wundef -Wcast-align -W -Wpointer-arith -Wwrite-strings -Wno-unused-parameter

# Enable stat() for large files
CPPFLAGS += -D_FILE_OFFSET_BITS=64

ifdef WINDOWS_BUILD
  CPPFLAGS += -DWINDOWS_BUILD
endif

# Select OS/2 explicitly with: make OS2_BUILD=1
ifeq ($(OS2_BUILD),1)
  SHARED_LIBRARY = bk.dll
  EXEEXT = .exe
  PICFLAGS =
else
  SHARED_LIBRARY = libbk.so
  EXEEXT =
  PICFLAGS = -fPIC
endif

PROGRAMS = example$(EXEEXT) skeleton$(EXEEXT) addfileiso$(EXEEXT) adddiriso$(EXEEXT)

.PHONY: all clean

all: $(SHARED_LIBRARY) $(PROGRAMS)

bk.a: $(OBJECTS)
	@echo 'Creating bk.a'
	@$(AR) -cr bk.a $(OBJECTS)

# Static pattern rule
$(OBJECTS): %.o: %.c %.h Makefile bk.h bkInternal.h
	@echo 'Compiling' $<
	@$(CC) $(CPPFLAGS) $(CFLAGS) $(PICFLAGS) -c $< -o $@

example$(EXEEXT): example.c bk.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) example.c bk.a $(LDLIBS) -o $@

skeleton$(EXEEXT): skeleton.c bk.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) skeleton.c bk.a $(LDLIBS) -o $@

addfileiso$(EXEEXT): addfileiso.c bk.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) addfileiso.c bk.a $(LDLIBS) -o $@

adddiriso$(EXEEXT): adddiriso.c bk.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) adddiriso.c bk.a $(LDLIBS) -o $@

ifeq ($(OS2_BUILD),1)
bk.dll: bk.a
	@$(CC) -Zdll bk.a
else
libbk.so: $(OBJECTS)
	$(CC) $(CFLAGS) $(PICFLAGS) $(LDFLAGS) -shared -o $@ $(OBJECTS) $(LDLIBS)
endif

clean:
	$(RM) $(OBJECTS) bk.a bk.dll libbk.so example example.exe skeleton skeleton.exe addfileiso addfileiso.exe adddiriso adddiriso.exe
