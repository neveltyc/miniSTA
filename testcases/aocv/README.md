# AOCV 深度 GBA 回归

原生格式、优先级和分析范围见 [variation.md](../../docs/variation.md)。`make test` 通过 `scripts/check_variation.py` 验证本目录；报告显示实际弧的结构深度。使用自己编写的常量 Liberty 表，避免负载、slew 干扰手算。

## 手算

基础电路与 [OCV 用例](../ocv/README.md) 相同。`base.aocv` 的 early 曲线为 (1:.8, 2:.9, 4:.95)，late 为 (1:1.2, 2:1.1, 4:1.05)；depth=3 插值为 .925 / 1.075。

短电路数据路径深度为 2。公共时钟 BUF 的最短上下文为 2，launch 分支为 2，capture 分支为 3：

- setup：launch=4.4；capture=2×.9+4×.925=5.5；data arrival=4.4+3.5×1.1=8.25；required=10+5.5−1=14.5；slack=**6.25**。
- hold：launch=3.6；capture=2×1.1+4×1.075=6.5；data arrival=3.6+1.25×.9=4.725；required=6.5+.1=6.6；slack=**−1.875**。
- 加 CPPR：公共 BUF 的差为 2×(1.1−.9)=.4；slack=**6.65 / −1.475**。

长电路有 3 个 DATA，数据总深度为 4：setup arrival=4.4+(0.5+9)×1.05=14.375，slack=**.125**；hold arrival=3.6+(.25+3)×.95=6.6875，slack=**.0875**。

混合电路在同一起点出发，短支路为 1 个 DATA，长支路为 3 个 DATA，最后通过 MERGE 汇聚。共享 clk-to-Q 用最短完整深度 3，短支路深度 3，长支路深度 5（取最后边界行）：setup 选长支路，slack=**−.9375**；hold 选短支路，slack=**−.91875**。这同时检查共享弧上下文、插值、边界和不同角的获胜支路。

## 用例

| 用例 | 覆盖 |
| --- | --- |
| short / long / mixed | 完整深度、分支与汇聚、长短路径、线性插值 |
| clamp | 超出最后深度后的边界系数 |
| data / clock | 路径类别独立，未匹配维度为 1 |
| scalar | 常量 AOCV 表等价于固定 OCV |
| priority | 设计/库单元/实例同时匹配，并按未设置边沿回退 |
| data_check | 中间检查两侧纳入数据深度端点；−4.225, 0 |
| input_ref / output_ref | I/O 参考引脚纳入时钟深度端点 |
| cell / instance | 库单元、实例优先级与缺省回退 |
| disabled | 保留表但关闭分析 |
| reload | 换表后缓存失效，整套替换 |
| cppr / multiply | 与 CPPR / 固定 derate 联合使用 |

断言脚本另直接测试导入 API：非整数/溢出/零 depth、NaN/Inf/负系数、超范围 early/late、非单调/重复 depth、额外 distance 列、空表、超长行、过多行都会拒绝整文件，旧表与开关保持不变。也检查边界及插值的数值。

坏表回归另覆盖行中、尾部、注释内的 NUL，失败仍保留旧表与开关。
