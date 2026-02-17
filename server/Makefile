CC ?= $(CROSS_COMPILE)gcc
CFLAGS ?= -Wall -Werror -Wextra -O0 -g
LDFLAGS ?=
TARGET ?= aesdsocket
SRC := aesdsocket.c
OBJ := $(SRC:.c=.o)

all default: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJ)
