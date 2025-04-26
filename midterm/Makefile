CC=gcc
CFLAGS=-Wall -Wextra -O2 -pthread
LDFLAGS=-pthread

OBJS_SERVER=bank_server.o teller_runtime.o teller.o
OBJS_CLIENT=bank_client.o

all: bank_server bank_client

bank_server: $(OBJS_SERVER)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

bank_client: $(OBJS_CLIENT)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c common.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f *.o bank_server bank_client
