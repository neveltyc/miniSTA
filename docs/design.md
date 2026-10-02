# 设计说明

本文面向阅读和修改源码的人，讲 miniSTA 的模块划分、一次时序分析的步骤、SDC 的读入链路、编码约定和模型边界。使用方法见 [README](../README.md)，SDC 命令清单见 [SDC 参考](sdc.md)。

## 1. 模块划分与数据流

`msta` 按 dofile 逐条执行命令。读入阶段把三种输入各自转成内存模型，报告命令触发时序分析：

```text
dofile / -c 命令串
  └─ msta_main.c → msta_cmds.c（命令表、全局状态 MstaApp）
       read_liberty   → msta_lib.c（语法树来自 abc_scl_liberty_tree.inc）
       read_verilog   → 调用 yosys 转 JSON → msta_json.c → msta_net.c
       read_json      → msta_json.c → msta_net.c
       current_design → msta_net.c 按顶层展平
       read_sdc       → msta_sdc*.c（经 tclsh 运行 scripts/sdc_bridge.tcl）
       report_*       → msta_timing.c 分析 → msta_report.c 打印
```

| 文件 | 职责 |
| --- | --- |
| `msta_types.h`、`msta_util.{c,h}` | 公共类型、单位约定、动态数组模板；字符串、名字表、整数哈希表、日志 |
| `msta_cmds.{c,h}`、`msta_main.c` | 命令行入口、dofile 切分、命令表 |
| `msta_lib.{c,h}`、`abc_scl_liberty_tree.inc` | Liberty：单元、引脚、时序弧、NLDM 表、时序单元、工作条件；多库合并 |
| `msta_json.{c,h}` | 只读的极简 JSON 解析器（Yosys 网表和 SDC 前端输出共用） |
| `msta_net.{c,h}` | 读 Yosys JSON，按 `current_design` 展平成扁平网表 |
| `msta_sdc.h`、`msta_sdc*.c`、`scripts/sdc_bridge.tcl` | SDC 数据模型、读入与查询，见[第 3 节](#3-sdc-命令的读入) |
| `msta_timing.{c,h}` | 时序引擎，见[第 2 节](#2-时序分析步骤) |
| `msta_report.{c,h}` | 路径报告、汇总、时钟树报告；只排版，不改时序结果 |

全工程共用两条约定（`msta_types.h`）：

- 内部时间和摆率用 ps，电容用 fF。Liberty 和 SDC 在读入时换算，报告输出时再换成 ns。
- 网表、库和约束之间用整数 ID 互相引用，不传裸指针。字符串经全局名字表换成 ID，`MSTA_NO_ID` 表示"没有"。

## 2. 时序分析步骤

流程以 `src/msta_timing.h` 文件头为准；`Msta_TimingAnalyze` 的步骤号与 `msta_timing.c` 的分节编号一致。

| 步骤 | 函数 | 做什么 |
| --- | --- | --- |
| 1 端点 | `BuildChecks` | 时序单元的每条检查（setup/hold、recovery/removal）和每个顶层输出端口各是一个端点；数据脚接常量的检查跳过 |
| 2 时钟树 | `MarkClockNets`、`AttachClockIds` | 从时钟源沿组合弧正向标出时钟网络，按 `timing_sense` 和 case 值求极性，处理 `set_clock_sense -stop_propagation` |
| 3 理想网络 | `MarkIdealNets` | `set_ideal_network` 沿组合扇出铺开，这些网络不累计弧延迟 |
| 4 拓扑序 | `BuildTopoOrder` | 显式栈迭代 DFS（深链不会爆栈）；遇到组合环告警并在该处断开 |
| 5 时钟传播 | `PropagateClocks`、`SeedGeneratedClock` | 按拓扑序求每个时钟在各时钟网络上 [角][本地边沿][源边沿] 的到达和摆率；生成时钟从主时钟接上源延迟 |
| 6 数据到达与检查 | `BuildStartClasses`、`EvaluateCandidatePaths`、`CheckDataChecks`、`CheckClockGating` | 见下文 |
| 7 汇总 | `Msta_TimingAnalyze` 末尾、`CheckDesignRules` | WNS/TNS、未约束端点、设计面积、DRC |

第 5 步排在第 4 步之后执行（要按拓扑序走），但源文件里紧跟第 3 节，因为它和时钟树、理想网络属于同一类逻辑。

第 6 步是引擎的主体：

1. **起点分类**（`BuildStartClasses`）。起点是时序单元输出和带 `set_input_delay` 的输入端口。前向传播在汇聚点只保留最差到达；如果出发沿不同、或命中的 `-from` 例外不同的起点混在一起传，留下的那条可能被例外切掉，或因出发沿不同而并非最差，从而盖住别的候选。因此按"各时钟下的出发相位 + 命中的 `-from` 例外组"给起点分类，同类起点共用一遍传播。
2. **前向传播**（`PropagateData`）。按 (出发时钟, 起点分类, 出发源边沿标签) 各传一遍：max 角用最坏延迟供 setup，min 角用最好延迟供 hold。每根网络按角和边沿记下前驱，报告据此回溯整条路径。没有后向传播，要求时间直接在端点上算。
3. **端点检查**（`EvaluateCandidatePaths` → `CheckEndpoint` → `CheckEndpointOne` → `CheckEndpointEdge`）。捕获网络上有几个时钟就各查一遍；每个数据边沿、每个捕获源边沿分别查，保留最差。输出端口按每个时钟各当一次捕获时钟。
4. **跨时钟边沿对齐**（`FindClockPairEdges`）。出发和捕获时钟周期不同时，在公共周期里找最紧的一对边沿，结果按时钟对缓存，最多搜 1000 拍。
5. **次优路径**（`CheckEndpointEdge`、`AddPathExclusions`）。`-through`、`-rise_through` 这类例外只切掉命中的那条路径，不切整个端点。某个角的最差路径被切掉时，把命中的 (网络, 边沿) 加入排除表，重新传播，换到下一条路径，直到每个角都找到未被切掉的路径或没有候选。
6. **附加检查**。`CheckDataChecks` 处理 `set_data_check`，`CheckClockGating` 处理时钟门控检查，它们的最差 slack 在第 7 步计入 WNS/TNS。

延迟计算：一根弧的延迟和输出摆率由 NLDM 表按 (输入摆率, 输出负载) 双线性插值得到，上升、下降各一张表。max/min 两个角各自保留到达时间与摆率，并按 `set_operating_conditions` 选用各角的库。

## 3. SDC 命令的读入

### 3.1 链路

```text
SDC 文件
  │ tclsh scripts/sdc_bridge.tcl            Tcl 语法、变量、source、集合
  ▼
JSON 命令数组（每条命令是一组字符串，集合打包成标记）
  │ Msta_SdcExpandRecord（msta_sdc.c）      集合标记展开成对象名
  ▼
argv：命令名 + 参数
  │ Msta_SdcRunOne 查分发表 s_vSdcCommands
  │ Msta_SdcParseCmd（msta_sdc_parse.c）    按选项表检查语法，得到 MstaSdcCmd
  ▼
处理函数（msta_sdc_clock.c / _io.c / _except.c / _env.c）
  │ 检查取值、查对象、换算单位，写进 MstaSdc
  ▼
msta_sdc_query.c 的查询接口 → 时序引擎
```

1. **Tcl 桥**。`Msta_SdcReadFile` 起一个 `tclsh` 子进程运行 `scripts/sdc_bridge.tcl`。桥接脚本用 `info complete` 逐条执行命令，单条出错只丢这一条，其余照常生效。约束命令不在 Tcl 里执行，而是由 `msta::emit` 记录下来；`-from`、`-to`、`-through`、`-group` 后面的 Tcl 列表在这一步拆成多个词。
2. **集合标记**。`get_*`、`all_*` 在 Tcl 侧返回一个字符串标记：`\x1e` 开头，后跟一个类型字符和以 `\x1f` 分隔的名字；`-of_objects`、`remove_from_collection` 等选项挂在 `\x1d` 之后，由 C 侧展开。类型字符如下（C 侧在 `Msta_SdcExpandRecord` 里识别），小写表示该集合用了 `-quiet`：

   | 标记 | 对象 |
   | --- | --- |
   | `P` / `G` / `N` / `I` | 端口 / 引脚 / 网络 / 实例 |
   | `C` | 时钟 |
   | `L` / `B` / `Y` | 库 / 库单元 / 库引脚 |
   | `A` | `all_*` 形式，选项交给 C 侧展开 |
   | `Z` | 集合无效（桥接脚本已告警），整条命令丢弃 |

3. **需要设计信息的查询**。Tcl 子进程看不到 C 侧的数据，所以 C 侧在启动 `tclsh` 之前把需要的信息写成临时文件，路径作为参数传给桥接脚本：库索引（`Msta_SdcWriteLibIndex`）供 `get_libs`、`get_lib_cells`、`get_lib_pins` 查询；网表索引（`Msta_SdcWriteDesignIndex`）只在 SDC 文本里出现 `-filter` 时才导出，供 Tcl 用 `expr` 求过滤表达式。其他需要设计信息的查询也走这条通道。
4. **JSON 与展开**。桥接脚本把命令写成 JSON 数组，C 侧用 `msta_json.c` 读回，`Msta_SdcExpandRecord` 把每个标记展开成对象名，并记下每个词来自第几个 Tcl 参数（选项的值要按参数取）。集合为空时整条命令作废。
5. **分发表**。`Msta_SdcRunOne` 在 `s_vSdcCommands` 里按命令名查找。每一行就是一条命令的完整语法：`{ 命令名, 选项表, 位置参数开头的值, 对象个数下限, 上限, 处理函数 }`。查不到时再查 `s_vIgnoredCommands`（手册里有但不建模的命令，告警说明原因）；两张表都没有的报 `sdc：未知命令`。
6. **选项表解析**。`Msta_SdcParseCmd` 按选项表从左到右读 argv，统一检查语法（规则见 [SDC 参考的命令解析规则](sdc.md#命令解析规则)），语法错误时整条作废并计数。选项种类为 `MSTA_SDC_FLAG`、`VALUE`、`OBJECTS`、`LIST`，附加属性 `MSTA_SDC_RF`（也认 `-rise_xxx`/`-fall_xxx`）、`REPEAT`、`IGNORE`（告警后忽略该选项）、`REJECT`（出现即作废整条约束），定义在 `msta_sdc_int.h`。
7. **处理函数**。只做语义：用 `Msta_SdcHasFlag`、`Msta_SdcOptValue`、`Msta_SdcOptList` 取选项，用 `pValue`、`ppObjs` 取位置参数；取值错误调用 `Msta_SdcReject` 作废整条，提醒用 `Msta_SdcNote`。本条命令的临时内存（`Msta_SdcArena`）在读下一条命令时统一释放。

### 3.2 加一条新命令要改哪几处

1. 在对应领域的 `msta_sdc_clock.c`、`msta_sdc_io.c`、`msta_sdc_except.c` 或 `msta_sdc_env.c` 里写选项表（`MstaSdcOpt` 数组，以 `pName == NULL` 结尾）和处理函数；没有选项时用 `Msta_SdcNoOpts`。
2. 在 `msta_sdc_int.h` 对应的分组里声明选项表和处理函数。
3. 在 `msta_sdc.c` 的 `s_vSdcCommands` 加一行，写明位置参数的形状；如果这条命令原本在 `s_vIgnoredCommands` 里，从那里删掉。
4. 检查 `scripts/sdc_bridge.tcl`：Tcl 的 `unknown` 只把 `set_*`、`create_*` 开头的命令转给 C 侧，其他名字（例如 `group_path`）必须加进文件末尾的 `interp alias` 列表，否则会被当成未实现的命令丢掉。选项后面跟的是 Tcl 列表并需要拆开时，把选项名加进 `msta::emit` 的列表。
5. 需要新的约束数据时，扩展 `msta_sdc.h` 里的 `MstaSdc`（初始化和释放在 `Msta_SdcStart`、`Msta_SdcFree`），在 `msta_sdc_query.c` 提供查询接口，再在时序引擎里使用。
6. 在 `testcases/sdc_features/` 加 `.sdc` 和 `.dofile`，在 `scripts/check_sdc.sh` 加断言，并更新 [SDC 参考](sdc.md)。

## 4. 约定

- **只实现 SDC 1.8 手册里的命令和选项**（Synopsys SDC 1.8 手册 Appendix A）。别家工具的写法（如 `-quiet`、`all_inputs -no_clocks`）由兼容层接受，并告警说明它不是 SDC 1.8 语法，不当作标准语法实现。
- **用例只写标准 SDC**。`scripts/check_sdc.sh` 会扫描 `testcases/` 下所有 `.sdc`，出现方言拼写（`-no_clocks`、`-quiet`、`unset_path_exceptions`、`-reset_path`）或 `set_clock_uncertainty -setup 0.3 -hold 0.1` 这种"值跟在开关后面"的写法时失败。
- **正确性优先于功能**。没有建模的命令或选项，宁可告警后忽略或作废整条约束，也不给出一个错误的数。无法用用例核对的实现（例如 K 因子缩放）在告警和文档里写明。
- **代码风格**。每个函数上方用一行注释说明它做什么；注释解释"是什么、怎么算"，不写选型争论。文件头用 `/**CFile**`、`/**CHeader**` 块说明本文件职责。导出函数以 `Msta_<模块>` 开头，类型以 `Msta` 开头；变量前缀 `p` 指针、`n` 个数或下标、`v` 动态数组、`f` 标志、`s_` 文件内静态变量。
- **测试**。每个用例目录里一个或多个 `.dofile`；`make test` 运行全部 dofile，再执行 `scripts/check_sdc.sh`、`check_edges.sh`、`check_clock_edges.py`、`check_nonunate_clock.py` 中的断言。加用例时同步加断言。
- **参考工具只用于对比**。`make compare` 系列用本地编译的 OpenSTA、OpenTimer 运行同一组输入（见[参考工具对比](compare_sta.md)）。OpenSTA 按 GPL-3.0 发布，不复制它的代码和测试。

## 5. 模型边界

面向用户的限制清单见 [README 的支持范围](../README.md#支持范围)。下面补充实现上的取舍，修改相关代码前应先了解：

- **CPPR**。不做时钟再收敛悲观消除：发射和捕获时钟路径的公共部分按各自的角分别计算，结果偏悲观。
- **锁存器**。锁存器 D 脚是普通端点；Q 的出发时间取关闭沿加 clk-to-Q，不按透明期提前出数，所以下游路径偏保守。`set_max_time_borrow v` 只改变锁存器自己 D 检查的要求时间：从默认的"关闭沿减 setup"换成"开沿加 v"。
- **数据检查**。`set_data_check` 只建模 setup 角，`-hold` 告警后不检查；两条路径的出发时钟不同时不检查，并告警。
- **附加检查计入 TNS 的方式**。数据检查和时钟门控检查都只把最差的一条计入 WNS/TNS，不逐条求和。门控检查的 `-rise/-fall/-high/-low` 不改变检查对象，检查总按库里声明的有效沿进行。
- **K 因子**。库里声明 `k_volt`、`k_temp` 时，延迟按 `1 + k_volt × ΔV + k_temp × ΔT` 缩放（process 项不建模）；否则电压和温度只记录、报告。K 因子缩放没有回归用例覆盖。
- **Liberty 时序弧**。未识别的 `timing_type`（例如 `non_seq_setup_rising`）保留为不使用的弧，读库时告警一次，不参与检查。`non_seq_*` 检查是否成立取决于另一个引脚的状态，需要状态分析才能使用。约束表最多二维。
- **默认传播时钟**。SDC 默认时钟是理想的，miniSTA 默认沿时序弧传播时钟；具体规则见 [SDC 参考的时钟与时间](sdc.md#时钟与时间)。
- **跨时钟边沿搜索**。两个时钟的公共周期超过 1000 个出发周期时只搜前 1000 拍，并告警。
