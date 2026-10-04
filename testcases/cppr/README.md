# CPPR 回归

规则与边界见 [variation.md](../../docs/variation.md)。`make test` 做手算断言，`make compare` 在 OpenSTA 上映射 `set_cppr` 到 `sta_crpr_enabled` / `sta_crpr_mode`，比较 setup/hold WNS（容差 .001 ns）。

基础 OCV early=.8、late=1.2，公共 BUF 的 rise 延迟为 2 ns，差值为 .8 ns。基础 slack=4.8/−3.1，开启 CPPR 后为 **5.6/−2.3**。`source` 增加 early=.2、late=.6 的 source latency，补偿为 1.2，但因原 slack 同时减少 .4，最后结果仍为 5.6/−2.3。

`same_transition` / `same_pin` 的 capture 时钟经过反相器，公共路径的 rise/fall 差为 .8/.4；same_transition 不补偿，same_pin 补偿 .4，结果分别为 **−1/3.1**、**−.6/3.5**。

`ranking` 有两个 launch 寄存器：一个与 capture 共用更多时钟路径、原始到达更晚；另一个到达较早，但 CPPR 补偿更少。开启 CPPR 后最差 setup 起点必须是 `u_launch`，WNS=**4.4**。如果按同一 launch 相位把两个起点合并，可能错误报告另一条路径。

| 用例 | 覆盖 / 预期 setup, hold |
| --- | --- |
| on / off | 开关；5.6, −2.3 / 4.8, −3.1 |
| bc_wc | 非 OCV 不补偿；7.2, −.7 |
| no_common | 无公共单元，补偿为零；3.2, −.7 |
| aliases | 同源不同命名时钟；5.6, −2.3 |
| generated | 生成时钟继承主时钟真实路径；5.6, −2.3 |
| async / gate | recovery/removal、门控检查补偿；5.6, −2.3 |
| source | 源延迟 min/max 差；5.6, −2.3 |
| io_reference | 输出参考引脚，整条共用路径得到补偿；5.6, −2.3 |
| same_pin / same_transition | 异沿规则 |
| ranking | 补偿后重新选择最差起点；4.4, −1.5 |
| path_budget / async_budget | 本工具既有 launch-relative 预算，无 CPPR；3.8, 0 |
| reconvergent | 早晚角选择不同前驱，保守保留同一物理根前缀；2, −5.9 |

16 个用例中 13 个与 OpenSTA 对照。`path_budget` / `async_budget` 涉及既有预算语义差异，`reconvergent` 涉及更保守的公共前缀定义，只做手算回归。后者无论分岔后再次经过哪些同名网络，都只补偿根 BUF 的 .8 ns。
