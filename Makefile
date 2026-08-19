
CC = gcc
TARGET = kvstore
SRCS = kvstore.c reactor.c kvs_array.c kvs_rbtree.c kvs_hash.c
INC = -I ./NtyCo/core
LIBS = -L ./NtyCo/lib  -lntyco -lpthread -ldl


all:
	$(CC) -o $(TARGET) $(SRCS)

clean:
	rm -rf kvstore