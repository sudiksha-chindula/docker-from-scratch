CC = gcc
CFLAGS = -Wall -Wextra -Wpedantic -Iinclude -g -MMD -MP
LDFLAGS = -lcrypto -larchive -lcjson

SRCS = $(wildcard src/*.c)
OBJS = $(SRCS:.c=.o)
DEPS = $(OBJS:.o=.d)
TARGET = docksmith

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

-include $(DEPS)

clean:
	rm -f src/*.o src/*.d $(TARGET)

.PHONY: all clean
