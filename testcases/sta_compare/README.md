# sta_compare 用例

这个小型门级电路包含输入路径、触发器和输出路径，可用于查看 setup/hold 报告或运行参考工具对比。单元取自 `testcases/lib/sky130.lib`（sky130_fd_sc_hd，tt_025C_1v80 角），三个工具读同一份库。

在仓库根目录执行：

```bash
make
./build/msta testcases/sta_compare/msta.dofile
make compare
```

`msta.dofile` 使用 `common.sdc`；`opensta.tcl` 和 `opentimer.cmd` 分别供参考工具运行。`opentimer.sdc` 是适配 OpenTimer 输入语法的约束文件。第三方测试数据的来源和许可见[第三方声明](../../THIRD_PARTY_NOTICES.md)。
