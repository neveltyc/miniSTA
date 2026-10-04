# AOCV 与 CPPR

miniSTA 支持固定系数 OCV、原生深度表 AOCV（保守图分析，GBA），以及公共时钟路径悲观量消除 CPPR。AOCV 在成功读表后开启；CPPR 默认关闭，可通过 `set_cppr` 开启。两者可一起使用。AOCV 仅作用于网表中的单元延迟弧，不缩放 slew、I/O delay、显式理想 latency 或库检查值；库检查仍由 `set_timing_derate -cell_check` 控制。

## 使用

在 dofile 中读表，再读 SDC：

```text
read_liberty cells.lib
read_verilog design.v
current_design top
read_aocv variation.aocv
read_sdc design.sdc
report_aocv
report_checks -setup
report_checks -hold
```

在 SDC 中控制功能（miniSTA 扩展）：

```tcl
set_aocv_analysis 1
set_cppr -mode same_transition 1
```

`set_aocv_analysis 0` 保留表但关闭 AOCV。`set_cppr 0` 关闭 CPPR；模式可选 `same_transition`（默认）或 `same_pin`。开关必须为 0/1。`unset_timing_derate` 只清除固定系数，不清除 AOCV 表。重新 `read_aocv` 成功时替换整套旧表并使旧时序结果失效；读入失败保留旧表、开关和结果。`report_aocv` 列出表及开关，路径报告显示每一级数据弧使用的结构深度。

## AOCV 文件

以下是 **miniSTA 原生文本格式**，不是 PrimeTime/Tempus 私有格式的兼容解析器。每行七列，空行和 `#` 注释允许：

```text
# scope   pattern  path  transition  corner  depth  factor
design    *        data  rise        early   1      0.80
design    *        data  rise        early   2      0.90
design    *        data  rise        early   4      0.95
design    *        data  rise        late    1      1.20
design    *        data  rise        late    2      1.10
design    *        data  rise        late    4      1.05
cell      DATA     data  rise        late    1      1.15
instance  u_data   data  rise        late    1      1.12
```

- scope：`design`、`cell`（Liberty 单元名）、`instance`（展平实例名）。design 的 pattern 必须为 `*`；其余支持 `*`、`?` 等 glob。优先级 instance → cell → design，同优先级后出现的匹配表覆盖。
- path：`clock` 或 `data`；transition：`rise` 或 `fall`，选择延迟弧输入沿，包括 clk-to-Q 的时钟沿；corner：`early` 或 `late`。
- 前五列相同的行组成一张表，可与其他表交错；该表内 depth 必须是递增正整数，范围 1～1000000，每表最多 256 点。
- early 系数须为有限正数、≤1、随深度不减；late 系数须为有限数、≥1、随深度不增。约束确保较短结构上下文的曲线值保守。任意一行错误都会拒绝整个文件。
- depth 中间线性插值，超出范围取最近边界；没有匹配表的维度使用 1.0。
- 最终单元延迟 = NLDM 延迟 × 固定 OCV derate × 工作条件 K 因子 × AOCV 深度系数。

数据与时钟的表、起点/端点集合独立。可以只提供 clock 或 data 表；上面的例子仅覆盖 data/rise，其他维度不会自动复制。

## 深度定义与 GBA 保守性

每个普通网表单元延迟弧计 1 级，clk-to-Q 计 1 级，理想或未建模为零延迟的弧计 0。输入端口为数据起点，触发器输出为新的数据起点；数据端点包括寄存器检查脚、顶层输出和门控使能脚。时钟端点包括寄存器/异步检查/门控时钟脚，生成时钟源作为主时钟的保守边界。

对每根网络预计算最短前向级数 F 和最短后向级数 B。普通弧 In→Out 的表索引为 **F[In]+1+B[Out]**；clk-to-Q 的数据索引为 **1+B[Q]**。这是经过当前弧的完整结构路径上下文，不是当前弧在路径中的位置。当前图不按各条路径例外重新收缩；包含未约束起点/端点可能使深度更短、结果更保守。未知上下文使用 depth=1。

共享弧使用经过它的最短上下文；分叉后的长支路可有自己的较长上下文。由于表满足单调性，用最短上下文不会因遗漏长短路径候选而得到更乐观的系数。生成时钟的深度在新时钟源重新计数；继承主时钟路径保留其已有的深度系数。这是图分析模型，不是针对每条路径重新求完整深度的 PBA，也不实现从 CPPR 分岔点重新计深度的商业工具算法。

不支持布局距离、非整数/单元加权深度、Liberty AOCV/LVF 表、商业私有 AOCV 文件、POCV、外部 driving cell 的深度模型。原理参考 [Synopsys AOCV 白皮书](https://www.synopsys.com/content/dam/synopsys/implementation%26signoff/white-papers/PrimeTime_AdvancedOCV_WP.pdf)；本工具的格式与深度规则以上述定义为准。

## CPPR 算法与边界

在 OCV 中，setup 用 late launch、early capture，hold 相反。时钟共用的一段电路无法同时是两个角，因此应移除这个人为的 min/max 延迟差。每条获胜时钟路径按 corner、本地沿和源沿保存真实前驱；生成时钟前驱接回主时钟路径。

CPPR 从根开始比较实际 launch/capture 路径，只补偿相同物理网络的公共前缀。`same_transition` 要求本地沿相同；`same_pin` 允许异沿，并取两种转换的 min/max 差的较小值。数据起点按独立实例分别传播，保证 CPPR 改变 slack 排名后仍能保留正确的获胜起点。输出 reference pin、recovery/removal、门控检查使用同一补偿入口。

setup 要求时间增加补偿，hold 要求时间减去补偿；到达时间与原始 capture 时间保持原值。路径报告显示 `clock reconvergence pessimism`；WNS/TNS、违例计数和路径选择均使用补偿后的 slack。

- 只在 `on_chip_variation` 模式启用；single/bc_wc、理想时钟、没有物理公共根路径时补偿为零。
- 若某角与其反角的公共前缀不同，停止在最后可确认的共同点；不会用另一条物理路径的摘要到达冒充反角延迟。分岔后重新汇聚也不会仅因网络再次相交就增加补偿。因此在复杂 clock reconvergence 上可能比 OpenSTA 更保守。
- miniSTA 现有 `set_max_delay/set_min_delay` 是相对 launch 的绝对预算，替换 capture/check/skew 关系；此时 CPPR 为零。这个既有语义与 OpenSTA 有差别，`path_budget` 只做本工具手算回归。
- 开启 CPPR 后增加时钟前驱存储并按起点分别传播，运行时间可能增加。当前仍是图分析，不枚举所有时钟路径做 PBA。

CPPR 模式与差值规则参考 [OpenSTA 变量文档](https://opensta.readthedocs.io/en/latest/Variables/)。

## 用例与对照

`make test` 运行仓库用例及断言，覆盖 OCV/AOCV/CPPR 手算、无效深度表和读表失败后的状态保留。`make compare` 使用 OpenSTA/OpenTimer 对照受支持场景；变化深度表的 AOCV 结果按本工具的 GBA 定义进行手算校验。

见 [AOCV 用例](../testcases/aocv/README.md) 和 [CPPR 用例](../testcases/cppr/README.md)。
