CC = gcc
CFLAGS = -Wall -Wextra -Iinclude -g
LDFLAGS = -lcrypto -larchive -lcjson

SRCS = src/main.c src/cli.c src/isolation.c src/build.c src/state.c src/tar_utils.c
OBJS = $(SRCS:.c=.o)
TARGET = docksmith

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f src/*.o $(TARGET)
