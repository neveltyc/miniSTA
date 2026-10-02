# SDC 支持参考

miniSTA 使用 `tclsh` 读取 SDC 文件。可使用 Tcl 变量、`expr`、`source`、条件语句和续行。SDC 的时间与电容单位默认取自首先读取的 Liberty 库，`set_units` 可以更改约束文件中的单位。报告以 ns 显示时间。

本页列出支持的常用命令及关键行为。miniSTA 实现的是 SDC 子集；对于未建模的命令或选项，读取时会给出警告。使用外部约束文件时应检查警告和未约束端点数量。

## 对象集合

| 命令 | 支持范围 |
| --- | --- |
| `get_ports`、`get_pins`、`get_nets`、`get_cells`、`get_clocks` | 名称、Tcl 列表、`*`/`?` 匹配与 `-of_objects` |
| `all_inputs`、`all_outputs`、`all_clocks` | 获取对应对象；输入和输出可按时钟筛选 |
| `all_registers` | 可选 `-cells`、`-data_pins`、`-clock_pins`、`-async_pins`、`-output_pins`，以及 `-clock`、`-rise_clock`、`-fall_clock` 筛选 |
| `get_libs`、`get_lib_cells`、`get_lib_pins` | 库对象查询；支持名称匹配，部分命令支持 `-regexp`、`-nocase` 和 `-of_objects` |
| `current_design`、`current_instance`、`set_hierarchy_separator` | 设置查询范围与层次名称 |
| `remove_from_collection` | 从集合中排除名称或模式 |

`get_* -of_objects` 按对象关系选择，例如：

```tcl
get_pins -of_objects [get_cells u1]
get_nets -of_objects [get_pins u1/Y]
get_cells -of_objects [get_nets data_net]
```

`get_ports`、`get_pins`、`get_nets` 和 `get_cells` 支持 `-filter`，可按 `name`、`full_name`、`ref_name`、`direction`、`is_clock` 或 `fanout` 等适用属性筛选。表达式使用 Tcl `expr` 运算规则，`=~` 和 `!~` 用于通配匹配：

```tcl
get_cells -filter {ref_name =~ DFF*}
get_pins {u1/*} -filter {direction == in}
get_nets -filter {fanout > 1}
```

可用属性取决于对象类型；时钟的 `is_virtual` 等属性不能通过该过滤器查询。`-filter` 与 `-of_objects` 同时使用，以及 `-expression`、`-level` 等未支持选项会告警并跳过相关约束。

## 命令解析规则

每条命令按同一套规则解析；写错的约束整条作废（告警，并计入读入结束时报告的忽略条数），其余约束照常生效。

- 只有以 `-` 开头、后面跟字母的词才是选项。`-5`、`-0.1` 这样的负数永远是值，可以写在选项之间。
- 不认识的选项、带值的选项缺值、同一个选项写两次，都会使整条约束作废。`-from` 与 `-rise_from` 算同一个选项。手册里有但未建模的选项各有说明：有的告警后忽略，有的使整条约束作废。
- 位置参数先写值，再写对象。值的个数是固定的：多出来的数值（例如 `-max 2 -min 1` 这种为两个角分别给值的写法）、对象太多或缺少对象，都会使整条约束作废。
- 数值要在合理范围内：周期数必须是正整数，负载、转换时间、面积不能为负，周期、电压、`-multiply_by`、`group_path -weight` 等必须为正。`set_case_analysis` 只接受 `0`、`1`、`zero`、`one`。
- 给了对象集合、但集合为空（例如 `get_clocks` 的模式没有匹配，或 `-of_objects` 没有找到对象）时，整条约束作废，不会当作"没写对象"处理。对象列表中找不到的对象逐个告警并跳过，一个都找不到时整条作废。
- 同时写 `-rise` 和 `-fall`（或 `-min` 和 `-max`）等于两个都选。

## 时钟与时间

| 命令 | 主要选项或用途 |
| --- | --- |
| `create_clock` | `-period`、`-name`、`-waveform`、`-add` |
| `create_generated_clock` | `-divide_by`、`-multiply_by`、`-invert`、`-duty_cycle`、`-edges`、`-edge_shift` |
| `set_clock_latency`、`set_clock_transition` | 时钟延迟和源转换时间；支持 max/min 与 rise/fall 限定 |
| `set_propagated_clock` | 标记传播时钟 |
| `set_clock_sense` | `-positive`、`-negative`、`-stop_propagation`、`-clock` |
| `set_ideal_network`、`set_ideal_latency`、`set_ideal_transition` | 理想网络及其延迟、转换时间 |
| `set_clock_groups` | 异步、逻辑互斥、物理互斥时钟组 |
| `set_clock_uncertainty` | setup/hold 不确定度，可指定时钟对 |
| `set_timing_derate` | early/late、clock/data、cell delay/cell check 及分对象系数 |
| `set_clock_gating_check`、`set_max_time_borrow` | 时钟门控检查与锁存器借时限制 |
| `set_units` | 时间和电容单位 |

`create_generated_clock -edges {e1 e2 e3}` 的边号从 1 开始：1 是首个上升沿，2 是首个下降沿，3 是下一个上升沿。三个边沿定义生成时钟的上升沿、下降沿及下一次上升沿；`-edge_shift` 可分别施加偏移。`-edges` 不可与分频、倍频或占空比选项同时使用。生成时钟源位于 PLL 等黑盒时，可用 `set_clock_latency` 显式给出延迟。理想生成时钟仍继承主时钟到生成源的延迟，只将自身网络分发视为理想；`set_disable_timing` 禁用的 CLK→Q/QN 弧不参与源延迟和数据 launch。

与 SDC 标准不同，miniSTA 默认沿 Liberty 时序弧传播时钟（SDC 默认是理想时钟）；给时钟设了网络延迟（不带 `-source` 的 `set_clock_latency`）后按理想时钟处理。时钟源 slew 的取法：理想时钟只取 `set_clock_transition`；显式 `set_propagated_clock` 的时钟只取源端口的 `set_input_transition`；这两种未给出时都为 0。未显式声明的默认传播时钟依次取输入 slew、`set_clock_transition`、默认 slew。

传播时钟的到达时间和摆率按 min/max × rise/fall 分开保存；反相弧交换输入边沿，launch/capture 使用对应的本地时钟边沿。`create_clock -add` 允许同一源网络上有多个时钟；跨时钟检查会查找最紧的出发和捕获边沿对。`set_ideal_network` 标记的网络不累计时序弧延迟；`-no_propagate` 限制理想属性继续传递。

## I/O、负载与工作条件

| 命令 | 主要用途 |
| --- | --- |
| `set_input_delay`、`set_output_delay` | I/O 时序要求；支持时钟、边沿、max/min、`-add_delay`、`-reference_pin` 等选项 |
| `set_load` | 端口或网络的集中电容负载，支持 `-pin_load`、`-wire_load`、`-subtract_pin_load` |
| `set_input_transition` | 输入转换时间，可按边沿和 max/min 指定 |
| `set_driving_cell` | 用 Liberty 单元的延迟和输出转换时间模拟外部驱动 |
| `set_case_analysis`、`set_disable_timing` | 固定逻辑值或禁用单元时序弧 |
| `set_logic_zero`、`set_logic_one`、`set_logic_dc` | 把端口或网络标为常量 0、常量 1 或无关值 |
| `set_operating_conditions`、`set_voltage` | 选择工作条件、库角和电压信息 |
| `set_max_transition`、`set_max_fanout`、`set_max_capacitance`、`set_min_capacitance`、`set_max_area` | 设计规则与面积限制 |

对 I/O delay，`-max` 和 `-min` 分别约束 setup 和 hold；只写其中一个不会自动产生另一个角的值。两个选项都不写时，同一个值用于两角。`-source_latency_included` 和 `-network_latency_included` 表示延迟数值已包含相应时钟延迟。`-reference_pin` 使用指定时钟树引脚或端口上的时钟到达作为参照。

`set_logic_zero` 和 `set_logic_one` 的效果与 `set_case_analysis 0`、`set_case_analysis 1` 相同。`set_logic_dc` 标记的对象不作为路径起点，路径也不穿过它，但它不提供确定的逻辑值，不参与单元极性推导。

`set_driving_cell` 支持选择库、输入/输出引脚及输入转换时间。多库情况下可用 `-library` 或 `库名/单元名` 消除同名单元歧义。`read_liberty` 可读取多个文件；`set_operating_conditions` 可以为 max/min 分析选择不同的库和工作条件。若库提供 K 因子并指定电压或温度，延迟会按该信息缩放；否则电压和温度仅用于报告。

## 路径约束与分组

| 命令 | 用途 |
| --- | --- |
| `set_false_path` | 排除匹配的时序路径 |
| `set_multicycle_path` | 修改路径的捕获周期 |
| `set_max_delay`、`set_min_delay` | 为路径设置延迟预算 |
| `group_path` | 将匹配的路径归入命名报告组 |
| `set_data_check` | 两条数据路径之间的 setup 检查 |

路径选取可使用 `-from`、`-to` 和重复的 `-through`。同一个 `-through` 集合中的对象取“或”，多个 `-through` 按路径经过顺序匹配。支持路径边沿限定；边沿无法判定时会告警。`group_path` 只改变报告分组，不改变 slack。

`set_data_check` 支持 setup 检查，不建模 hold 数据检查。`set_clock_gating_check` 的边沿、高低电平选项不会改变按 Liberty 标记确定的检查对象。

## 未支持的选项

- `create_generated_clock -add`、`-combinational`，`set_clock_sense -pulse`，`set_clock_groups -allow_paths` 等选项会使整条约束作废并告警。
- 部分常见工具扩展（如 `-quiet`、`all_inputs -no_clocks`）可以读取，并告警说明它们不是 SDC 1.8 语法。
- 不支持的 SDC 命令或选项不应被视为已参与时序分析。miniSTA 整体的模型范围见 [README 的支持范围](../README.md#支持范围)。

## 明确不支持的命令

下列命令属于 SDC 1.8，但 miniSTA 不建模。读取时会识别它们，告警说明原因，并只忽略该条命令。

| 命令 | 说明 |
| --- | --- |
| `set_drive` | 用电阻描述输入驱动的过时写法；请使用 `set_driving_cell` |
| `set_resistance` | 给网络标注电阻；miniSTA 不计算线网 RC |
| `set_fanout_load` | 用扇出负载单位描述输出负载的过时写法；请使用 `set_load` |
| `set_port_fanout_number` | 用外部扇出个数描述端口负载的过时写法；请使用 `set_load` |
| `set_wire_load_min_block_size` | 线负载模型的设置；miniSTA 不使用线负载模型 |
| `set_wire_load_mode` | 同上 |
| `set_wire_load_model` | 同上 |
| `set_wire_load_selection_group` | 同上 |
| `create_voltage_area` | 多电压域的物理区域，不是 STA 约束 |
| `set_level_shifter_strategy` | 多电压域的电平转换策略，不是 STA 约束 |
| `set_level_shifter_threshold` | 多电压域的电平转换阈值，不是 STA 约束 |
| `set_max_dynamic_power` | 动态功耗目标，不是 STA 约束 |
| `set_max_leakage_power` | 漏电功耗目标，不是 STA 约束 |

其他不认识的命令同样告警并忽略。

SDC 用例在 `testcases/sdc_features/`，`make test` 会全部运行。其中 `sdc_commands.sdc` 对手册中的命令各写一条，检查它们都能被识别。
