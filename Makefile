# miniSTA 构建与用例命令。运行时依赖见 README.md。

CC       ?= gcc
CSTD     ?= -std=gnu99
OPT      ?= -O2
WARN     := -Wall -Wextra -Wno-unused-parameter
CFLAGS   += $(CSTD) $(OPT) $(WARN) -Isrc
LDFLAGS  += -lm

SRC_DIR  := src
OBJ_DIR  := build
BIN      := build/msta

SRCS     := $(wildcard $(SRC_DIR)/*.c)
OBJS     := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRCS))
DEPS     := $(wildcard $(SRC_DIR)/*.h) $(wildcard $(SRC_DIR)/*.inc)

all: $(BIN)

$(BIN): $(OBJS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)
	@echo "==> $@"

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c $(DEPS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf build

# 跑 testcases/ 下所有用例 + SDC 语义断言
test: all
	@bash scripts/run_all.sh

# 和本地编译的 OpenSTA/OpenTimer 比 WNS（缺工具就跳过）
compare: all
	@bash scripts/compare_sta.sh

# 在 Yosys 综合出的 sky130 网表上和 OpenSTA 比
compare-sky130: all
	@bash scripts/compare_synth_sky130.sh

# 在 testcases/ 里的真实 sky130 网表上和 OpenSTA 比
compare-real: all
	@bash scripts/compare_real_sky130.sh

# 用 Yosys 从 RTL 重新生成 sky130 门级网表
synth-sky130:
	@bash scripts/synth_sky130.sh

# 前端耗时拆分：Yosys 一步和 msta 自己的 JSON 一步
bench:
	@bash scripts/bench_frontend.sh

.PHONY: all clean test compare compare-sky130 compare-real synth-sky130 bench
