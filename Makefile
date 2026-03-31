# Makefile for TTC Transit Terminal Departure Board
#
# Targets:
#   all      - Build the departure board binary (default)
#   clean    - Remove compiled output
#   run      - Build and run
#
# Requires: gcc, ncurses development libraries

CC      = gcc
TARGET  = departures
SRCS    = departures.c
CFLAGS  = -Wall -Wextra -std=c11 -O2
LDFLAGS = -lncurses

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) $(LDFLAGS)
	@echo "Build successful: ./$(TARGET)"

run: all
	./$(TARGET)

clean:
	rm -f $(TARGET)
