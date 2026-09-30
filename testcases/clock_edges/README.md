# 时钟 min/max × rise/fall 回归

时钟网络保存四组 arrival/slew，均指网络引脚上的本地边沿。组合弧按 timing_sense 映射输入边沿，按输出边沿查 cell_rise/cell_fall 和 rise_transition/fall_transition。反相时钟的源边沿与寄存器触发引脚的本地边沿分别使用。

setup 捕获使用 early arrival/slew，hold 捕获使用 late arrival/slew；launch 使用数据分析角对应的 arrival/slew。生成时钟的 clk-to-Q 源延迟也按输出边沿分别计算。合并数值只供摘要报告和 DRC 使用。

`capture_rise` 复现不对称 BUF：rise/fall 延迟为 2/1 ns，输出 slew 为 0.2/0.4 ns。上升沿 DFF 的 setup 捕获时刻为 12 ns，D↓ 约束为 4 ns，arrival 为 2 ns，所以 required=8 ns、slack=6 ns。hold 最差 slack=−1.6 ns。

| 用例 | 检查点 | setup / hold WNS (ns) |
| --- | --- | --- |
| capture_rise | 上升沿捕获，BUF 两边沿不对称 | 6 / −1.6 |
| capture_fall | 下降沿触发 DFF | −0.2 / 4.2 |
| capture_invert | 反相时钟路径及源边沿映射 | 1 / 3.4 |
| capture_nonmonotonic | 约束随时钟 slew 增大而减小 | 6.4 / −1.2 |
| launch_rise | 上升沿 launch 插入延迟 | 5.9 / 3.1 |
| launch_fall | 下降沿 launch 插入延迟 | 1.9 / 7.1 |
| launch_invert | 反相路径上的 launch 插入延迟 | 0.9 / 8.1 |
| source_slew | rise/fall 源摆率及首级查表 | 5 / −2.6 |
| source_slew_invert | 反相弧使用相反输入边沿的 slew | 3 / −0.6 |
| source_latency | early/late × rise/fall 源延迟 | 5.6 / −2 |
| ideal_source_slew | 理想时钟的 rise/fall 源摆率 | 4 / 0.4 |
| ideal_corner_slew | setup 取 early slew，hold 取 late slew | 3.8 / 0.4 |
| ideal_edges | 理想网络的四组显式 latency/transition | 4.3 / −0.4 |
| reference_pin | 输入参考引脚选择本地时钟边沿 | 2.1 / 2.5 |
| generated_buf | 组合输出生成时钟继承主时钟插入延迟和 slew | 6 / −1.6 |
| generated_buf_invert | 反相生成时钟接管端点，主时钟只供源延迟继承 | 1 / 3.4 |
| generated_qn | QN 源正确使用 CLK→QN 弧，并分别计算输出 rise/fall | 6.6 / −2.4 |
| source_conflict | 传播时钟优先使用已指定的源输入 slew | 5 / −2.6 |
| ideal_zero_slew | 显式零时钟 slew 参与查表，不能替换成默认值 | 4.2 / 0.6 |
| generated_ideal | 理想生成时钟仍继承主时钟源延迟 | 6.1 / −1.5 |
| generated_ideal_net | 继承源延迟后再加入自身网络延迟 | 7.1 / −2.5 |
| generated_ideal_qn | 理想 QN 生成时钟保留 CLK→QN 源延迟 | 6.8 / −2.2 |
| generated_disable | 禁用 CLK→QN 后使用显式生成时钟定义并告警 | 4.2 / 0.6 |
| zero_output | 转换表中的零 slew 保留 | 6.2 / −1.4 |
| launch_disable | 普通数据 launch 同样尊重禁用的 CLK→Q 弧 | 无可分析路径 |

`make test` 对全部 25 个用例做手算断言，还核对 capture、check、required、arrival 和 launch 插入延迟。
`make compare` 对 23 个用例与 OpenSTA 做 WNS 对比（容差 1 ps）。`ideal_edges` 和 `reference_pin` 保持 miniSTA 现有的显式理想网络/参考引脚模型，用手算断言验证，不作为与 OpenSTA 等价的用例。源摆率依赖的传播测试同时设置 input transition 与 clock transition，以避免两工具对传播时钟源摆率默认值的差异。

生成时钟定义点上的主时钟仍保留 rise/fall 插入延迟，供生成时钟继承；数据 launch/capture 改由生成时钟接管，主时钟不再越过这个定义点传播。普通多时钟源上的 `create_clock -add` 行为保留。
传播时钟源已给出 `set_input_transition` 时优先采用该输入 slew；理想时钟使用 `set_clock_transition`。未给出输入 slew 时仍保留 miniSTA 原有源摆率兜底，默认值差异仍可能影响与 OpenSTA 的对比。

理想生成时钟只将自身的网络分发视为理想，仍继承主时钟到生成源的延迟；其 capture、launch 和 I/O 参考源延迟都使用同一份继承结果，理想时钟 slew 则采用自身 `set_clock_transition`。
禁用的 CLK→Q/QN 弧不会用于生成时钟源或普通数据 launch。生成时钟无有效时序源弧时，报告告警，保留显式生成时钟波形和注解 source latency；传播源 slew 取零，与该用例中 OpenSTA 的无源路径行为一致。
转换表缺失时仍使用已有备用表或默认 slew，明确给出的零值不会再视为缺表。数据侧约束查表也保留零 slew。
