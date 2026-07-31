# RikkaHub core engine — M0 infrastructure
CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wpedantic -std=c11
CPPFLAGS += -Iinclude
LDFLAGS += -lpthread -lssl -lcrypto

SRC := $(wildcard src/*/*.c)
OBJ := $(SRC:.c=.o)

TEST_SRC := $(wildcard tests/test_*.c)
TEST_OBJ := $(TEST_SRC:.c=.o)
TEST_BIN := build/test_runner

BENCH_SRC := $(wildcard benchmarks/bench_*.c)
BENCH_BIN := $(patsubst benchmarks/%.c,build/%,$(BENCH_SRC))

.PHONY: all test bench clean

all: $(OBJ)

build:
	mkdir -p build

$(OBJ): %.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# 测试：所有 *_test.c 编译为独立可执行，主文件链接运行
$(TEST_BIN): build $(TEST_OBJ) $(OBJ)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(filter-out tests/test_main.o,$(TEST_OBJ)) tests/test_main.o $(OBJ) -o $@ $(LDFLAGS)

test: $(TEST_BIN)
	./$(TEST_BIN)

$(BENCH_BIN): build/bench_%: benchmarks/bench_%.c $(OBJ) | build
	$(CC) $(CFLAGS) $(CPPFLAGS) $< $(OBJ) -o $@ $(LDFLAGS)

bench: $(BENCH_BIN)
	@for b in $(BENCH_BIN); do echo "== $$b =="; ./$$b; done

clean:
	rm -rf build $(OBJ) $(TEST_OBJ)
