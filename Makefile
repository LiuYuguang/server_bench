CFLAGS := -Wall -O
INC := -I./include

SRC := $(wildcard src/*.c)
OBJ := $(patsubst src/%.c, obj/%.o, $(SRC))

CC     := gcc

TARGET := bench

all: obj $(TARGET)

bench: bench.o server_bench.o
	$(CC) -o $@ $^ -lpthread

$(OBJ): obj/%.o : src/%.c
	$(CC) -c $(CFLAGS) -o $@ $< $(INC)

obj:
	@mkdir -p $@

clean:
	-rm $(OBJ) $(TARGET)
	@rmdir obj

.PHONY: all clean 

vpath %.c  src
vpath %.o  obj
vpath %.h  include
