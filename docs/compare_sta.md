# 与参考 STA 工具对比

仓库提供使用同一组网表和约束运行 miniSTA 与参考工具的脚本。脚本提取 setup/hold WNS，输出差值，并将日志写入 `build/`。

| 命令 | 用例 | 参考工具 |
| --- | --- | --- |
| `make compare` | `testcases/sta_compare` | OpenSTA、OpenTimer |
| `make compare-sky130` | `testcases/synth_sky130` | OpenSTA |
| `make compare-real` | `testcases/eth_sky130`、`testcases/e902_sky130` | OpenSTA |

先运行 `make` 构建 miniSTA。`make compare` 会跳过未安装的参考工具；另外两条命令需要 OpenSTA。可通过 `OPENSTA_BIN` 和 `OPENTIMER_BIN` 指定可执行文件路径。例如：

```bash
OPENSTA_BIN=/path/to/sta OPENTIMER_BIN=/path/to/ot-shell make compare
```

`make compare` 和 `make compare-sky130` 默认以 `TOLERANCE_NS=0.05` 比较 WNS；`make compare-real` 默认使用 0.25 ns，对 `e902_sky130` 只输出结果。可在命令前设置 `TOLERANCE_NS` 调整容差。

不同工具的 NLDM 插值、边沿跟踪、时钟传播和 CPPR 处理可能造成数值差异。miniSTA 的模型范围见 [README 的支持范围](../README.md#支持范围)，因此对比结果适合检查用例行为，不能替代对目标设计所需建模能力的核查。
