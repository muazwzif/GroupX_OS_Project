CC = gcc
CFLAGS = -Wall -Wextra -O3
LDFLAGS = -pthread

all: server client operations generate_data verify page_simulator

server: src/server.c src/common.h
	$(CC) $(CFLAGS) src/server.c -o server $(LDFLAGS)

client: src/client.c src/common.h
	$(CC) $(CFLAGS) src/client.c -o client $(LDFLAGS)

operations: src/operations.c src/common.h
	$(CC) $(CFLAGS) src/operations.c -o operations $(LDFLAGS)

generate_data: src/generate_data.c
	$(CC) $(CFLAGS) src/generate_data.c -o generate_data

verify: src/verify.c
	$(CC) $(CFLAGS) src/verify.c -o verify

page_simulator: src/page_simulator.c
	$(CC) $(CFLAGS) src/page_simulator.c -o page_simulator

run: all
	@echo "=== Cleaning previous execution outputs ==="
	@rm -f reassembled.dat result_min.txt result_max.txt result_sorted.dat execution_log.txt
	@echo "=== Checking/Generating 1GB original.dat dataset ==="
	@if [ ! -f original.dat ]; then ./generate_data original.dat; fi
	@echo "=== Starting Server & Client Loopback ==="
	@./server original.dat & SERVER_PID=$$!; \
	sleep 1.5; \
	./client -p 4 -t 8; \
	wait $$SERVER_PID
	@echo "=== Verifying Reassembly & Analytics Output ==="
	@./verify
	@echo ""
	@echo "=== execution_log.txt ==="
	@cat execution_log.txt
	@echo "========================="

test: all
	chmod +x run_test.sh
	./run_test.sh

clean:
	rm -f server client operations generate_data verify page_simulator original.dat reassembled.dat result_min.txt result_max.txt result_sorted.dat execution_log.txt

.PHONY: all clean test run
