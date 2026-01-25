# AESD Assignment 2 - finder-app/Makefile

TARGET := writer
SRCS   := writer.c
OBJS   := $(SRCS:.c=.o)

# If CROSS_COMPILE is not set, this becomes just "gcc"
CC := $(CROSS_COMPILE)gcc

# Reasonable warnings; feel free to add -Werror after you’re stable
CFLAGS ?= -Wall -Wextra -O2 -g

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)
