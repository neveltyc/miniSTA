# 与参考 STA 工具对比

仓库提供脚本，用同一组网表和约束运行 miniSTA 与本地编译的参考工具（OpenSTA、OpenTimer），比较 setup/hold WNS，日志写入 `build/`。参考工具不随仓库分发，只在运行这些命令时需要。

## 命令与用例

| 命令 | 用例 | 参考工具 | 比较内容与容差 |
| --- | --- | --- | --- |
| `make compare` | `testcases/sta_compare` | OpenSTA、OpenTimer | setup/hold WNS，`TOLERANCE_NS`（默认 0.05 ns） |
| | `testcases/check_edges`（17 个） | OpenSTA | 逐个比较 setup/hold WNS，容差 0.001 ns |
| | `testcases/clock_edges`（27 个中的 25 个） | OpenSTA | 逐个比较 setup/hold WNS，容差 0.001 ns；同时核对手算值 |
| | `testcases/nonunate_clock`（21 个中的 20 个） | OpenSTA | 逐个比较 setup/hold WNS，容差 0.001 ns；同时核对手算值和获胜边沿 |
| `make compare-sky130` | `testcases/synth_sky130` | OpenSTA | setup/hold WNS，`TOLERANCE_NS`（默认 0.05 ns） |
| `make compare-real` | `testcases/eth_sky130`、`testcases/e902_sky130` | OpenSTA | `eth_sky130` 断言 setup WNS，`TOLERANCE_NS`（默认 0.25 ns）；`e902_sky130` 只输出结果 |

`make compare` 由 `scripts/compare_sta.sh` 执行：先比较 `sta_compare`，缺少哪个参考工具就跳过哪个；找到 OpenSTA 时，再依次运行 `scripts/compare_check_edges.py`、`scripts/check_clock_edges.py`、`scripts/check_nonunate_clock.py`。后三组的容差写在脚本里，不受 `TOLERANCE_NS` 影响。不参加对比的三个用例：

- `clock_edges` 的 `ideal_edges` 和 `reference_pin` 使用 miniSTA 的显式理想网络和参考引脚模型，只做手算断言。
- `nonunate_clock` 的 `xor_slew` 按时钟源边沿标签分别保存 slew，只做手算断言。

这些用例的手算断言在 `make test` 中也会运行，说明见各用例目录的 README。

## 参考工具路径

先运行 `make` 构建 miniSTA。可用环境变量指定参考工具的可执行文件：

```bash
OPENSTA_BIN=/path/to/sta OPENTIMER_BIN=/path/to/ot-shell make compare
```

不设置时，脚本在 `vendor/` 下查找（`vendor/` 已加入 `.gitignore`，用于存放本地编译的工具）：

- OpenSTA：`vendor/opensta-build/sta` 或 `vendor/build/opensta/sta`。
- OpenTimer：`vendor/opentimer-src/` 或 `vendor/opentimer/` 下的 `bin/ot-shell`。

`make compare-sky130` 和 `make compare-real` 必须有 OpenSTA；`make compare` 在两个工具都缺少时只运行 miniSTA。

## 解读结果

`make compare` 和 `make compare-sky130` 默认以 0.05 ns 比较 WNS，`make compare-real` 默认使用 0.25 ns；可在命令前设置 `TOLERANCE_NS` 调整。

不同工具的 NLDM 插值、边沿跟踪、时钟传播和 CPPR 处理可能造成数值差异。miniSTA 的模型范围见 [README 的支持范围](../README.md#支持范围)和[设计说明的模型边界](design.md#5-模型边界)。对比结果适合检查用例行为，不能替代对目标设计所需建模能力的核查。
