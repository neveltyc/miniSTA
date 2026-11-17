#!/usr/bin/env python3
"""Phase 2 modularity & teaching-diagnostics regression suite (5 groups)."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
MSTA = (Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build" / "msta").resolve()
SKY130_LIB = ROOT / "testcases" / "lib" / "sky130.lib"


def run_msta(cwd, script_text, name="case.dofile"):
    df = Path(cwd) / name
    df.write_text(script_text, encoding="utf-8")
    res = subprocess.run(
        [str(MSTA), "-q", str(df)],
        cwd=str(cwd),
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=60,
    )
    assert res.returncode == 0, f"msta exited {res.returncode} on {name}:\n{res.stdout}"
    return res.stdout


def test_group1_report_checks_flags():
    """Group 1: real pin names on cells + -tran / -cap / -clock_path expansion."""
    # 1a: standard input port formatting compatibility on existing testcase
    out_sdc = (ROOT / "build" / "testcases_sdc_features_base.dofile.log")
    if out_sdc.exists():
        txt = out_sdc.read_text(encoding="utf-8")
        assert "(input port )" in txt
        assert "pin -" not in txt, "expected real input pin names instead of 'pin -'"

    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        (tmp / "top.v").write_text(
            """
module top(input clk, input d, output q);
  wire c1, c2, cq1, n1, n2, n3, n4, n5;
  sky130_fd_sc_hd__buf_1 cb_root(.A(clk), .X(c1));
  sky130_fd_sc_hd__buf_1 cb_lch (.A(c1),  .X(c2));
  sky130_fd_sc_hd__buf_1 cb_cap (.A(c1),  .X(cq1));
  sky130_fd_sc_hd__dfxtp_1 ff1 (.CLK(c2),  .D(d),  .Q(n1));
  sky130_fd_sc_hd__inv_1   u1  (.A(n1),    .Y(n2));
  sky130_fd_sc_hd__buf_1   u2  (.A(n2),    .X(n3));
  sky130_fd_sc_hd__buf_1   u3  (.A(n3),    .X(n4));
  sky130_fd_sc_hd__buf_1   u4  (.A(n4),    .X(n5));
  sky130_fd_sc_hd__dfxtp_1 ff2 (.CLK(cq1), .D(n5), .Q(q));
endmodule
""",
            encoding="utf-8",
        )
        (tmp / "top.sdc").write_text(
            """
create_clock -period 5.0 [get_ports clk]
set_propagated_clock [all_clocks]
set_input_delay 0.1 -clock clk [get_ports d]
set_output_delay 0.1 -clock clk [get_ports q]
""",
            encoding="utf-8",
        )
        out = run_msta(
            tmp,
            f"""
read_liberty {SKY130_LIB}
read_verilog {tmp / 'top.v'}
current_design top
read_sdc {tmp / 'top.sdc'}
report_checks -setup -max_paths 3
report_checks -setup -tran -cap -clock_path -max_paths 1
""",
        )
        blocks = out.split("===== setup path")
        def_rpt = "".join(blocks[1:4])
        ext_rpt = blocks[-1]

        assert "pin -" not in def_rpt, def_rpt
        assert "sky130_fd_sc_hd__dfxtp_1  pin CLK" in def_rpt, def_rpt
        assert "sky130_fd_sc_hd__inv_1  pin A" in def_rpt, def_rpt
        assert "sky130_fd_sc_hd__buf_1  pin A" in def_rpt, def_rpt

        assert "tran(ns)" in ext_rpt and "cap(fF)" in ext_rpt, ext_rpt
        assert "cb_root/X" in ext_rpt and "cb_lch/X" in ext_rpt, ext_rpt
        assert "cb_cap/X" in ext_rpt, ext_rpt
        assert "(launch clock)" in ext_rpt and "(capture clock)" in ext_rpt, ext_rpt

        # Startpoint ff1/Q inc(ns) equals arr(ff1/Q) - arr(cb_lch/X) (pure clk-to-Q)
        m_cb = re.search(r"cb_lch/X\s+([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)", ext_rpt)
        m_ff = re.search(r"ff1/Q\s+([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)", ext_rpt)
        assert m_cb and m_ff, ext_rpt
        cb_arr = float(m_cb.group(2))
        ff_inc, ff_arr = float(m_ff.group(1)), float(m_ff.group(2))
        assert cb_arr > 0.05, ext_rpt
        assert abs(ff_inc - (ff_arr - cb_arr)) <= 0.002, (ff_inc, ff_arr, cb_arr)
        assert float(m_ff.group(3)) > 0.0 and float(m_ff.group(4)) > 0.0, ext_rpt
    print("[PASS] Group 1: report_checks real pin names & -tran/-cap/-clock_path")


def verify_explain_block(block):
    m_row = re.search(
        r"axis\[0\] row\s*:\s*\S+\s*=\s*([-\d.]+)\s+\S+\s*->\s*bracket \[([-\d.]+),\s*([-\d.]+)\],\s*iRow\s*=\s*(\d+),\s*wRow\s*=\s*([-\d.]+)",
        block,
    )
    m_col = re.search(
        r"axis\[1\] col\s*:\s*\S+\s*=\s*([-\d.]+)\s+\S+\s*->\s*bracket \[([-\d.]+),\s*([-\d.]+)\],\s*iCol\s*=\s*(\d+),\s*wCol\s*=\s*([-\d.]+)",
        block,
    )
    m_grid = re.search(
        r"grid 2x2\s*:\s*V00\s*=\s*([-\d.]+),\s*V01\s*=\s*([-\d.]+),\s*V10\s*=\s*([-\d.]+),\s*V11\s*=\s*([-\d.]+)",
        block,
    )
    m_raw = re.search(r"nldm raw\s*:\s*([-\d.]+)\s+ps", block)
    assert m_row and m_col and m_grid and m_raw, block

    ri, r0, r1, _, wr = float(m_row.group(1)), float(m_row.group(2)), float(m_row.group(3)), int(m_row.group(4)), float(m_row.group(5))
    ci, c0, c1, _, wc = float(m_col.group(1)), float(m_col.group(2)), float(m_col.group(3)), int(m_col.group(4)), float(m_col.group(5))
    v00, v01, v10, v11 = map(float, m_grid.groups())
    raw = float(m_raw.group(1))

    if r1 > r0:
        assert abs((ri - r0) / (r1 - r0) - wr) < 1e-4, (ri, r0, r1, wr)
    if c1 > c0:
        assert abs((ci - c0) / (c1 - c0) - wc) < 1e-4, (ci, c0, c1, wc)

    calc = (v00 * (1.0 - wc) + v01 * wc) * (1.0 - wr) + (v10 * (1.0 - wc) + v11 * wc) * wr
    assert abs(calc - raw) < 1e-3, f"bilinear mismatch: calc={calc}, raw={raw}\n{block}"

    m_sc = re.search(r"ocv_derate\s*=\s*([-\d.]+),\s*aocv_factor\s*=\s*([-\d.]+)", block)
    m_fin = re.search(r"final delay\s*:\s*([-\d.]+)\s+ps\s*\(([-\d.]+)\s+ns,\s*report\s+([-\d.]+)\s+ns\)", block)
    if m_sc and m_fin:
        derate, aocv = float(m_sc.group(1)), float(m_sc.group(2))
        fin_ps, fin_ns = float(m_fin.group(1)), float(m_fin.group(2))
        assert abs(raw * derate * aocv - fin_ps) < 1e-3, (raw, derate, aocv, fin_ps)
        assert abs(fin_ps / 1000.0 - fin_ns) < 1e-6, (fin_ps, fin_ns)
        return raw, derate, fin_ps, float(m_fin.group(3))
    return raw, 1.0, raw, raw / 1000.0


def test_group2_explain_arc_hand_math():
    """Group 2: explain_arc NLDM bilinear interpolation + OCV derate + report_checks agreement."""
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        (tmp / "top.v").write_text(
            """
module top(input clk, input d, output q);
  wire n1, n2, n3;
  sky130_fd_sc_hd__dfxtp_1 ff1 (.CLK(clk), .D(d),  .Q(n1));
  sky130_fd_sc_hd__inv_1   u1  (.A(n1),    .Y(n2));
  sky130_fd_sc_hd__buf_2   u2  (.A(n2),    .X(n3));
  sky130_fd_sc_hd__dfxtp_1 ff2 (.CLK(clk), .D(n3), .Q(q));
endmodule
""",
            encoding="utf-8",
        )
        (tmp / "top.sdc").write_text(
            """
create_clock -period 10.0 [get_ports clk]
set_input_delay 0.5 -clock clk [get_ports d]
set_output_delay 0.05 -clock clk [get_ports q]
set_timing_derate -late -data 1.15
set_timing_derate -early -data 0.85
""",
            encoding="utf-8",
        )
        out = run_msta(
            tmp,
            f"""
read_liberty {SKY130_LIB}
read_verilog {tmp / 'top.v'}
current_design top
read_sdc {tmp / 'top.sdc'}
report_checks -setup -max_paths 3
report_checks -hold -max_paths 3
explain_arc u1 A Y
explain_arc u1 A Y -min
explain_arc ff2 CLK D
""",
        )
        exp_max = out.split("===== explain_arc: u1")[1].split("===== explain_arc:")[0]
        exp_min = out.split("===== explain_arc: u1")[2].split("===== explain_arc:")[0]
        exp_chk = out.split("===== explain_arc: ff2")[1]

        assert "[max corner]" in exp_max and "[min corner]" in exp_min
        assert "u2/A" in exp_max, exp_max

        # Parse both output rise delay and output fall delay blocks
        verified_blocks = 0
        max_delays = {}
        min_delays = {}
        for tag, txt, dst in (("max", exp_max, max_delays), ("min", exp_min, min_delays)):
            for part in txt.split("  --- ")[1:]:
                hdr = part.splitlines()[0]
                _, der, fin_ps, rpt_ns = verify_explain_block(part)
                verified_blocks += 1
                if "delay" in hdr:
                    edge = "rise" if "output rise delay" in hdr else "fall"
                    assert abs(der - (1.15 if tag == "max" else 0.85)) < 1e-6, (tag, der)
                    dst[edge] = (fin_ps, rpt_ns)
        assert verified_blocks >= 8, verified_blocks

        # Cross-check constraint check table in explain_arc ff2 CLK D
        assert "constraint rise (data rise)" in exp_chk and "constraint fall (data fall)" in exp_chk
        m_chk = re.search(r"final check\s*:\s*([-\d.]+)\s+ps", exp_chk)
        assert m_chk, exp_chk

        # Compare u1/Y inc(ns) in report_checks against explain_arc final delay
        setup_blk = next(
            b for b in out.split("===== setup path")[1:] if "endpoint   : ff2\n" in b
        ).split("===== hold path")[0]
        m_se = re.search(r"data edge\s*:\s*(rise|fall)", setup_blk)
        m_u1 = re.search(r"u1/Y\s+([-\d.]+)\s+([-\d.]+)", setup_blk)
        assert m_se and m_u1, setup_blk
        # u1 is an inverter right before buf u2 before ff2/D: output edge of u1 == data edge at ff2/D
        u1_edge = m_se.group(1)
        assert abs(float(m_u1.group(1)) - max_delays[u1_edge][1]) <= 0.001, (
            m_u1.group(1),
            max_delays[u1_edge],
        )
    print("[PASS] Group 2: explain_arc NLDM bilinear math + OCV derate + report_checks match")


def test_group3_backward_rat_and_multiclock_drc():
    """Group 3: backward RAT theorem on critical path & multi-clock DRC coverage."""
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        # Part A: reconvergent combinational cone with violation (period 0.5 ns)
        (tmp / "rat.v").write_text(
            """
module rat(input clk, input d1, input d2, output q);
  wire n_q1, n_s1, n_s2, n_fast, n_end;
  sky130_fd_sc_hd__dfxtp_1  ff_s1 (.CLK(clk), .D(d1), .Q(n_q1));
  sky130_fd_sc_hd__dfxtp_1  ff_s2 (.CLK(clk), .D(d2), .Q(n_fast));
  sky130_fd_sc_hd__buf_1    ub1   (.A(n_q1), .X(n_s1));
  sky130_fd_sc_hd__buf_1    ub2   (.A(n_s1), .X(n_s2));
  sky130_fd_sc_hd__nand2_1  ujoin (.A(n_s2), .B(n_fast), .Y(n_end));
  sky130_fd_sc_hd__dfxtp_1  ff_d  (.CLK(clk), .D(n_end), .Q(q));
endmodule
""",
            encoding="utf-8",
        )
        (tmp / "rat.sdc").write_text(
            """
create_clock -period 0.5 [get_ports clk]
set_input_delay 0.1 -clock clk [get_ports {d1 d2}]
set_output_delay 0.1 -clock clk [get_ports q]
""",
            encoding="utf-8",
        )
        out = run_msta(
            tmp,
            f"""
read_liberty {SKY130_LIB}
read_verilog {tmp / 'rat.v'}
current_design rat
read_sdc {tmp / 'rat.sdc'}
report_checks -setup -max_paths 1
report_net n_q1
report_net n_s1
report_net n_s2
report_net n_end
report_net n_fast
dump_graph -o {tmp / 'rat.dot'}
dump_graph -o {tmp / 'rat_trunc.dot'} -max_nodes 4
""",
        )
        m_wns = re.search(r"setup\s*:\s*WNS\s+(-?\d+\.\d+)\s+ns", out)
        assert m_wns, out
        wns = float(m_wns.group(1))
        assert wns < 0.0, f"expected negative WNS to test critical violation styling, got {wns}"

        expected_lvls = {"n_q1": 0, "n_s1": 1, "n_s2": 2, "n_end": 3, "n_fast": 0}
        slacks = {}
        for net, lvl in expected_lvls.items():
            blk = out.split(f"===== report_net: {net} ")[1].split("===== ")[0]
            m_lvl = re.search(r"logic level\s*:\s*(\d+)", blk)
            m_slk = re.search(r"setup slack\s*:\s*([-\d.]+)\s+ns", blk)
            assert m_lvl and int(m_lvl.group(1)) == lvl, (net, lvl, blk)
            assert m_slk, (net, blk)
            slacks[net] = float(m_slk.group(1))

        for crit_net in ("n_q1", "n_s1", "n_s2", "n_end"):
            assert abs(slacks[crit_net] - wns) <= 0.001, (crit_net, slacks[crit_net], wns)
        assert slacks["n_fast"] > wns + 0.05, (slacks["n_fast"], wns)

        # Group 4 check on generated DOT files right here
        dot = (tmp / "rat.dot").read_text(encoding="utf-8")
        dot_trunc = (tmp / "rat_trunc.dot").read_text(encoding="utf-8")
        assert dot.startswith("digraph msta_timing_graph {")
        assert "rankdir=LR;" in dot
        assert "truncated:" not in dot, dot
        assert "color=red, penwidth=2.0" in dot, dot
        assert "shape=diamond, color=blue" in dot, dot
        assert "shape=box, color=green4" in dot, dot
        assert "ujoin:A->Y" in dot and "ujoin:B->Y" in dot, dot
        assert "/* truncated: showing 4 of" in dot_trunc, dot_trunc

        # Part B: 2-clock design where clk0 (first clock) violates set_max_transition, clk1 (last clock) is clean
        (tmp / "mc.v").write_text(
            """
module mc(input clk0, input clk1, input d0, input d1, output q0, output q1);
  wire n0_q, n0_w, n1_q, n1_w;
  sky130_fd_sc_hd__dfxtp_1 f0_a (.CLK(clk0), .D(d0),   .Q(n0_q));
  sky130_fd_sc_hd__buf_1   b0   (.A(n0_q),  .X(n0_w));
  sky130_fd_sc_hd__dfxtp_1 f0_b (.CLK(clk0), .D(n0_w), .Q(q0));

  sky130_fd_sc_hd__dfxtp_1 f1_a (.CLK(clk1), .D(d1),   .Q(n1_q));
  sky130_fd_sc_hd__buf_1   b1   (.A(n1_q),  .X(n1_w));
  sky130_fd_sc_hd__dfxtp_1 f1_b (.CLK(clk1), .D(n1_w), .Q(q1));
endmodule
""",
            encoding="utf-8",
        )
        (tmp / "mc.sdc").write_text(
            """
create_clock -name clk0 -period 10.0 [get_ports clk0]
create_clock -name clk1 -period 10.0 [get_ports clk1]
set_input_delay 0.5 -clock clk0 [get_ports d0]
set_input_delay 0.5 -clock clk1 [get_ports d1]
set_output_delay 0.5 -clock clk0 [get_ports q0]
set_output_delay 0.5 -clock clk1 [get_ports q1]
set_load 0.5 [get_nets n0_w]
set_max_transition 0.2 [current_design]
""",
            encoding="utf-8",
        )
        mc_out = run_msta(
            tmp,
            f"""
read_liberty {SKY130_LIB}
read_verilog {tmp / 'mc.v'}
current_design mc
read_sdc {tmp / 'mc.sdc'}
report_summary
report_net n0_w
report_net n1_w
""",
        )
        m_drc = re.search(r"max_transition 违例 (\d+) 处", mc_out)
        assert m_drc and int(m_drc.group(1)) >= 1, mc_out
        assert "N/A" not in mc_out.split("===== report_net: n0_w ")[1].split("max     rise")[1].splitlines()[0], mc_out
    print("[PASS] Group 3: backward RAT critical-path theorem & multi-clock DRC")
    print("[PASS] Group 4: dump_graph DOT export & -max_nodes truncation")


def test_group5_phase2_review_fixes():
    """Group 5: Phase 2 code review fixes (both edges RAT, genclk -clock_path, dump_graph pruning, -max_paths err, data_check RAT)."""
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        # 5a: Generated clock -clock_path convergence + seq constraint explain_arc + missing -max_paths error
        (tmp / "gen.v").write_text(
            """
module gen(input clk, input d, output q);
  wire div_clk, c1, n1, n2;
  sky130_fd_sc_hd__buf_1   cb_root(.A(clk), .X(c1));
  sky130_fd_sc_hd__dfxtp_1 u_div  (.CLK(c1), .D(d), .Q(div_clk));
  sky130_fd_sc_hd__dfxtp_1 ff1    (.CLK(div_clk), .D(d), .Q(n1));
  sky130_fd_sc_hd__buf_1   u1     (.A(n1), .X(n2));
  sky130_fd_sc_hd__dfxtp_1 ff2    (.CLK(div_clk), .D(n2), .Q(q));
endmodule
""",
            encoding="utf-8",
        )
        (tmp / "gen.sdc").write_text(
            """
create_clock -name clk -period 5.0 [get_ports clk]
create_generated_clock -name gclk -source [get_pins u_div/CLK] -divide_by 2 [get_pins u_div/Q]
set_propagated_clock [all_clocks]
set_input_delay 0.5 -clock clk [get_ports d]
set_output_delay 0.5 -clock clk [get_ports q]
""",
            encoding="utf-8",
        )
        gen_out = run_msta(
            tmp,
            f"""
read_liberty {SKY130_LIB}
read_verilog {tmp / 'gen.v'}
current_design gen
read_sdc {tmp / 'gen.sdc'}
report_checks -setup -clock_path -max_paths 4
explain_arc ff2 CLK D
explain_arc cb_root A X
""",
        )
        # Find ff1 -> ff2 path launched by generated clock gclk
        blocks = [b for b in gen_out.split("===== setup path ") if "startpoint : ff1" in b and "to register" in b]
        assert blocks, gen_out
        clk_blk = blocks[0]
        m_launch = re.search(r"launch clock\s*:\s*\S+\s*@\s*([0-9.]+)\s*ns", clk_blk)
        assert m_launch, clk_blk
        launch_ns = float(m_launch.group(1))
        path_blk = clk_blk.split("point ", 1)[1].split("ff1/Q", 1)[0]
        clk_lines = [ln for ln in path_blk.splitlines() if "u_div/Q" in ln or "cb_root/X" in ln]
        assert clk_lines, clk_blk
        last_clk_arr = float(clk_lines[-1].split()[2])
        assert abs(last_clk_arr - launch_ns) < 0.002, (
            f"launch clock path arrival {last_clk_arr} != LaunchTime {launch_ns}\n{clk_blk}"
        )
        assert "constraint rise (data rise)" in gen_out, gen_out

        # Missing -max_paths argument must exit non-zero and print error
        bad_df = tmp / "bad_max_paths.dofile"
        bad_df.write_text(
            f"read_liberty {SKY130_LIB}\nread_verilog {tmp / 'gen.v'}\ncurrent_design gen\n"
            f"read_sdc {tmp / 'gen.sdc'}\nreport_checks -max_paths\n",
            encoding="utf-8",
        )
        bad_res = subprocess.run(
            [str(MSTA), "-q", str(bad_df)],
            cwd=str(tmp),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=60,
        )
        assert bad_res.returncode != 0 and "缺少参数" in bad_res.stdout, bad_res.stdout

        # 5b: Both rise & fall edge RAT + set_case_analysis dump_graph edge pruning + set_data_check AuxReq
        (tmp / "dual.v").write_text(
            """
module dual(input clk, input a, input sel, input ref_d, output q);
  wire g1, g2, qa, qsel, qref;
  sky130_fd_sc_hd__dfxtp_1 ffa (.CLK(clk), .D(a),     .Q(qa));
  sky130_fd_sc_hd__dfxtp_1 ffs (.CLK(clk), .D(sel),   .Q(qsel));
  sky130_fd_sc_hd__dfxtp_1 ffr (.CLK(clk), .D(ref_d), .Q(qref));
  sky130_fd_sc_hd__and2_1  uand(.A(qa), .B(qsel), .X(g1));
  sky130_fd_sc_hd__inv_1   uinv(.A(g1), .Y(g2));
  sky130_fd_sc_hd__dfxtp_1 ffb (.CLK(clk), .D(g2),    .Q(q));
endmodule
""",
            encoding="utf-8",
        )
        (tmp / "dual.sdc").write_text(
            """
create_clock -name clk -period 5.0 [get_ports clk]
set_input_delay 0.5 -clock clk [get_ports {a sel ref_d}]
set_output_delay 0.5 -clock clk [get_ports q]
set_case_analysis 1 [get_pins uand/B]
set_data_check -setup 0.8 -from [get_nets qref] -to [get_nets g1]
""",
            encoding="utf-8",
        )
        dual_out = run_msta(
            tmp,
            f"""
read_liberty {SKY130_LIB}
read_verilog {tmp / 'dual.v'}
current_design dual
read_sdc {tmp / 'dual.sdc'}
report_net g2
report_net g1
dump_graph -o {tmp / 'dual.dot'}
""",
        )
        # g2 must have BOTH rise and fall max RAT populated (non-N/A)
        g2_blk = dual_out.split("===== report_net: g2 ")[1].split("setup slack")[0]
        max_rows = [ln for ln in g2_blk.splitlines() if ln.strip().startswith("max ")]
        assert len(max_rows) == 2 and all("N/A" not in r for r in max_rows), g2_blk
        # g1 must include AuxReq constraint from set_data_check
        g1_blk = dual_out.split("===== report_net: g1 ")[1].split("setup slack")[0]
        assert "N/A" not in g1_blk, g1_blk
        dot = (tmp / "dual.dot").read_text(encoding="utf-8")
        assert "uand:A->X" in dot, dot
        assert "uand:B->X" not in dot, dot
        # Every DOT node with numerical AT, RAT, SLK must satisfy RAT - AT == SLK within 0.002 ns rounding
        for m in re.finditer(r"AT:([0-9.-]+)\s+RAT:([0-9.-]+)\s+SLK:([0-9.-]+)", dot):
            at_ns, rat_ns, slk_ns = float(m.group(1)), float(m.group(2)), float(m.group(3))
            assert abs((rat_ns - at_ns) - slk_ns) <= 0.0025, f"AT/RAT/SLK mismatch: {m.group(0)}\n{dot}"
    print("[PASS] Group 5: Phase 2 review fixes (per-edge RAT, genclk -clock_path, dump_graph pruning, -max_paths, data_check)")


if __name__ == "__main__":
    assert MSTA.exists(), f"missing binary {MSTA}"
    test_group1_report_checks_flags()
    test_group2_explain_arc_hand_math()
    test_group3_backward_rat_and_multiclock_drc()
    test_group5_phase2_review_fixes()
    print("==> Phase 2 教学与模块化检查全部通过（5 组）")
