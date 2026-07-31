# RikkaHub core engine — M0 infrastructure
CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wpedantic -std=c11
CPPFLAGS += -Iinclude -MMD -MP
LDFLAGS += -lpthread -lssl -lcrypto

SRC := $(wildcard src/*/*.c)
OBJ := $(SRC:.c=.o)

TEST_SRC := $(wildcard tests/test_*.c)
TEST_OBJ := $(TEST_SRC:.c=.o)
TEST_BIN := build/test_runner

BENCH_SRC := $(wildcard benchmarks/bench_*.c)
BENCH_BIN := $(patsubst benchmarks/%.c,build/%,$(BENCH_SRC))

.PHONY: all test bench clean check tsan fuzz check-bench

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

-include $(OBJ:.o=.d) $(TEST_OBJ:.o=.d)

bench: $(BENCH_BIN)
	@for b in $(BENCH_BIN); do echo "== $$b =="; ./$$b; done


# ---- 工程目标 ----

FUZZ_BIN := build/fuzz_parsers

$(FUZZ_BIN): build tests/fuzz_parsers.c $(OBJ)
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/fuzz_parsers.c $(OBJ) -o $@ $(LDFLAGS)

fuzz: $(FUZZ_BIN)
	./$(FUZZ_BIN) 20000

check:
	make clean
	@make test 2>&1 | tee /tmp/rikka_test.log
	@if grep -E ': warning:' /tmp/rikka_test.log; then echo "!! WARNINGS PRESENT"; exit 1; fi
	@grep -q "ALL SUITES PASSED" /tmp/rikka_test.log && echo "CHECK OK (zero warnings, all tests)"

tsan:
	make clean
	@make test CFLAGS="-O1 -g -fsanitize=thread -Wall -Wextra -Wpedantic -std=c11" LDFLAGS="$(LDFLAGS) -fsanitize=thread"

check-bench:
	@make bench 2>&1 | tee /tmp/rikka_bench.log
	@python3 scripts/check_bench.py /tmp/rikka_bench.log

clean:
	rm -rf build $(OBJ) $(TEST_OBJ) $(OBJ:.o=.d) $(TEST_OBJ:.o=.d)
