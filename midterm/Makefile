CC=gcc
CFLAGS=-Wall -Wextra -O2 -pthread
# Add -lrt for shm_open/shm_unlink
LDFLAGS=-pthread -lrt

# Separate objects for server main, teller runtime, and teller logic
OBJS_SERVER=bank_server.o teller_runtime.o teller.o
OBJS_CLIENT=bank_client.o

# Default target
all: bank_server bank_client

# Link the server executable
bank_server: $(OBJS_SERVER)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Link the client executable
bank_client: $(OBJS_CLIENT)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Compile source files into object files
# The dependency on common.h ensures recompilation if the header changes
%.o: %.c common.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Clean up object files and executables
clean:
	rm -f *.o bank_server bank_client AdaBank.bankLog /tmp/adabank_fifo /tmp/bank_*_req /tmp/bank_*_res
	# Attempt to clean up shared memory segment if it exists
	rm -f /dev/shm/adabank_shm || true

# Phony target for clean
.PHONY: all clean