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
| to_rise | `-rise_to` D 引脚：只切 D↑，保留 D↓ | 4.1 | 1.1 |
| to_fall | `-fall_to` D 引脚：只切 D↓，保留 D↑ | 6.5 | 0.5 |
| from_rise | `-rise_from` 输入端口：按起点数据边沿沿前驱回溯，只切 d↑ 出发的路径 | 4.1 | 1.1 |
| from_fall | `-fall_from` 输入端口：只切 d↓ 出发的路径 | 6.5 | 0.5 |
| clock_to_rise | `-rise_to` 时钟：限定捕获时钟边沿，上升沿捕获的寄存器路径全部被切 | — | — |
| clock_to_fall | `-fall_to` 时钟：寄存器在上升沿捕获，不受影响 | 4.1 | 0.5 |
| output_clock_to_rise | 输出延迟参照时钟下降沿；`-rise_to` 时钟只切寄存器路径，保留到输出 q 的路径 | 2.9 | 5.1 |
| output_clock_to_fall | 同上，`-fall_to` 时钟按输出延迟的参考时钟边沿匹配，只切到 q 的路径（与数据边沿无关） | 4.1 | 0.5 |
| async_phase | a 从时钟上升沿、b 从下降沿出发，经 OR2 驱动异步复位；不同 launch 相位独立传播（slack 为 recovery/removal） | 3 | −1 |
| gating_phase | 同样的汇聚逻辑驱动门控使能脚；跨多个 launch 分组仍只计一条门控检查 | 3 | −1 |

clock_to_rise 的 setup/hold 均没有可分析的路径。async_phase 中 recovery 最差路径来自 a（D↓，arrival=2 ns，capture=5 ns），removal 最差路径来自 b（D↑，arrival=6 ns，launch/capture=5 ns）。

`make test` 检查以上 17 个用例的手算值，normal 至 clock_to_fall 中有路径的用例还检查获胜边沿、约束值和路径快照。
`make compare` 在 OpenSTA 可用时逐个运行并比较 setup/hold WNS（容差 1 ps）。

miniSTA 的约束表最多二维（NLDM），不支持三维约束表。
