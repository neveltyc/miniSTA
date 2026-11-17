# miniSTA

miniSTA（命令名 `msta`）是一个门级静态时序分析器。它读取 Liberty 单元库、门级 Verilog 网表或 Yosys JSON 网表，以及 SDC 约束，生成 setup/hold 路径报告和 WNS/TNS 汇总。

支持组合逻辑、触发器、锁存器、时钟传播、生成时钟、常见路径例外、时钟门控检查和部分设计规则检查。适合学习 STA、检查小型门级设计，以及复现仓库中的时序用例。分析范围和已知限制见[支持范围](#支持范围)。

## 构建与依赖

```bash
make
./build/msta --help
```

| 依赖 | 何时需要 |
| --- | --- |
| 支持 GNU C99 的 C 编译器、`make` | 构建 `msta` |
| `tclsh` | `read_sdc`：SDC 由 `scripts/sdc_bridge.tcl` 在 Tcl 中解释 |
| Yosys | `read_verilog`：运行时调用 `yosys` 把门级 Verilog 转成 JSON。仓库中的用例都使用 `read_verilog`，因此运行用例和 `make test` 也需要它 |
| `python3` | `make test` 和 `make compare` 中的断言脚本 |
| OpenSTA、OpenTimer（可选） | `make compare` 系列对比，需要本地编译，见[参考工具对比](docs/compare_sta.md) |

`tclsh` 和 `yosys` 需在 `PATH` 中。`msta` 按可执行文件的位置查找 `../scripts/sdc_bridge.tcl`，移动 `build/msta` 时要保持这一相对位置。已有 Yosys `write_json` 输出时，可以用 `read_json` 读取网表，运行时无需 Yosys。

## 快速开始

仓库包含可直接运行的 Liberty、网表和约束：

```bash
./build/msta testcases/wrp/wrp.dofile
```

`dofile` 每行一条命令，`#` 开头的行是注释。下面是分析自有设计的基本流程：

```text
read_liberty lib/standard_cells.lib
read_verilog netlist/top.v
current_design top
read_sdc constraints/top.sdc
report_design
report_clocks
report_checks -max_paths 5 -setup
report_checks -max_paths 5 -hold
```

将内容保存为 `design.dofile` 后运行 `./build/msta design.dofile`。文件路径通常相对于 `dofile` 所在目录解析；也可使用绝对路径。若网表已由 Yosys 导出为 JSON，将 `read_verilog netlist/top.v` 换成 `read_json netlist/top.json`。

也可以直接执行命令串，路径相对于当前工作目录：

```bash
./build/msta -c "read_liberty testcases/lib/sky130.lib; read_verilog testcases/sta_compare/netlist.v; current_design simple; read_sdc testcases/sta_compare/common.sdc; report_checks -max_paths 3 -setup"
```

使用 `-q` 减少过程输出，使用 `-o report.txt` 将报告写入文件：

```bash
./build/msta -q -o report.txt testcases/sta_compare/msta.dofile
```

## 输入和报告

| 输入 | 用途 |
| --- | --- |
| Liberty (`.lib`) | 单元、引脚、NLDM 时序表及约束弧；可读取多个库 |
| 门级 Verilog (`.v`) | 通过 Yosys 转换为 JSON 后读取 |
| Yosys JSON (`write_json`) | 直接读取并按 `current_design` 指定的顶层展平 |
| SDC (`.sdc`) | 定义时钟、I/O 延迟、负载和路径约束；支持的命令见 [SDC 参考](docs/sdc.md) |

常用报告命令包括 `report_checks -setup`、`report_checks -hold`（可选 `-tran`、`-cap`、`-clock_path` 展示转换时间、负载电容和启动/捕获时钟路径）、`report_summary`、`report_clocks`、`report_clock_tree`、`report_design`、`report_lib` 和 `print_cell`。面向教学与调试还可直接使用：

- `explain_arc <inst> <in_pin> <out_pin> [-max|-min]`：拆解 NLDM 查表轴向、命中格点、双线性插值权重、OCV/AOCV 缩放系数与最终延迟；
- `report_net <net>`：查看单个网络的逻辑级数、驱动/扇出、总负载电容以及前后向传播后的逐边沿 AT、RAT、Slack 与 Slew；
- `dump_graph [-o file.dot] [-max_nodes N]`：导出 Graphviz DOT 时序有向图（按角色与违例着重标注）；
- `set_wire_model zero_rc|elmore_rc [-r kohm] [-c ff]` 与 `set_path_refiner none|pba_retrace`：切换内置线网 RC / PBA 路径精修示范插件（也可通过 `src/msta_plugin.h` 挂接自定义 C99 算法表）；
- `swap_cell <inst> <new_cell> [-verify]`：支持引脚兼容的在位替换与分级增量 STA 重算（`-verify` 逐网逐端点比对全量结果）。

运行 `./build/msta -c "help"` 可查看完整命令表。路径报告包含到达时间、要求时间和 slack；汇总给出 WNS/TNS、未约束端点及适用的设计规则检查结果。报告时间单位为 ns。

## 支持范围

- 使用 Liberty NLDM 表计算单元延迟和转换时间，支持 setup、hold、recovery、removal 及库中可识别的时钟门控检查。
- 支持时钟树传播、生成时钟、输入和输出延迟、时钟不确定度、路径例外、路径分组、负载与输入驱动等 SDC 子集。具体选项和行为见 [SDC 参考](docs/sdc.md)。
- 支持基础 OCV：独立 early/late、clock/data、rise/fall 与 cell delay/check derate，实例覆盖、max/min 工艺库和 `single` / `bc_wc` / `on_chip_variation` 模式切换。示例见 [OCV 用例](testcases/ocv/README.md)。
- 支持原生深度表 AOCV（保守 GBA）与公共时钟路径 CPPR，使用方法和模型边界见 [AOCV/CPPR](docs/variation.md)。

miniSTA 默认使用集中电容（`zero_rc`）表示负载，不读取 SPEF/SDF；可通过可选的内置 `elmore_rc` 插件或自定义 `MstaWireOps` 估算扇出 RC 延迟与摆率退化，结果适用于前布局估算与教学实验。当前不建模 SI、CCS/ECSM、POCV/LVF、多场景分析、锁存器透明期提前出数及部分 SDC 选项。未支持的约束会在读取时给出警告；请检查日志和未约束端点数量，再使用分析结果。实现上的取舍见[设计说明的模型边界](docs/design.md#5-模型边界)。

## 示例与验证

`testcases/` 包含最小电路、SDC 功能、多库设计和较大的 sky130 门级网表。运行全部用例及其断言：

```bash
make test
```

与 OpenSTA、OpenTimer 对照的方法见[参考工具对比](docs/compare_sta.md)。这些工具只在运行相应对比命令时需要。

## 文档

- [SDC 参考](docs/sdc.md)：支持的命令、选项、解析规则和明确不支持的命令。
- [设计说明](docs/design.md)：模块划分、时序分析步骤、SDC 读入链路、编码约定和模型边界。
- [参考工具对比](docs/compare_sta.md)：`make compare` 系列对比的用例、容差和工具路径。
- 各用例目录下的 `README.md`：用例的手算推导与覆盖点。

## 许可证

miniSTA 使用 [MIT 许可证](LICENSE)。随仓库分发的第三方代码与测试数据见[第三方声明](THIRD_PARTY_NOTICES.md)。
