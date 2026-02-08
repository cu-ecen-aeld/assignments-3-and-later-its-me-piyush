# AESD Assignment - finder-app/Makefile (Buildroot-friendly)

TARGET := writer
SRCS   := writer.c
OBJS   := $(SRCS:.c=.o)

# Buildroot (and other build systems) will provide CC/CFLAGS via environment.
# Default to gcc only if not set.
CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2 -g

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)
