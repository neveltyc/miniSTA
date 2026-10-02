#!/usr/bin/env bash
#
# SDC 语义断言：读取 run_all.sh 写在 build/ 下的用例日志，检查约束是否按预期生效。
# 由 make test 调用；加 SDC 用例时在这里同步加断言。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG="${ROOT}/build"

grep -Eq '^external[[:space:]]+50\.000' "${LOG}/testcases_sdc_features_clock_groups.dofile.log"
grep -Fq 'setup : WNS    9.748 ns' "${LOG}/testcases_sdc_features_add_delay.dofile.log"
grep -Fq 'capture clock: external @ 50.000 ns' "${LOG}/testcases_sdc_features_add_delay.dofile.log"
# external/spare 与 core 之间是异步域，跨域的那条 input->register 路径要被排除掉。
grep -Fq '未约束 0 个   路径例外排除 1 个' "${LOG}/testcases_sdc_features_clock_groups.dofile.log"
if grep -Fq 'startpoint : inp1' "${LOG}/testcases_sdc_features_clock_groups.dofile.log"; then
    echo 'clock_groups: cross-domain path from inp1 was not cut' >&2
    exit 1
fi
grep -Eq '^slow[[:space:]]+20\.000' "${LOG}/testcases_sdc_features_generated.dofile.log"
grep -Eq '^slow[[:space:]]+20\.000[[:space:]]+0\.100[[:space:]]+0\.100' "${LOG}/testcases_sdc_features_generated.dofile.log"
grep -Fq 'capture clock: master @ 11.200 ns' "${LOG}/testcases_sdc_features_generated.dofile.log"
grep -Fq 'capture clock: master @ 1.500 ns' "${LOG}/testcases_sdc_features_generated.dofile.log"
grep -Fq '最大插入延迟 0.689 ns' "${LOG}/testcases_sdc_features_generated.dofile.log"
# create_generated_clock -edges：用主时钟的第 1/3/5 个边沿定义新时钟。
# 主时钟波形 1.0/4.0、周期 10 → rise@1、fall@11、下个 rise@21：周期 20，
# 与 generated.sdc 的 -divide_by 2 是同一个波形。
grep -Eq '^slow[[:space:]]+20\.000' "${LOG}/testcases_sdc_features_generated_edges.dofile.log"
grep -Fq '  edges 1.000/11.000 ns' "${LOG}/testcases_sdc_features_generated_edges.dofile.log"
# -edges 还能做出 -divide_by 做不到的波形：duty30 = 周期 10、1.00/4.00；
# shifted = 周期 9.8、1.50/4.20。
grep -A1 -E '^duty30' "${LOG}/testcases_sdc_features_generated_edges_duty.dofile.log" \
    | grep -Fq '  edges 1.000/4.000 ns'
grep -A1 -E '^shifted' "${LOG}/testcases_sdc_features_generated_edges_shift.dofile.log" \
    | grep -Fq '  edges 1.500/4.200 ns'
grep -Fq 'setup : WNS    8.513 ns' "${LOG}/testcases_sdc_features_generated_edges.dofile.log"
grep -Fq 'setup : WNS    8.813 ns' "${LOG}/testcases_sdc_features_generated_edges_duty.dofile.log"
grep -Fq 'setup : WNS   -0.972 ns' "${LOG}/testcases_sdc_features_generated_edges_shift.dofile.log"
grep -Fq 'startpoint : inp2' "${LOG}/testcases_sdc_features_reconverge.dofile.log"
grep -Fq 'endpoint   : f1' "${LOG}/testcases_sdc_features_reconverge.dofile.log"
grep -Fq 'startpoint : inp2' "${LOG}/testcases_sdc_features_same_clock_exception.dofile.log"
grep -Fq 'endpoint   : f1' "${LOG}/testcases_sdc_features_same_clock_exception.dofile.log"

# -setup 只切寄存器的 setup 检查，hold 检查保留。
if grep -Fq 'setup path (max corner, to register)' "${LOG}/testcases_sdc_features_false_setup.dofile.log"; then
    echo 'false_setup: register setup path was not cut' >&2
    exit 1
fi
grep -Fq 'hold path (min corner, to register)' "${LOG}/testcases_sdc_features_false_setup.dofile.log"
grep -Fq 'capture edge                             100.000' "${LOG}/testcases_sdc_features_multicycle.dofile.log"
grep -Fq 'hold edge                                  0.000' "${LOG}/testcases_sdc_features_multicycle.dofile.log"
grep -Fq 'setup : WNS   19.709 ns' "${LOG}/testcases_sdc_features_units.dofile.log"
grep -Fq 'hold  : WNS   -9.784 ns' "${LOG}/testcases_sdc_features_units.dofile.log"
grep -Fq 'startpoint : inp1' "${LOG}/testcases_sdc_features_path_budget.dofile.log"
grep -Fq 'startpoint : inp2' "${LOG}/testcases_sdc_features_case_disable.dofile.log"

# 前端容错：拼错的命令名、未建模的集合选项各丢自己一条，其余约束照常生效。
grep -Fq 'collection option -expression is not modeled; the command is skipped' \
    "${LOG}/testcases_sdc_features_tolerant.dofile.log"
grep -Fq 'unknown sdc command "set_max_transtion" (ignored)' \
    "${LOG}/testcases_sdc_features_tolerant.dofile.log"
grep -Fq 'set_voltage: per-object voltage is not modeled; constraint rejected' \
    "${LOG}/testcases_sdc_features_tolerant.dofile.log"
grep -Fq '1 command(s) skipped; the remaining constraints are applied' \
    "${LOG}/testcases_sdc_features_tolerant.dofile.log"
grep -Fq 'setup : WNS   19.748 ns' "${LOG}/testcases_sdc_features_tolerant.dofile.log"
grep -Fq '未约束 0 个' "${LOG}/testcases_sdc_features_tolerant.dofile.log"

# -through：组内取"或"，组与组按路径顺序匹配，顺序反了就不命中。
grep -Fq '路径例外排除 1 个' "${LOG}/testcases_sdc_features_through_order.dofile.log"
grep -Fq '路径例外排除 0 个' "${LOG}/testcases_sdc_features_through_reversed.dofile.log"

# -rise_through / -fall_through：命中路径上的那个边沿时，换成同一条链上的另一个
# 边沿继续查（端点不是整条被切掉）。对照基准最差路径 slack 15.505、在 X 上下降。
grep -Fq 'setup : WNS   15.684 ns' "${LOG}/testcases_sdc_features_through_edge.dofile.log"
grep -Fq 'hold  : WNS    0.265 ns' "${LOG}/testcases_sdc_features_through_edge.dofile.log"
grep -Fq 'endpoint   : my_cfo' "${LOG}/testcases_sdc_features_through_edge.dofile.log"
if grep -Fq '未约束 1 个' "${LOG}/testcases_sdc_features_through_edge.dofile.log"; then
    echo 'through_edge: 端点被整条切掉了（应该换到次优路径）' >&2
    exit 1
fi
# 最差 setup 路径在 X 上是下降沿，setup 不变；hold 独立选取未被切掉的边沿。
grep -Fq 'setup : WNS   15.505 ns' "${LOG}/testcases_sdc_features_through_edge_rise.dofile.log"
grep -Fq 'hold  : WNS    0.477 ns' "${LOG}/testcases_sdc_features_through_edge_rise.dofile.log"
for edge_case in through_edge through_edge_rise; do
    if grep -Fq 'Warning: sdc' "${LOG}/testcases_sdc_features_${edge_case}.dofile.log"; then
        echo "through_edge: ${edge_case} 产生了 sdc 告警" >&2
        exit 1
    fi
done
# rise/fall 的 I/O 延迟按保守方式合并，结果与 SDC 命令顺序无关。
grep -Fq 'setup : WNS   44.754 ns' "${LOG}/testcases_sdc_features_io_edge_rise_first.dofile.log"
grep -Fq 'setup : WNS   44.754 ns' "${LOG}/testcases_sdc_features_io_edge_fall_first.dofile.log"

# set_operating_conditions / set_voltage：选择分析角（bc_wc）并记录/报告电压温度；
# 库里没有 K 因子时明确告警"表值原样用"。
grep -Fq '工艺角   : late tt_025C_1v80 [sky130_fd_sc_hd__tt_025C_1v80]（0.950 V / 105.0 C）   early tt_025C_1v80 [sky130_fd_sc_hd__tt_025C_1v80]（0.950 V / 105.0 C）' \
    "${LOG}/testcases_sdc_features_operating_conditions.dofile.log"
grep -Fq 'the library has no k_volt/k_temp factor; delay tables are used as read' \
    "${LOG}/testcases_sdc_features_operating_conditions.dofile.log"
grep -Fq 'set_voltage: 0.950 V is recorded, but the library has no k_volt factor' \
    "${LOG}/testcases_sdc_features_operating_conditions.dofile.log"
grep -Fq 'setup : WNS   15.505 ns' "${LOG}/testcases_sdc_features_operating_conditions.dofile.log"
grep -Fq 'hold  : WNS    0.465 ns' "${LOG}/testcases_sdc_features_operating_conditions.dofile.log"
grep -Fq 'set_operating_conditions: -analysis_type on_chip_variation is not modeled; constraint rejected' \
    "${LOG}/testcases_sdc_features_operating_conditions.dofile.log"

# 显式库限定必须精确匹配，不能静默改用另一个库里的同名单元。
grep -Fq 'set_driving_cell: cell "no_such_lib/INVX1" is not in the requested library' \
    "${LOG}/testcases_sdc_features_bad_driving_library.dofile.log"
grep -Fq '1 command(s) were not modeled and were ignored' \
    "${LOG}/testcases_sdc_features_bad_driving_library.dofile.log"

# set_data_check：-from 的到达（减去它的出发沿）加 margin 当要求时间，
# 计入 WNS/TNS。
grep -Fq '数据检查 : 1 条   setup 最差 -0.995 ns（my_cti -> u_sff/D，margin 0.500）' \
    "${LOG}/testcases_sdc_features_data_check.dofile.log"
grep -Fq 'setup : WNS   -0.995 ns' "${LOG}/testcases_sdc_features_data_check.dofile.log"
if grep -Fq 'Warning: sdc' "${LOG}/testcases_sdc_features_data_check.dofile.log"; then
    echo 'data_check: 标准写法不应该产生 sdc 告警' >&2
    exit 1
fi

# -from/-to 直接写 get_pins。
grep -Fq '路径例外排除 1 个' "${LOG}/testcases_sdc_features_pin_exception.dofile.log"
grep -Fq 'hold  : WNS    2.100 ns' "${LOG}/testcases_sdc_features_pin_exception.dofile.log"

# recovery/removal 端点同样接受路径例外：两个异步复位脚各有 recovery 和 removal 两个端点。
grep -Fq '路径例外排除 4 个' "${LOG}/testcases_sdc_features_async_exception.dofile.log"
grep -Fq 'hold  : WNS   -0.323 ns   TNS   -11.951 ns' "${LOG}/testcases_sdc_features_async_exception.dofile.log"

grep -Fq 'recovery path' "${LOG}/testcases_ac97_ac97.dofile.log"
# 设计规则检查：各类违例的个数与最差项。
grep -Fq 'max_transition 违例 60 处' "${LOG}/testcases_sdc_features_drc.dofile.log"
grep -Fq 'max_capacitance 违例 3 处（最差超出 120.244 fF）' "${LOG}/testcases_sdc_features_drc.dofile.log"
grep -Fq 'max_fanout 违例 3 处（最大扇出 39）' "${LOG}/testcases_sdc_features_drc.dofile.log"
grep -Fq 'min_capacitance 违例 40 处（最差低了 2.000 fF）' \
    "${LOG}/testcases_sdc_features_drc.dofile.log"
grep -Fq '面积  : 1385.1（目标 1000.0，超出 385.1）' \
    "${LOG}/testcases_sdc_features_drc.dofile.log"

# SDC 1.8 手册里的全部命令各来一条：没建模的告警后忽略，只丢自己那一条，
# 日志里不应出现 unknown sdc command。
if grep -Fq 'unknown sdc command' "${LOG}/testcases_sdc_features_sdc_commands.dofile.log"; then
    echo 'sdc_commands: 手册里的命令不应被当成未知命令' >&2
    exit 1
fi
# 过时命令明确不支持：告警里要写出"obsolete"和替代写法。
for obsolete in 'set_drive" is not modeled by msta: obsolete command (input drive resistance); use set_driving_cell instead' \
                'set_resistance" is not modeled by msta: obsolete command (net resistance); not modeled' \
                'set_fanout_load" is not modeled by msta: obsolete command (fanout load units); use set_load instead' \
                'set_port_fanout_number" is not modeled by msta: obsolete command (fanout load units); use set_load instead' \
                'set_wire_load_model" is not modeled by msta: obsolete command (wire load models); not modeled'; do
    grep -Fq "sdc command \"${obsolete}" "${LOG}/testcases_sdc_features_sdc_commands.dofile.log"
done
# 与 STA 无关的命令（多电压域 / 功耗）：明确不支持，告警里说清"不是 STA 约束"。
for nonsta in create_voltage_area set_level_shifter_strategy set_level_shifter_threshold \
              set_max_dynamic_power set_max_leakage_power; do
    grep -Fq "sdc command \"${nonsta}\" is not modeled by msta: not an STA constraint" \
        "${LOG}/testcases_sdc_features_sdc_commands.dofile.log"
done
# 库对象查询：手册里的写法照常执行，查不到对象只提示一句、不算"跳过命令"。
grep -Fq 'sdc: get_libs "xx" matched no libraries' \
    "${LOG}/testcases_sdc_features_sdc_commands.dofile.log"
grep -Fq 'sdc: get_lib_cells "xx" matched no lib cells' \
    "${LOG}/testcases_sdc_features_sdc_commands.dofile.log"
grep -Fq 'sdc: get_lib_pins "xx" matched no lib pins' \
    "${LOG}/testcases_sdc_features_sdc_commands.dofile.log"
# 其余命令照常生效：面积约束和 setup 汇总都在。
grep -Fq '面积  : 37.5（目标 100000.0，余量 99962.5）' \
    "${LOG}/testcases_sdc_features_sdc_commands.dofile.log"
grep -Fq 'setup : WNS    8.737 ns' "${LOG}/testcases_sdc_features_sdc_commands.dofile.log"

# set_clock_transition / set_timing_derate 对 slack 的影响。
grep -Fq 'setup : WNS   19.678 ns' "${LOG}/testcases_sdc_features_clock_transition.dofile.log"
grep -Fq 'hold  : WNS   -9.740 ns' "${LOG}/testcases_sdc_features_clock_transition.dofile.log"
grep -Fq 'setup : WNS   19.680 ns' "${LOG}/testcases_sdc_features_timing_derate.dofile.log"
grep -Fq 'hold  : WNS   -9.806 ns' "${LOG}/testcases_sdc_features_timing_derate.dofile.log"

# 时钟间不确定度：只作用于 -from/-to 指定的那对时钟。
grep -Fq -- '- clock uncertainty                       -0.500' "${LOG}/testcases_sdc_features_inter_clock_uncertainty.dofile.log"
grep -Fq -- '+ clock uncertainty                        0.200' "${LOG}/testcases_sdc_features_inter_clock_uncertainty.dofile.log"

grep -Fq 'capture edge                              20.000' "${LOG}/testcases_sdc_features_edge_io.dofile.log"

# 分对象 DRC：resetn 上单独设的扇出限制生效（扇出 153）。
grep -Fq 'max_fanout 违例 1 处（最大扇出 153）' \
    "${LOG}/testcases_multi_vt_drc_objects.dofile.log"
grep -Fq 'max_transition 违例 446 处' "${LOG}/testcases_multi_vt_drc_objects.dofile.log"
grep -Fq 'max_capacitance 违例 82 处' "${LOG}/testcases_multi_vt_drc_objects.dofile.log"

# 多阈值多库用例：一份网表里同时有 LVT/RVT/HVT 三种库的单元。
grep -Fq 'Libraries    : 3' "${LOG}/testcases_multi_vt_multi_vt_soc.dofile.log"
grep -Fq 'ics55_LLSC_H7CL_typ_tt_1p2_25' "${LOG}/testcases_multi_vt_multi_vt_soc.dofile.log"
grep -Fq 'ics55_LLSC_H7CR_typ_tt_1p2_25' "${LOG}/testcases_multi_vt_multi_vt_soc.dofile.log"
grep -Fq 'ics55_LLSC_H7CH_typ_tt_1p2_25' "${LOG}/testcases_multi_vt_multi_vt_soc.dofile.log"
grep -Fq 'setup : WNS   13.593 ns' "${LOG}/testcases_multi_vt_multi_vt_soc.dofile.log"
grep -Fq 'hold  : WNS    0.077 ns' "${LOG}/testcases_multi_vt_multi_vt_soc.dofile.log"
grep -Fq '寄存器 1613 个' "${LOG}/testcases_multi_vt_multi_vt_soc.dofile.log"
# 三方库的单元都要真的出现在网表里（综合时按块用了不同阈值库）。
for flavor in H7L H7R H7H; do
    grep -Fq "${flavor}" "${ROOT}/testcases/multi_vt/netlist/multi_vt_soc.v" \
        || { echo "multi_vt: netlist 里缺 ${flavor} 单元" >&2; exit 1; }
done
# 用 "库名/cell 名" 限定驱动单元的那条用例：能查到 cell，不应有 not in the library 告警。
if grep -Fq 'set_driving_cell: cell' "${LOG}/testcases_multi_vt_driving_cell_lib.dofile.log"; then
    echo 'multi_vt: 限定库名的 set_driving_cell 没查到 cell' >&2
    exit 1
fi
grep -Fq 'setup : WNS    3.599 ns' "${LOG}/testcases_multi_vt_driving_cell_lib.dofile.log"

# 理想网络（set_ideal_network）：不累计延迟。同一张网表的对照——4 级缓冲的时钟树
# 插入延迟 0.584 ns，把时钟端口标成理想网络后整棵树都是 0，-no_propagate 只去掉
# ct2 那一级（0.442 ns）。三个用例的 slack 差就是时钟偏差被去掉的那部分。
# 时钟端口没有 set_input_transition，传播时钟源 slew 按 SDC 取 0。
grep -Fq '时钟树：网络 5 根，算出插入延迟的 5 根，最深 4 级缓冲，最大插入延迟 0.584 ns' \
    "${LOG}/testcases_sdc_features_ideal_network.dofile.log"
grep -Fq 'setup : WNS    8.672 ns' "${LOG}/testcases_sdc_features_ideal_network.dofile.log"
grep -Fq 'hold  : WNS   -0.316 ns' "${LOG}/testcases_sdc_features_ideal_network.dofile.log"
grep -Fq '最大插入延迟 0.000 ns' "${LOG}/testcases_sdc_features_ideal_network_tree.dofile.log"
grep -Fq 'setup : WNS    8.813 ns' "${LOG}/testcases_sdc_features_ideal_network_tree.dofile.log"
grep -Fq 'hold  : WNS   -0.003 ns' "${LOG}/testcases_sdc_features_ideal_network_tree.dofile.log"
grep -Fq '最大插入延迟 0.442 ns' "${LOG}/testcases_sdc_features_ideal_network_noprop.dofile.log"
grep -Fq 'hold  : WNS   -0.177 ns' "${LOG}/testcases_sdc_features_ideal_network_noprop.dofile.log"
# set_ideal_latency / set_ideal_transition：理想网络上的延迟与摆率取给定值。
grep -Fq '最大插入延迟 0.500 ns' "${LOG}/testcases_sdc_features_ideal_network_value.dofile.log"
grep -Fq 'capture clock: core @ 10.200 ns' "${LOG}/testcases_sdc_features_ideal_network_value.dofile.log"
grep -Fq 'setup : WNS    8.336 ns' "${LOG}/testcases_sdc_features_ideal_network_value.dofile.log"
# 标准写法不该产生任何 sdc 告警：三条命令都要被认下来。
for idea_case in ideal_network ideal_network_tree ideal_network_noprop ideal_network_value; do
    if grep -Fq 'Warning: sdc' "${LOG}/testcases_sdc_features_${idea_case}.dofile.log"; then
        echo "ideal_network: ${idea_case} 产生了 sdc 告警" >&2
        exit 1
    fi
done

# create_clock -add：同一根网络两个时钟，各自传播、各自检查（见 docs/sdc.md）。
grep -Eq '^core[[:space:]]+20\.000' "${LOG}/testcases_sdc_features_clock_add.dofile.log"
grep -Eq '^slow[[:space:]]+50\.000' "${LOG}/testcases_sdc_features_clock_add.dofile.log"
grep -Fq '  时钟 core       网络 1 根，算出插入延迟的 1 根，最大插入延迟 0.000 ns' \
    "${LOG}/testcases_sdc_features_clock_add.dofile.log"
grep -Fq '  时钟 slow       网络 1 根，算出插入延迟的 1 根，最大插入延迟 0.000 ns' \
    "${LOG}/testcases_sdc_features_clock_add.dofile.log"
# 慢钟那一拍给得紧，最差 slack 必须出自 slow：-add 进来的时钟真的参与了检查。
grep -Fq 'launch clock : slow @ 0.000 ns' "${LOG}/testcases_sdc_features_clock_add.dofile.log"
grep -Fq 'capture clock: slow @ 50.000 ns' "${LOG}/testcases_sdc_features_clock_add.dofile.log"
grep -Fq 'setup : WNS    4.754 ns' "${LOG}/testcases_sdc_features_clock_add.dofile.log"
grep -Fq 'hold  : WNS    0.050 ns' "${LOG}/testcases_sdc_features_clock_add.dofile.log"
if grep -Fq 'Warning: sdc' "${LOG}/testcases_sdc_features_clock_add.dofile.log"; then
    echo 'clock_add: 标准写法不应该产生 sdc 告警' >&2
    exit 1
fi

# 跨时钟（周期不同）的边沿对齐：20 ns 的 core 与 50 ns 的 slow，最紧的 setup 关系
# 是 slow 第 1 拍（50）对 core 第 3 拍（60）。
grep -Fq 'launch clock : slow @ 50.000 ns' "${LOG}/testcases_sdc_features_clock_add_align.dofile.log"
grep -Fq 'capture clock: core @ 60.000 ns' "${LOG}/testcases_sdc_features_clock_add_align.dofile.log"
grep -Fq 'setup : WNS  -35.246 ns' "${LOG}/testcases_sdc_features_clock_add_align.dofile.log"
grep -Fq 'hold  : WNS    0.050 ns' "${LOG}/testcases_sdc_features_clock_add_align.dofile.log"
if grep -Fq 'Warning: sdc' "${LOG}/testcases_sdc_features_clock_add_align.dofile.log"; then
    echo 'clock_add_align: 标准写法不应该产生 sdc 告警' >&2
    exit 1
fi

# set_input_delay -clock_fall：出发沿和到达时间都参照时钟下降沿（25 ns）。
grep -Fq '  inp1                                           30.000     30.000   (input port )' \
    "${LOG}/testcases_sdc_features_input_clock_fall.dofile.log"
grep -Fq 'setup : WNS   19.748 ns' "${LOG}/testcases_sdc_features_input_clock_fall.dofile.log"
grep -Fq 'hold  : WNS   -9.820 ns' "${LOG}/testcases_sdc_features_input_clock_fall.dofile.log"
if grep -Fq 'Warning: sdc' "${LOG}/testcases_sdc_features_input_clock_fall.dofile.log"; then
    echo 'input_clock_fall: 标准写法不应该产生 sdc 告警' >&2
    exit 1
fi

# set_clock_sense：时钟路径上的反相器自动把 FF 的有效沿取反（捕捉沿落到 fall
# 沿 10.0），-positive 把它强制回 rise 沿，-stop_propagation 让时钟不再往下传。
grep -Fq '最深 2 级缓冲，最大插入延迟 0.170 ns' \
    "${LOG}/testcases_sdc_features_clock_sense.dofile.log"
# 输入延迟参照的出发沿是时钟 rise（0），捕捉沿是反相后的 fall（10.168）；
# hold 取不晚于出发沿的那个捕捉沿（-9.832）。
grep -Fq 'capture clock: clk @ 10.168 ns' "${LOG}/testcases_sdc_features_clock_sense.dofile.log"
grep -Fq 'capture clock: clk @ -9.832 ns' "${LOG}/testcases_sdc_features_clock_sense.dofile.log"
grep -Fq 'setup : WNS    8.668 ns' "${LOG}/testcases_sdc_features_clock_sense.dofile.log"
grep -Fq 'hold  : WNS    9.831 ns' "${LOG}/testcases_sdc_features_clock_sense.dofile.log"
# -positive：捕捉沿回到 20.168，slack 差一个半周期（10 ns）。
grep -Fq 'capture clock: clk @ 20.168 ns' "${LOG}/testcases_sdc_features_clock_sense_positive.dofile.log"
grep -Fq 'setup : WNS   18.668 ns' "${LOG}/testcases_sdc_features_clock_sense_positive.dofile.log"
# -stop_propagation：时钟停在反相器输出，后面那级网络不再算插入延迟，
# 下面的 FF 没有时钟可用（端点算未约束）。
grep -Fq '时钟树：网络 3 根，算出插入延迟的 2 根，最深 1 级缓冲，最大插入延迟 0.035 ns' \
    "${LOG}/testcases_sdc_features_clock_sense_stop.dofile.log"
grep -Fq '寄存器 1 个   端点 2 个   未约束 2 个' \
    "${LOG}/testcases_sdc_features_clock_sense_stop.dofile.log"
for sense_case in clock_sense clock_sense_positive clock_sense_stop; do
    if grep -Fq 'Warning: sdc' "${LOG}/testcases_sdc_features_${sense_case}.dofile.log"; then
        echo "clock_sense: ${sense_case} 产生了 sdc 告警" >&2
        exit 1
    fi
done

# 移植自 OpenTimer 的 benchmark 用例（MIT 许可）：逐边沿 I/O 约束 + 虚拟时钟。
grep -Fq 'setup : WNS   -0.018 ns' "${LOG}/testcases_opentimer_c17_c17.dofile.log"
grep -Fq 'hold  : WNS    0.004 ns' "${LOG}/testcases_opentimer_c17_c17.dofile.log"
grep -Fq 'setup : WNS   -0.057 ns' "${LOG}/testcases_opentimer_simple_simple.dofile.log"
grep -Fq 'hold  : WNS    0.038 ns' "${LOG}/testcases_opentimer_simple_simple.dofile.log"
grep -Fq 'setup : WNS   -0.378 ns' "${LOG}/testcases_opentimer_s27_s27.dofile.log"
grep -Fq 'hold  : WNS   -0.230 ns' "${LOG}/testcases_opentimer_s27_s27.dofile.log"
grep -Fq '寄存器 3 个   端点 4 个' "${LOG}/testcases_opentimer_s27_s27.dofile.log"

# 自己的用例一律写 SDC 1.8 语法：standard_ports 用显式端口列表表达"除时钟外的输入"。
grep -Fq 'setup : WNS   17.496 ns' "${LOG}/testcases_sdc_features_standard_ports.dofile.log"
# 也可以用 Tcl 层集合运算拼出同一组对象（remove_from_collection），结果必须一致。
grep -Fq 'setup : WNS   17.496 ns' "${LOG}/testcases_sdc_features_collection_ops.dofile.log"
grep -Fq 'hold  : WNS    0.465 ns' "${LOG}/testcases_sdc_features_collection_ops.dofile.log"
if grep -Fq 'Warning: sdc' "${LOG}/testcases_sdc_features_collection_ops.dofile.log"; then
    echo 'collection_ops: 集合运算不应产生 sdc 告警' >&2
    exit 1
fi
if grep -Fq 'Warning: sdc' "${LOG}/testcases_sdc_features_standard_ports.dofile.log"; then
    echo 'standard_ports: 标准写法不应该产生任何 sdc 告警' >&2
    exit 1
fi
# 只看命令本身，注释里提到方言名不算（去掉 # 之后的内容再扫）。
# 除了别家工具的拼写，set_clock_uncertainty 的"值跟在 -setup/-hold 后面"也算方言。
vDialect="$(find "${ROOT}/testcases" -name '*.sdc' -exec sed 's/#.*//' {} + \
            | grep -E -e '-no_clocks' -e '-quiet' -e 'unset_path_exceptions' -e '-reset_path' \
                      -e 'set_clock_uncertainty.*-(setup|hold)[[:space:]]+[0-9.]+.*-(setup|hold)[[:space:]]+[0-9.]+' \
            || true)"
if [[ -n "${vDialect}" ]]; then
    echo 'testcases 里不应出现 SDC 1.8 之外的方言写法：' >&2
    echo "${vDialect}" >&2
    exit 1
fi

# 手册里有、msta 没建模的选项：报告后整条命令作废。
grep -Fq 'collection option -expression is not modeled; the command is skipped' \
    "${LOG}/testcases_sdc_features_sdc_conformance.dofile.log"
grep -Fq 'set_multicycle_path: option "-start" is not modeled; constraint rejected' \
    "${LOG}/testcases_sdc_features_sdc_conformance.dofile.log"
# "-max 0.5 -min 0.2" 不是 SDC 1.8 语法（-min/-max 只是开关，值只有一个），整条作废。
grep -Fq 'set_ideal_latency: "-min 0.2" is not SDC 1.8 syntax (-min takes no value); constraint rejected' \
    "${LOG}/testcases_sdc_features_sdc_conformance.dofile.log"
# 同一根网络上的第二个 create_clock 必须写 -add（不写是替换，msta 不做替换）。
grep -Fq 'create_clock: the source already has a clock (use -add for another one); constraint rejected' \
    "${LOG}/testcases_sdc_features_sdc_conformance.dofile.log"
grep -Fq 'setup : WNS   17.737 ns' "${LOG}/testcases_sdc_features_sdc_conformance.dofile.log"

# 命令解析规则（docs/sdc.md）：写错的约束整条作废、告警并计入忽略数，
# 其余约束照常生效。sdc_rules 在 generated.sdc 之后追加了 12 条写错的约束。
RULES="${LOG}/testcases_sdc_features_sdc_rules.dofile.log"
for rule in 'set_input_delay: option "-foo" is not modeled; constraint rejected' \
            'create_clock: option "-name" needs a value; constraint rejected' \
            'set_output_delay: option "-max" is given more than once; constraint rejected' \
            'set_multicycle_path: cycle count must be positive (got -2); constraint rejected' \
            'set_multicycle_path: cycle count must be a positive integer (got 2.5); constraint rejected' \
            'set_input_delay: "-min 1.0" is not SDC 1.8 syntax (-min takes no value); constraint rejected' \
            'set_load: unexpected value "0.02"; constraint rejected' \
            'set_case_analysis: value "2" must be 0, 1, zero or one; constraint rejected' \
            'set_case_analysis: value "rising" is not modeled (only 0, 1, zero and one); constraint rejected' \
            'set_load: value must not be negative (got -0.05); constraint rejected' \
            'set_timing_derate: object collection [get_clocks] is empty; constraint rejected' \
            'set_timing_derate: object collection [get_cells -of_objects] is empty; constraint rejected' \
            '12 command(s) were not modeled and were ignored'; do
    grep -Fq -- "${rule}" "${RULES}" || { echo "sdc_rules: 缺少告警：${rule}" >&2; exit 1; }
done
# 负数是值：-2 不应被当成不认识的选项。
if grep -Fq 'option "-2"' "${RULES}"; then
    echo 'sdc_rules: 负数被当成了选项' >&2
    exit 1
fi
# 作废的约束不留下任何影响：时序结果与只读 generated.sdc 的逐行相同
# （空集合的 derate 若退化成全局系数，到达时间会变）。
if ! diff <(grep -E 'WNS|arrival time|capture clock' "${LOG}/testcases_sdc_features_generated.dofile.log") \
          <(grep -E 'WNS|arrival time|capture clock' "${RULES}") > /dev/null; then
    echo 'sdc_rules: 作废的约束影响了时序结果' >&2
    exit 1
fi

# 兼容层（读别家工具生成的 SDC 才用得到）单独在这里守一条：
# all_inputs -no_clocks 应该被接受，并告警说明它不是 SDC 1.8 语法。
printf 'set_input_delay 1 -clock c [all_inputs -no_clocks]\n' > "${LOG}/compat_dialect.sdc"
tclsh "${ROOT}/scripts/sdc_bridge.tcl" "${LOG}/compat_dialect.sdc" "${LOG}/compat_dialect.json" \
    2> "${LOG}/compat_dialect.log"
grep -Fq -- '-no_clocks is not SDC 1.8 syntax; honored as a compatibility extension' \
    "${LOG}/compat_dialect.log"
grep -Fq 'Ainputs' "${LOG}/compat_dialect.json"
grep -Fq 'no_clocks' "${LOG}/compat_dialect.json"

# 方言写法（C 侧）：有的工具把 -no_propagate 写成 -no_propagation，这条认但告警。
printf 'set_ideal_network -no_propagation [get_pins ct2/Y]\n' > "${LOG}/compat_ideal_network.sdc"
( cd "${ROOT}/testcases/sdc_features" && "${ROOT}/build/msta" -q -c \
    "read_liberty ../sta_compare/lib/osu018_stdcells.lib; read_verilog ideal_network.v; \
     current_design ideal_demo; read_sdc ideal_network.sdc; \
     read_sdc ${LOG}/compat_ideal_network.sdc; report_clock_tree" ) \
    > "${LOG}/compat_ideal_network.log" 2>&1
grep -Fq 'set_ideal_network: -no_propagation is not SDC 1.8 syntax; honored as -no_propagate' \
    "${LOG}/compat_ideal_network.log"
# 认下来之后行为与 -no_propagate 一致（0.442 ns）；如果被丢掉会是整棵树的 0.584 ns。
grep -Fq '最大插入延迟 0.442 ns' "${LOG}/compat_ideal_network.log"

# 从 RTL 综合出的 sky130 网表有 39 个异步复位 FF，hold 角由复位释放路径上的
# removal 检查主导。
grep -Fq 'removal path' "${LOG}/testcases_synth_sky130_pipe_demo.dofile.log"
grep -Fq '寄存器 39 个   端点 125 个   未约束 0 个' "${LOG}/testcases_synth_sky130_pipe_demo.dofile.log"

# 真实设计：核对两个工具对比时用的展平规模。
# group_path：命中的路径归到命名组，报告按组出 WNS/TNS；分组不改变 slack。
grep -Fq 'path group   : reg_grp' "${LOG}/testcases_sdc_features_group_path.dofile.log"
grep -Fq 'path group   : out_grp' "${LOG}/testcases_sdc_features_group_path.dofile.log"
grep -Eq '^reg_grp[[:space:]]+2\.00[[:space:]]+1[[:space:]]+44\.754' \
    "${LOG}/testcases_sdc_features_group_path.dofile.log"
grep -Eq '^out_grp[[:space:]]+1\.00[[:space:]]+1[[:space:]]+19\.707[[:space:]]+0\.000[[:space:]]+1[[:space:]]+-9\.781' \
    "${LOG}/testcases_sdc_features_group_path.dofile.log"
# 带 group_path 的那一版 slack 和不带的基准一致（分组只换报告的组织方式）。
grep -Fq 'setup : WNS   19.707 ns' "${LOG}/testcases_sdc_features_group_path.dofile.log"
grep -Fq 'hold  : WNS   -9.781 ns' "${LOG}/testcases_sdc_features_group_path.dofile.log"
# -default 组收命名组之外的所有路径。
grep -Fq 'path group   : **default**' "${LOG}/testcases_sdc_features_group_path_default.dofile.log"
grep -Eq '^\*\*default\*\*[[:space:]]+1\.00[[:space:]]+1[[:space:]]+19\.707' \
    "${LOG}/testcases_sdc_features_group_path_default.dofile.log"
# -from/-through 也参与分组匹配。
grep -Fq 'path group   : in_grp' "${LOG}/testcases_sdc_features_group_path_through.dofile.log"
grep -Eq '^in_grp[[:space:]]+1\.00[[:space:]]+1[[:space:]]+44\.754' \
    "${LOG}/testcases_sdc_features_group_path_through.dofile.log"
# 和路径例外一起用时，分组跟着"最终获胜的那条路径"走（次优路径仍在 u_mux 上）。
grep -Fq 'path group   : mux_grp' "${LOG}/testcases_sdc_features_group_path_exception.dofile.log"
grep -Fq 'setup : WNS   15.684 ns' "${LOG}/testcases_sdc_features_group_path_exception.dofile.log"
grep -Eq '^mux_grp[[:space:]]+1\.00[[:space:]]+2[[:space:]]+15\.684' \
    "${LOG}/testcases_sdc_features_group_path_exception.dofile.log"
for gp in group_path group_path_default group_path_through group_path_exception; do
    if grep -Fq 'Warning: sdc' "${LOG}/testcases_sdc_features_${gp}.dofile.log"; then
        echo "group_path: ${gp} 产生了 sdc 告警" >&2
        exit 1
    fi
done

# set_clock_gating_check：门控单元的使能脚相对时钟脚做 setup/hold 检查，
# 值优先取 SDC，其次取库里使能脚上的约束弧；最差的那条计入 WNS/TNS。
grep -Fq '时钟门控 : 1 条检查   setup 最差 7.600 ns（u_icg/GATE，检查值 0.400）   hold 最差 0.400 ns（u_icg/GATE，检查值 0.100）' \
    "${LOG}/testcases_sdc_features_gating.dofile.log"
grep -Fq 'setup : WNS    7.600 ns' "${LOG}/testcases_sdc_features_gating.dofile.log"
grep -Fq 'hold  : WNS    0.400 ns' "${LOG}/testcases_sdc_features_gating.dofile.log"
# 不写命令时用库里的约束弧（u_icg 的 GATE setup 0.130 / hold -0.060）。
grep -Fq '时钟门控 : 1 条检查   setup 最差 7.870 ns（u_icg/GATE，检查值 0.130）   hold 最差 0.560 ns（u_icg/GATE，检查值 -0.060）' \
    "${LOG}/testcases_sdc_features_gating_lib.dofile.log"
# 真实设计：1144 个 ICG 都被识别出来，setup 最差这条远于关键路径，所以 WNS 不变。
grep -Fq '时钟门控 : 1144 条检查' "${LOG}/testcases_eth_sky130_eth.dofile.log"
grep -Fq 'setup : WNS    3.897 ns' "${LOG}/testcases_eth_sky130_eth.dofile.log"

# 锁存器：默认按"关闭沿减 setup"检查（即借满整个开窗），
# set_max_time_borrow 0.5 换成"开沿 + 0.5"，两边都是 -1.5。
grep -Fq 'setup path (max corner, to latch)' "${LOG}/testcases_sdc_features_latch.dofile.log"
grep -Fq 'setup : WNS    2.805 ns' "${LOG}/testcases_sdc_features_latch.dofile.log"
grep -Fq '寄存器 0 个   锁存器 1 个   端点 2 个' "${LOG}/testcases_sdc_features_latch.dofile.log"
grep -Fq 'setup : WNS   -1.500 ns' "${LOG}/testcases_sdc_features_latch_borrow.dofile.log"
grep -Fq '(锁存器：开沿起算，max_time_borrow 0.500 ns 取代上面的关闭沿要求)' \
    "${LOG}/testcases_sdc_features_latch_borrow.dofile.log"

# get_libs / get_lib_cells / get_lib_pins：库对象查询，结果就是名字。
grep -Fq 'LIBS : osu018_stdcells' "${LOG}/testcases_sdc_features_lib_query.dofile.log"
grep -Fq 'CELL : NAND2X1' "${LOG}/testcases_sdc_features_lib_query.dofile.log"
grep -Fq 'QUAL : osu018_stdcells/NAND2X1' "${LOG}/testcases_sdc_features_lib_query.dofile.log"
grep -Fq 'PINS : A B Y' "${LOG}/testcases_sdc_features_lib_query.dofile.log"
# 查询结果喂给 set_driving_cell：到达时间 5.000 -> 5.064，与直接写名字的版本逐位一致。
grep -Eq '^  inp1[[:space:]]+5\.064[[:space:]]+5\.064' \
    "${LOG}/testcases_sdc_features_lib_query.dofile.log"
grep -Eq '^  inp1[[:space:]]+5\.064[[:space:]]+5\.064' \
    "${LOG}/testcases_sdc_features_lib_query_literal.dofile.log"
grep -Fq 'setup : WNS   19.707 ns' "${LOG}/testcases_sdc_features_lib_query.dofile.log"
grep -Fq 'hold  : WNS   -9.781 ns' "${LOG}/testcases_sdc_features_lib_query.dofile.log"

# all_registers -clock / -rise_clock / -fall_clock：按时钟树（含有效沿）挑寄存器。
# clkB 域有 2 个寄存器，
# 切掉它们出发的路径后正好 2 个端点被排除；
# clkA 上只有 c1 是真正的下降沿（c2 挂在反相时钟上、有效沿是上升），
# 所以 -fall_clock clkA 只切掉 c1 -> q3 一条，c2 -> q4 仍然报出来。
grep -Fq '未约束 0 个   路径例外排除 2 个' "${LOG}/testcases_sdc_features_all_registers_clock.dofile.log"
grep -Fq 'setup : WNS    3.812 ns' "${LOG}/testcases_sdc_features_all_registers_clock.dofile.log"
grep -Fq '未约束 3 个   路径例外排除 1 个' "${LOG}/testcases_sdc_features_all_registers_clock_edge.dofile.log"
grep -Fq 'startpoint : c2' "${LOG}/testcases_sdc_features_all_registers_clock_edge.dofile.log"

# set_timing_derate 分对象：分对象的值覆盖全局（不是相乘）。
# 基准（不带 derate）是 f1/Q 0.165、u2/Y 0.060、u3/Y 0.068、f1 的检查值 -0.188，
# 所以全局 1.1 + u2 上 1.2 + f1 的 -cell_check 1.2 应当得到下面这一组数。
grep -Eq '^  f1/Q[[:space:]]+0\.181' "${LOG}/testcases_sdc_features_timing_derate_obj.dofile.log"
grep -Eq '^  u2/Y[[:space:]]+0\.072' "${LOG}/testcases_sdc_features_timing_derate_obj.dofile.log"
grep -Eq '^  u3/Y[[:space:]]+0\.075' "${LOG}/testcases_sdc_features_timing_derate_obj.dofile.log"
grep -Fq '  - setup check (from lib)                        -0.226' \
    "${LOG}/testcases_sdc_features_timing_derate_obj.dofile.log"
# -rise 只管上升沿：u2 上升沿 ×1.5（0.060 -> 0.089），下降沿仍是全局 1.1。
grep -Eq '^  u2/Y[[:space:]]+0\.089' "${LOG}/testcases_sdc_features_timing_derate_obj_edge.dofile.log"

# set_load -subtract_pin_load：注解值就是总负载（脚电容不再另加），
# 与"把注解值减掉脚电容"的写法结果相同。
grep -Eq '^  f1/Q[[:space:]]+0\.210' "${LOG}/testcases_sdc_features_load_subtract.dofile.log"
grep -Eq '^  f1/Q[[:space:]]+0\.210' "${LOG}/testcases_sdc_features_load_subtract_literal.dofile.log"
grep -Fq 'data arrival time                            0.352' \
    "${LOG}/testcases_sdc_features_load_subtract.dofile.log"

# get_* -of_objects：按对象关系推集合（父对象也可以是 all_* 这类整体集合）。
# 三份用例分别切掉一个端点。
grep -Fq '未约束 0 个   路径例外排除 1 个' "${LOG}/testcases_sdc_features_get_of_objects.dofile.log"
grep -Fq 'setup : WNS   19.707 ns' "${LOG}/testcases_sdc_features_get_of_objects.dofile.log"
grep -Fq '未约束 0 个   路径例外排除 1 个' "${LOG}/testcases_sdc_features_get_of_objects_net.dofile.log"
grep -Fq 'setup : WNS   44.754 ns' "${LOG}/testcases_sdc_features_get_of_objects_net.dofile.log"
grep -Fq '未约束 0 个   路径例外排除 1 个' "${LOG}/testcases_sdc_features_get_of_objects_all.dofile.log"
grep -Fq 'setup : WNS   19.707 ns' "${LOG}/testcases_sdc_features_get_of_objects_all.dofile.log"

# get_* -filter：属性来自 C 侧导出的设计索引，表达式用 Tcl 的 expr 求值
# （属性名补 $、裸词加引号、=~ 翻成通配匹配）。
grep -Fq '未约束 0 个   路径例外排除 1 个' "${LOG}/testcases_sdc_features_get_filter.dofile.log"
grep -Fq 'setup : WNS   44.754 ns' "${LOG}/testcases_sdc_features_get_filter.dofile.log"
grep -Fq '未约束 0 个   路径例外排除 1 个' "${LOG}/testcases_sdc_features_get_filter_pins.dofile.log"
grep -Fq 'setup : WNS   19.707 ns' "${LOG}/testcases_sdc_features_get_filter_pins.dofile.log"

grep -Fq 'instances after flatten: 25142' "${LOG}/testcases_eth_sky130_eth.dofile.log"
grep -Fq '寄存器 10543 个   端点 13133 个' "${LOG}/testcases_eth_sky130_eth.dofile.log"
grep -Fq '时钟树：网络 1270 根' "${LOG}/testcases_eth_sky130_eth.dofile.log"
grep -Fq 'instances after flatten: 23589' "${LOG}/testcases_e902_sky130_e902.dofile.log"
grep -Fq '寄存器 2782 个   端点 6724 个' "${LOG}/testcases_e902_sky130_e902.dofile.log"
echo '==> SDC semantic checks passed'
