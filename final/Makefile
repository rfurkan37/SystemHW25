CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -pthread -D_GNU_SOURCE
LDFLAGS = -pthread

SERVER_SRC = server/main.c server/client_handler.c server/room_manager.c \
             server/file_transfer.c server/logging.c server/utils.c
CLIENT_SRC = client/main.c client/network.c client/commands.c client/utils.c

SERVER_OBJ = $(SERVER_SRC:.c=.o)
CLIENT_OBJ = $(CLIENT_SRC:.c=.o)

all: chatserver chatclient

chatserver: $(SERVER_OBJ)
	$(CC) $(SERVER_OBJ) -o chatserver $(LDFLAGS)

chatclient: $(CLIENT_OBJ)
	$(CC) $(CLIENT_OBJ) -o chatclient $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(SERVER_OBJ) $(CLIENT_OBJ) chatserver chatclient server.log

install: all
	cp chatserver chatclient /usr/local/bin/

test: all
	./test_runner.sh

.PHONY: all clean install test 