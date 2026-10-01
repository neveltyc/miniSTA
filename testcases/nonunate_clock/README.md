# non_unate 时钟回归

时钟 arrival/slew 按 min/max × 本地边沿 × 源边沿保存。non_unate 同时传播两种源标签，同相和反相路径汇聚时也保留两者；launch/capture 分别求边沿关系并取最差 slack。报告显示获胜的 launch/capture source edge。

XOR 复现：源下降沿 5 ns 经 XOR 在 7 ns 形成 CLK↑，setup=4 ns，data arrival=2 ns，因此 required=3 ns、slack=1 ns；hold 为 −1.6 ns。xor_violation 将输入 max delay 改为 3 ns，setup WNS/TNS 为 −1 ns，确认不会漏报这条违例。

21 个用例覆盖 XOR、同相/反相 MUX 汇聚及后续缓冲、理想时钟、launch、源边沿例外、不确定度、selector case analysis、寄存器集合、异步检查、门控、生成时钟和 Liberty 布尔优先级。

`make test` 做手算与获胜标签断言。`make compare` 将其中 20 个用例与 OpenSTA 比较（1 ps 容差）。xor_slew 单独按源标签的 slew 做手算：其 setup/hold 为 0.6/−1.6 ns；OpenSTA GBA 使用引脚 rise/fall min/max 合并 slew，因此该用例不作为等价比较。此差异已保留在检查脚本的输出中。

修复后的 review 同时修正了 selector 固定时的可行极性裁剪、边沿不确定度选项误解析，以及 Liberty XOR 高于 AND 的优先级。侧输入条件求值支持常见布尔运算；语法不支持或自由侧输入超过 8 个时，保守回退库声明的 timing_sense。边沿限定 all_registers 查询只选确定单一源极性的寄存器，按源边沿裁剪路径应使用 -rise_to/-fall_to 时钟例外。

全部定向用例通过 ASan/UBSan。
