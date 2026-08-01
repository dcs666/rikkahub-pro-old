# RikkaHub core engine — M0 infrastructure
CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wpedantic -std=c11
CPPFLAGS += -Iinclude -MMD -MP
LDFLAGS += -lpthread -lssl -lcrypto -lz

SRC := $(wildcard src/*/*.c)
OBJ := $(SRC:.c=.o)

TEST_SRC := $(wildcard tests/test_*.c)
TEST_OBJ := $(TEST_SRC:.c=.o)
TEST_BIN := build/test_runner

BENCH_SRC := $(wildcard benchmarks/bench_*.c)
BENCH_BIN := $(patsubst benchmarks/%.c,build/%,$(BENCH_SRC))

.PHONY: all test bench clean check tsan fuzz check-bench cli

all: $(OBJ)

build:
	mkdir -p build

# CFLAGS/CPPFLAGS 签名：命令行传不同编译选项时强制重编译（防 .o 污染）
CFLAGS_FILE := .build-flags
.PHONY: FORCE
$(CFLAGS_FILE): FORCE
	@tmp="$(CFLAGS) $(CPPFLAGS)"; if [ "$$(cat $@ 2>/dev/null)" != "$$tmp" ]; then echo "$$tmp" > $@; fi

$(OBJ): %.o: %.c $(CFLAGS_FILE)
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

# ---- sanitizer 门禁 (ASan/UBSan/LSan) ----
# 用法: make ubsan / make asan / make lsan — 自动 clean + 全测试 + fuzz
# FUZZ_ROUNDS 可调: CI 默认 20000,本地可 FUZZ_ROUNDS=2000 轻量验证
# 注: ASan 在 proot/受限容器内 shadow 映射会失败,本地建议用 ubsan/lsan,asan 留给 CI
SAN_CFLAGS := -O1 -g -Wall -Wextra -Wpedantic -std=c11
FUZZ_ROUNDS ?= 20000

ubsan:
	make clean
	@make test CFLAGS="$(SAN_CFLAGS) -fsanitize=undefined -fno-sanitize-recover=all"
	@make build/fuzz_parsers CFLAGS="$(SAN_CFLAGS) -fsanitize=undefined -fno-sanitize-recover=all"
	./build/fuzz_parsers $(FUZZ_ROUNDS)

asan:
	make clean
	@make test CFLAGS="$(SAN_CFLAGS) -fsanitize=address -fno-sanitize-recover=all" LDFLAGS="$(LDFLAGS) -fsanitize=address"
	@make build/fuzz_parsers CFLAGS="$(SAN_CFLAGS) -fsanitize=address -fno-sanitize-recover=all" LDFLAGS="$(LDFLAGS) -fsanitize=address"
	./build/fuzz_parsers $(FUZZ_ROUNDS)

lsan:
	make clean
	@make test CFLAGS="$(SAN_CFLAGS) -fsanitize=leak" LDFLAGS="$(LDFLAGS) -fsanitize=leak"
	@make build/fuzz_parsers CFLAGS="$(SAN_CFLAGS) -fsanitize=leak" LDFLAGS="$(LDFLAGS) -fsanitize=leak"
	./build/fuzz_parsers $(FUZZ_ROUNDS)

# 严格告警门禁（P5 终审，本地跑；CI 不强制）
strict:
	make clean
	@make test CFLAGS="$(SAN_CFLAGS) -Wshadow -Wformat=2 -Wundef -Wconversion -Wsign-conversion -Wwrite-strings -Wpointer-arith -Wcast-align" 2>&1 | tee /tmp/rikka_strict.log
	@if grep -E ': warning:' /tmp/rikka_strict.log; then echo "!! STRICT WARNINGS PRESENT"; exit 1; fi
	@grep -q "ALL SUITES PASSED" /tmp/rikka_strict.log && echo "STRICT OK"

# CLI 工具
CLI_BIN := build/rikkahub

$(CLI_BIN): build tools/rikkahub.c $(OBJ)
	$(CC) $(CFLAGS) $(CPPFLAGS) tools/rikkahub.c $(OBJ) -o $@ $(LDFLAGS)

cli: $(CLI_BIN)

check-bench:
	@make clean >/dev/null 2>&1
	@make bench 2>&1 | tee /tmp/rikka_bench.log
	@python3 scripts/check_bench.py /tmp/rikka_bench.log

clean:
	rm -rf build $(OBJ) $(TEST_OBJ) $(OBJ:.o=.d) $(TEST_OBJ:.o=.d) $(CFLAGS_FILE)
