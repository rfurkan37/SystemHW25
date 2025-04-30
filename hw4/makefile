# Compiler and Flags
CC = gcc
# CFLAGS: -Wall (all warnings), -Wextra (more warnings), -g (debug symbols)
#         -pthread (necessary for pthreads compile flags)
#         -std=c11 (or c99, ensures standard compliance)
#         -D_POSIX_C_SOURCE=200809L (ensure getline, strdup are available)
CFLAGS = -Wall -Wextra -g -pthread -std=c11 -D_POSIX_C_SOURCE=200809L

# Linker Flags: -lpthread is crucial for linking the pthreads library
LDFLAGS = -lpthread

# The final executable name
TARGET = LogAnalyzer

# Source files
SOURCES = 210104004042_main.c buffer.c

# Object files: Automatically generate .o filenames from .c filenames
OBJECTS = $(SOURCES:.c=.o)

# Header files (dependencies for object files)
HEADERS = buffer.h

# Default target: Build the executable
all: $(TARGET)

# Rule to link the executable from object files
$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(OBJECTS) -o $(TARGET) $(LDFLAGS)

# Rule to compile .c files into .o files
# $< is the prerequisite (the .c file)
# $@ is the target (the .o file)
%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

# Clean target: Remove compiled files
clean:
	rm -f $(OBJECTS) $(TARGET)

# Phony targets: Targets that don't represent actual files
.PHONY: all clean