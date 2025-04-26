CC = gcc
CFLAGS = -Wall -pthread -g
LDFLAGS = -pthread
TARGET = hw3
SRC = main.c

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) *.o

memcheck: $(TARGET)
	valgrind --leak-check=full --show-leak-kinds=all ./$(TARGET)

helgrind: $(TARGET)
	valgrind --tool=helgrind --trace-children=yes ./$(TARGET)

.PHONY: clean run memcheck