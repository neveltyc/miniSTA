# 按数据边沿检查约束

BUF 的 rise/fall 延迟为 2/1 ns，输出 slew 为 0.2/0.4 ns。输入 arrival 为 1 ns，时钟周期为 10 ns，时钟 slew 为 0.1 ns。因此 D↑ arrival=3 ns，D↓ arrival=2 ns。

normal.lib 的约束表使用时钟 slew C 和数据 slew D：

- setup rise=C+2D=0.5 ns，fall=3+C+2D=3.9 ns：最差为 D↓，slack=10−3.9−2=4.1 ns。
- hold rise=2+C+2D=2.5 ns，fall=C+2D=0.9 ns：最差为 D↑，slack=3−2.5=0.5 ns。

最差 slack 的数据边沿和最差 arrival 的边沿相反，能发现合并约束、arrival、slew 或报告前驱的错误。

| 用例 | 覆盖点 | setup slack (ns) | hold slack (ns) |
| --- | --- | ---: | ---: |
| normal | 时钟/数据轴，表体继承模板索引 | 4.1 | 0.5 |
| swapped | 数据/时钟轴，表值转置，时间与电容单位比例不同 | 4.1 | 0.5 |
| rise_only | 只有 rise 表，不能为 fall 生成零约束检查 | 6.5 | 0.5 |
| data_1d | 只有 constrained_pin_transition 轴 | 4.5 | 0.7 |
| negative | 负约束不能被零值截断 | 7.2 | 2.4 |
| cut_rise | 只排除上升路径，setup/hold 均检查下降路径 | 4.1 | 1.1 |
| cut_fall | 只排除下降路径，setup/hold 均检查上升路径 | 6.5 | 0.5 |

`make test` 检查以上手算值、获胜边沿、约束值和路径快照。
`make compare` 在 OpenSTA 可用时逐个运行并比较 setup/hold WNS（容差 1 ps）。

miniSTA 仍使用现有二维 NLDM 表和时钟传播模型；本修改不增加三维约束表支持。

两轮 review 的补充回归：

- `to_rise/to_fall`：D 引脚的路径例外只切对应数据边沿；`clock_to_rise/clock_to_fall` 则限定捕获时钟边沿。
- `from_rise/from_fall`：输入端口的起点数据边沿沿前驱回溯，反相路径也使用起点上的真实边沿。
- `output_clock_to_rise/output_clock_to_fall`：输出延迟参照时钟下降沿，即使输出的数据边沿为上升，也必须按参考时钟边沿匹配。
- `async_phase`：a 从时钟上升沿出发，b 从下降沿出发，经 OR2 汇聚。恢复检查最差路径来自 a（D↓，arrival=2 ns，capture=5 ns，slack=3 ns）；移除检查最差路径来自 b（D↑，arrival=6 ns，launch/capture=5 ns，slack=−1 ns）。不同 launch 相位必须独立传播，不能只保留合并 arrival 的路径。
- `gating_phase`：同样的输入和汇聚逻辑用于门控使能脚，setup/hold WNS 为 3/−1 ns；跨多个 launch 分组仍只计一条门控检查。

以上共 17 个定向用例均包含在 `make test` 和 OpenSTA 对比中。
