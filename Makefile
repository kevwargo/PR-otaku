CC=mpic++
CFLAGS=-O2 -pthread

all: otaku

otaku: otaku.cpp
	$(CC) $(CFLAGS) otaku.cpp -o otaku

.PHONY: clean
clean:
	rm -f otaku
