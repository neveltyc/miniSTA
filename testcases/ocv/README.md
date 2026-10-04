# 基础 OCV 回归

`make test` 运行本目录的 27 个用例，并由 `scripts/check_ocv.py` 断言手算 slack。`make compare` 用本地 OpenSTA 对照其中 26 个用例，容差 0.001 ns；`clock_object` 是 miniSTA 的时钟对象 derate 扩展，OpenSTA 不接受这种对象，只做手算断言。OpenSTA 对照关闭 `sta_crpr_enabled`，因为 miniSTA 当前不计算 CPPR。

直接运行：

```bash
./build/msta testcases/ocv/global.dofile
python3 scripts/check_ocv.py build/msta vendor/opensta-build/sta build/compare_sta
```

## 电路与手算

库中 BUF 延迟 rise/fall = 2/1 ns，DATA = 3/1 ns；DFF clk-to-Q = 0.5/0.25 ns，setup = 1/0.5 ns，hold = 0.2/0.1 ns。全部表为常量，不受负载、slew 影响。时钟周期 10 ns，launch 时钟经过 2 个 BUF，capture 时钟经过 3 个 BUF，其中第一个 BUF 共用。寄存器之间经过一个 DATA。

系数为 1 时，launch clock = 4，capture clock = 6；数据 rise/fall 到达为 7.5/5.25。因此 setup slack 为 min(10+6−1−7.5, 10+6−0.5−5.25) = **7.5 ns**；hold slack 为 min(7.5−6−0.2, 5.25−6−0.1) = **−0.85 ns**。

`global` 设置 early = 0.8、late = 1.2，默认只缩放延迟，不缩放检查值：

- setup：late launch = 4.8，early capture = 4.8；rise arrival = 4.8+(0.5+3)×1.2 = 9；required = 10+4.8−1 = 13.8；slack = **4.8 ns**。
- hold：early launch = 3.2，late capture = 7.2；fall arrival = 3.2+(0.25+1)×0.8 = 4.2；required = 7.2+0.1 = 7.3；slack = **−3.1 ns**。
- `bc_wc` / `single` 使用同角 capture：setup = 10+7.2−1−9 = **7.2 ns**；hold = 4.2−4.8−0.1 = **−0.7 ns**。

`fast.lib` 的全部表值是普通库的一半：OCV 用早角库算 capture setup、用晚角库算 capture hold，得到 **(4.5, −3.425)**；bc_wc 同角分析得到 **(7.5, −0.425)**。比较使用 OpenSTA `read_liberty -min/-max`，miniSTA 使用 `set_operating_conditions -min_library/-max_library`。

`invert` 核对 derate 选择弧输入边沿。`nonunate` 的 rise/fall 系数让输入到达较早的 fall 路径成为 setup 获胜路径：fall clk-to-Q = 0.25×0.5，NON 的输出 rise 延迟 = 2×2，arrival = 8.125，setup slack = 16−1−8.125 = **6.875 ns**。

## 覆盖

| 用例 | 重点 |
| --- | --- |
| baseline / global | 缺省系数、全局 early/late、反角 capture、报告 arrival/required |
| split / split_reverse | clock/data 独立设置，改变命令顺序不影响结果 |
| obj / obj_fallback | 实例优先，全局回退，late 不覆盖 early，rise 不覆盖 fall |
| obj_clock / obj_data_only | 时钟树实例系数、实例的 data 系数不作用于 clock |
| edge / clock_edge / invert | 全局 rise/fall，clk-to-Q 输入沿，反相弧输入沿 |
| check / check_obj / both_types | 全局/实例检查值、检查数据边沿、delay 与 check 同时设置 |
| ocv_mode / bc_wc / single / switch | 仅设置分析模式、模式切换 |
| overwrite / unset | 同维度后写覆盖，清除全部 timing derate |
| nonunate | 两个输入边沿的完整候选比较 |
| ideal / generated / io_reference | 理想 latency 不缩放、生成时钟继承、输出参考引脚 |
| clock_object | miniSTA 已有的时钟对象扩展 |
| corners_ocv / corners_bc_wc | early/late Liberty 库选择与 capture 角 |

参考 [OpenSTA 文档](https://opensta.readthedocs.io/en/latest/Commands/) 与本地 OpenSTA 的 `sdc/Sdc.tcl`、`sdc/Sdc.cc`、`search/Search.cc` 中的语义。支持范围与边界见 [SDC 参考](../../docs/sdc.md#ocv-分析)。
