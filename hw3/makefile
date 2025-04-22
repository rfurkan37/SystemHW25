CC = gcc
CFLAGS = -Wall -pthread -g  # -g for debugging symbols
LDFLAGS = -pthread
TARGET = hw3
SRC = main.c # Assuming your C code is saved as main.c

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) *.o # Remove executable and object files (if any)

memcheck: $(TARGET)
	valgrind --leak-check=full --show-leak-kinds=all ./$(TARGET)

.PHONY: clean run memcheck