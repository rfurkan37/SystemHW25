CC = gcc
CFLAGS = -Wall -Wextra -pthread -g
LDFLAGS = -pthread

# Common source files for shared library (if we were making one, but here just objects)
SHARED_OBJS = shared/utils.o

# Client specific
CLIENT_SRCS = client/main.c client/commands.c client/network.c
CLIENT_OBJS = $(CLIENT_SRCS:.c=.o) $(SHARED_OBJS)
CLIENT_EXEC = chatclient

# Server specific
SERVER_SRCS = server/main.c server/client_handler.c server/room_manager.c server/file_transfer.c server/logging.c server/utils_server.c
SERVER_OBJS = $(SERVER_SRCS:.c=.o) $(SHARED_OBJS)
SERVER_EXEC = chatserver

.PHONY: all clean client server

all: client server

client: $(CLIENT_EXEC)

server: $(SERVER_EXEC)

$(CLIENT_EXEC): $(CLIENT_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(SERVER_EXEC): $(SERVER_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Rule for compiling .c files in specific directories to .o files in the same directory
client/%.o: client/%.c client/common.h shared/protocol.h
	$(CC) $(CFLAGS) -c $< -o $@

server/%.o: server/%.c server/common.h shared/protocol.h
	$(CC) $(CFLAGS) -c $< -o $@

shared/%.o: shared/%.c shared/utils.h shared/protocol.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(CLIENT_EXEC) $(SERVER_EXEC)
	rm -f client/*.o server/*.o shared/*.o
	rm -f server.log example_log.txt