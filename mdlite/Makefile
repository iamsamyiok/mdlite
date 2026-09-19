CC      = x86_64-w64-mingw32-gcc
CFLAGS  = -Os -Wall -Wextra -std=c99 -municode -DUNICODE -D_UNICODE
LDFLAGS = -mwindows -Wl,-s
LIBS    = -lgdi32 -lcomdlg32 -lshell32 -lshlwapi -luser32 -ladvapi32 -lwinhttp -lmsimg32 -lole32 -lwindowscodecs

all: MDLite.exe

MDLite.exe: main.c markdown.c markdown.h mdlite.h gitlite.c ai.c editlogic.c \
            editlogic.h export.c zliblite.c zliblite.h tree.c tree.h graph.c graph.h icon.o
	$(CC) $(CFLAGS) main.c markdown.c gitlite.c ai.c editlogic.c export.c \
	      zliblite.c tree.c graph.c backlinks.c icon.o -o $@ $(LDFLAGS) $(LIBS)

icon.o: icon.rc icon.ico
	x86_64-w64-mingw32-windres icon.rc -O coff -o $@

test: test.exe testrender.exe
	DISPLAY= /usr/lib/wine/wine64 test.exe
	DISPLAY= /usr/lib/wine/wine64 testrender.exe

test.exe: test.c markdown.c markdown.h editlogic.c editlogic.h backlinks.c backlinks.h
	$(CC) -O2 -Wall -Wextra -std=c99 test.c markdown.c editlogic.c backlinks.c -o $@ -Wl,-s $(LIBS)

testrender.exe: testrender.c markdown.c markdown.h
	$(CC) -O2 -Wall -Wextra -std=c99 testrender.c markdown.c -o $@ -Wl,-s $(LIBS)

clean:
	rm -f MDLite.exe test.exe testrender.exe
