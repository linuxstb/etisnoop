
CC=g++

SOURCES=etisnoop.cpp dabplussnoop.cpp lib_crc.c firecode.c faad_decoder.cpp wavfile.c etiinput.cpp
HEADERS=dabplussnoop.h lib_crc.h firecode.h faad_decoder.h wavfile.h etiinput.h

all: etisnoop

etisnoop: $(SOURCES) $(HEADERS)
	$(CC) -Wall -ggdb $(SOURCES) $(HEADERS) -lfaad -o etisnoop

etisnoop-static: libfaad $(SOURCES) $(HEADERS)
	$(CC) -Wall -ggdb $(SOURCES) $(HEADERS) -Ifaad2-2.7/include faad2-2.7/libfaad/.libs/libfaad.a -o etisnoop

libfaad:
	make -C ./faad2-2.7

.PHONY: tags
tags:
	ctags -R .


clean:
	rm -f etisnoop *.o
