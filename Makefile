CC = gcc
CFLAGS = -Wall -Wextra -O3
LDFLAGS = -pthread

all: server

server: src/server.c src/common.h
	$(CC) $(CFLAGS) src/server.c -o server $(LDFLAGS)

test: all
	chmod +x run_test.sh
	./run_test.sh

clean:
	rm -f server test.dat

.PHONY: all clean test
