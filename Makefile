CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -g
SRV_SRC = src/server.c src/store.c src/net.c src/flows.c src/customer.c \
          src/employee.c src/manager.c src/admin.c

all: server client

server: $(SRV_SRC) src/bank.h
	$(CC) $(CFLAGS) -pthread -o $@ $(SRV_SRC)

client: src/client.c src/bank.h
	$(CC) $(CFLAGS) -o $@ src/client.c

test_recovery: tests/test_recovery.c src/store.c src/net.c src/bank.h
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_recovery.c src/store.c src/net.c

# runs the recovery test and the concurrency test (needs python3)
test: server test_recovery
	./test_recovery
	python3 tests/test_concurrency.py

# build WITHOUT locks to demonstrate the race conditions (the tests should fail)
nolock: $(SRV_SRC) src/bank.h
	$(CC) $(CFLAGS) -DNO_LOCKS -pthread -o server $(SRV_SRC)

clean:
	rm -f server client test_recovery
	rm -rf data testdata_*

.PHONY: all test clean
