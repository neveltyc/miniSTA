#!/usr/bin/env python3
"""Phase 3 plugin architecture, per-fanout WireArc, PBA retrace & incremental swap_cell suite (4 groups)."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
MSTA = (Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build" / "msta").resolve()
SKY130_LIB = ROOT / "testcases" / "lib" / "sky130.lib"


def run_msta(cwd, script_text, name="case.dofile", expect_ok=True):
    df = Path(cwd) / name
    df.write_text(script_text, encoding="utf-8")
    res = subprocess.run(
        [str(MSTA), "-q", str(df)],
        cwd=str(cwd),
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=120,
    )
    if expect_ok:
        assert res.returncode == 0, f"msta exited {res.returncode} on {name}:\n{res.stdout}"
    else:
        assert res.returncode != 0, f"expected msta failure on {name}, got 0:\n{res.stdout}"
    return res.stdout


def test_group1_elmore_rc_per_fanout():
    """Group 1: elmore_rc per-fanout wire delay differentiation, cap, -clock_path & RAT."""
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        (tmp / "fanout3.v").write_text(
            """
module fanout3(input clk, input d, output q0, output q1, output q2);
  wire q_ff, w_drv, w_near, w_mid, w_far;
  sky130_fd_sc_hd__dfxtp_1 ff_src (.CLK(clk), .D(d), .Q(q_ff));
  sky130_fd_sc_hd__buf_1   u_drv  (.A(q_ff), .X(w_drv));
  sky130_fd_sc_hd__buf_1   u_near (.A(w_drv), .X(w_near));
  sky130_fd_sc_hd__buf_1   u_mid  (.A(w_drv), .X(w_mid));
  sky130_fd_sc_hd__buf_1   u_far  (.A(w_drv), .X(w_far));
  sky130_fd_sc_hd__dfxtp_1 ff_0   (.CLK(clk), .D(w_near), .Q(q0));
  sky130_fd_sc_hd__dfxtp_1 ff_1   (.CLK(clk), .D(w_mid),  .Q(q1));
  sky130_fd_sc_hd__dfxtp_1 ff_2   (.CLK(clk), .D(w_far),  .Q(q2));
endmodule
""",
            encoding="utf-8",
        )
        (tmp / "fanout3.sdc").write_text(
            """
create_clock -name clk -period 10.0 [get_ports clk]
set_input_delay 0.5 -clock clk [get_ports d]
set_output_delay 0.0 -clock clk [get_ports {q0 q1 q2}]
set_load 0.01 [get_ports {q0 q1 q2}]
""",
            encoding="utf-8",
        )
        out_zero = run_msta(
            tmp,
            f"""
read_liberty {SKY130_LIB}
read_verilog {tmp / 'fanout3.v'}
current_design fanout3
read_sdc {tmp / 'fanout3.sdc'}
explain_arc u_drv A X
report_checks -setup -max_paths 6 -cap -tran
""",
            name="zero.dofile",
        )
        out_elmore = run_msta(
            tmp,
            f"""
read_liberty {SKY130_LIB}
read_verilog {tmp / 'fanout3.v'}
current_design fanout3
read_sdc {tmp / 'fanout3.sdc'}
set_wire_model elmore_rc -r 0.2 -c 5.0
explain_arc u_drv A X
report_checks -setup -max_paths 6 -cap -tran -clock_path
report_net w_drv
report_net w_far
""",
            name="elmore.dofile",
        )

        # 1a: Effective capacitance increases by 3 * 5.0 = 15.0 fF on w_drv
        m_zcap = re.search(r"output net\s*:\s*w_drv\s*\(loads = 3, total = ([0-9.]+) fF", out_zero)
        m_ecap = re.search(r"output net\s*:\s*w_drv\s*\(loads = 3, total = ([0-9.]+) fF", out_elmore)
        assert m_zcap and m_ecap, out_elmore
        cap_z, cap_e = float(m_zcap.group(1)), float(m_ecap.group(1))
        assert abs((cap_e - cap_z) - 15.0) < 1e-4, f"expected +15.0 fF wire cap, got {cap_z} vs {cap_e}"

        # 1b: Parse actual vLoads order printed by explain_arc (Yosys JSON may reorder cells)
        u_to_info = {"u_near": ("ff_0", "w_near"), "u_mid": ("ff_1", "w_mid"), "u_far": ("ff_2", "w_far")}
        exp_drv = out_elmore.split("explain_arc: u_drv")[1].split("arc #1")[0]
        ordered_insts = re.findall(r"fanout\s+(\S+?)/A\s+:", exp_drv)
        assert set(ordered_insts) == set(u_to_info.keys()), exp_drv
        arrs = {}
        for blk in out_elmore.split("===== setup path ")[1:]:
            if "to register" not in blk:
                continue
            m_ep = re.search(r"endpoint\s*:\s*(\S+)", blk)
            m_at = re.search(r"data arrival time\s+([0-9.]+)", blk)
            if m_ep and m_at and m_ep.group(1) in ("ff_0", "ff_1", "ff_2"):
                arrs[m_ep.group(1)] = float(m_at.group(1))
        assert set(arrs.keys()) == {"ff_0", "ff_1", "ff_2"}, out_elmore
        ordered_ffs = [u_to_info[u][0] for u in ordered_insts]
        assert arrs[ordered_ffs[0]] < arrs[ordered_ffs[1]] < arrs[ordered_ffs[2]], (
            f"expected per-fanout delay increase along vLoads {ordered_ffs}: {arrs}"
        )
        assert "(wire)" in out_elmore, out_elmore

        # 1c: Critical net RAT - AT == Setup WNS under non-zero elmore_rc
        m_wns = re.search(r"setup\s*:\s*WNS\s+([0-9.-]+)\s+ns", out_elmore)
        assert m_wns, out_elmore
        wns = float(m_wns.group(1))
        worst_inst = ordered_insts[-1]
        worst_ff, worst_net = u_to_info[worst_inst]
        out_elmore_net = run_msta(
            tmp,
            f"""
read_liberty {SKY130_LIB}
read_verilog {tmp / 'fanout3.v'}
current_design fanout3
read_sdc {tmp / 'fanout3.sdc'}
set_wire_model elmore_rc -r 0.2 -c 5.0
explain_arc {worst_inst} A X
report_checks -setup -max_paths 6 -tran
report_net {worst_net}
""",
            name="elmore_net.dofile",
        )
        n_blk = out_elmore_net.split(f"===== report_net: {worst_net} ")[1].split("setup slack")[0]
        m_nw = re.search(r"setup slack\s*:\s*([0-9.-]+)\s+ns", out_elmore_net.split(f"===== report_net: {worst_net} ")[1])
        m_wd = re.search(r"setup slack\s*:\s*([0-9.-]+)\s+ns", out_elmore.split("===== report_net: w_drv ")[1])
        assert m_nw and m_wd, (out_elmore_net, out_elmore)
        assert abs(float(m_wd.group(1)) - wns) < 0.002, f"w_drv setup slack {m_wd.group(1)} != WNS {wns}"
        assert abs(float(m_nw.group(1)) - wns) < 0.002, f"{worst_net} setup slack {m_nw.group(1)} != WNS {wns}"
        for line in n_blk.splitlines():
            cols = line.split()
            if len(cols) >= 6 and cols[0] == "max":
                at_ns, rat_ns, slk_ns = float(cols[2]), float(cols[3]), float(cols[4])
                assert abs((rat_ns - at_ns) - slk_ns) < 0.0025, line

        # 1d: explain_arc under elmore_rc must expose input wire delay + degraded pin slew matching report_checks
        exp_worst = out_elmore_net.split(f"explain_arc: {worst_inst}")[1].split("===== setup path")[0]
        m_net_s = re.search(r"input net\s*:\s*w_drv\s*\(slew rise = ([0-9.]+) ps.*?fall = ([0-9.]+) ps", exp_worst)
        m_wire_s = re.search(
            r"input wire\s*:\s*delay rise = ([0-9.]+) ps.*?fall = ([0-9.]+) ps.*?pin slew rise = ([0-9.]+) ps.*?fall = ([0-9.]+) ps",
            exp_worst,
        )
        assert m_net_s and m_wire_s, exp_worst
        drv_sr, drv_sf = float(m_net_s.group(1)), float(m_net_s.group(2))
        wr_ps, wf_ps, pin_sr, pin_sf = map(float, m_wire_s.groups())
        assert wr_ps > 1.0 and wf_ps > 1.0 and pin_sr > drv_sr and pin_sf > drv_sf, exp_worst
        assert f"input wire   :" not in out_zero, "zero_rc explain_arc must not emit extra wire line"

        blk_worst = [
            b for b in out_elmore_net.split("===== setup path ")
            if f"endpoint   : {worst_ff}" in b
        ][0]
        m_edge = re.search(r"data edge\s*:\s*(rise|fall)", blk_worst)
        m_inc = re.search(rf"{re.escape(worst_inst)}/X\s+([0-9.-]+)\s+([0-9.-]+)", blk_worst)
        assert m_edge and m_inc, blk_worst
        is_rise = (m_edge.group(1) == "rise")
        tag = "output rise delay (input rise)" if is_rise else "output fall delay (input fall)"
        cell_blk = exp_worst.split(tag)[1].split("--- ")[0]
        m_fin = re.search(r"final delay\s*:\s*([0-9.]+) ps", cell_blk)
        assert m_fin, cell_blk
        expected_inc_ns = ((wr_ps if is_rise else wf_ps) + float(m_fin.group(1))) / 1000.0
        assert abs(float(m_inc.group(1)) - expected_inc_ns) < 0.002, (m_inc.group(1), expected_inc_ns)

        # Error check on unknown model
        run_msta(
            tmp,
            f"read_liberty {SKY130_LIB}\nset_wire_model bogus_rc\n",
            name="bad_wire.dofile",
            expect_ok=False,
        )
    print("[PASS] Group 1: elmore_rc per-fanout WireArc, explain_arc wire slew, -cap, -clock_path & RAT")


def test_group2_pba_retrace_aocv():
    """Group 2: pba_retrace removes conservative GBA shortest-depth AOCV pessimism."""
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        (tmp / "pba.v").write_text(
            """
module pba(input clk, input d, output q);
  wire n0, b1w, b2w, b3w, shortw, jw;
  sky130_fd_sc_hd__dfxtp_1 ff_in  (.CLK(clk), .D(d),      .Q(n0));
  sky130_fd_sc_hd__buf_1   u_b1   (.A(n0),     .X(b1w));
  sky130_fd_sc_hd__buf_1   u_b2   (.A(b1w),    .X(b2w));
  sky130_fd_sc_hd__buf_1   u_b3   (.A(b2w),    .X(b3w));
  sky130_fd_sc_hd__buf_1   u_sh   (.A(n0),     .X(shortw));
  sky130_fd_sc_hd__and2_1  u_join (.A(b3w),    .B(shortw), .X(jw));
  sky130_fd_sc_hd__dfxtp_1 ff_out (.CLK(clk),  .D(jw),     .Q(q));
endmodule
""",
            encoding="utf-8",
        )
        (tmp / "pba.sdc").write_text(
            """
create_clock -name clk -period 5.0 [get_ports clk]
set_input_delay 0.1 -clock clk [get_ports d]
set_output_delay 0.1 -clock clk [get_ports q]
""",
            encoding="utf-8",
        )
        (tmp / "pba.aocv").write_text(
            """# scope pattern path transition corner depth factor
design * data rise late 1 1.40
design * data rise late 2 1.35
design * data rise late 3 1.30
design * data rise late 4 1.15
design * data rise late 5 1.08
design * data rise late 6 1.02
design * data fall late 1 1.40
design * data fall late 2 1.35
design * data fall late 3 1.30
design * data fall late 4 1.15
design * data fall late 5 1.08
design * data fall late 6 1.02
""",
            encoding="utf-8",
        )
        out_gba = run_msta(
            tmp,
            f"""
read_liberty {SKY130_LIB}
read_verilog {tmp / 'pba.v'}
current_design pba
read_aocv {tmp / 'pba.aocv'}
read_sdc {tmp / 'pba.sdc'}
set_path_refiner none
report_checks -setup -max_paths 4
report_summary
""",
            name="gba.dofile",
        )
        out_pba = run_msta(
            tmp,
            f"""
read_liberty {SKY130_LIB}
read_verilog {tmp / 'pba.v'}
current_design pba
read_aocv {tmp / 'pba.aocv'}
read_sdc {tmp / 'pba.sdc'}
set_path_refiner pba_retrace
report_checks -setup -max_paths 4
report_summary
""",
            name="pba.dofile",
        )
        blk_gba = [b for b in out_gba.split("===== setup path ") if "startpoint : ff_in" in b][0]
        blk_pba = [b for b in out_pba.split("===== setup path ") if "startpoint : ff_in" in b][0]
        m_arr_gba = re.search(r"data arrival time\s+([0-9.]+)", blk_gba)
        m_arr_pba = re.search(r"data arrival time\s+([0-9.]+)", blk_pba)
        m_slk_gba = re.search(r"slack \(MET\)\s+([0-9.-]+)", blk_gba)
        m_slk_pba = re.search(r"slack \(MET\)\s+([0-9.-]+)", blk_pba)
        assert m_arr_gba and m_arr_pba and m_slk_gba and m_slk_pba, (blk_gba, blk_pba)
        arr_gba, arr_pba = float(m_arr_gba.group(1)), float(m_arr_pba.group(1))
        slk_gba, slk_pba = float(m_slk_gba.group(1)), float(m_slk_pba.group(1))
        assert arr_pba < arr_gba - 0.02, f"expected PBA arrival < GBA arrival under shallow bypass: pba={arr_pba}, gba={arr_gba}"
        assert slk_pba > slk_gba + 0.02, f"expected PBA slack > GBA slack: pba={slk_pba}, gba={slk_gba}"

        # 2b: Dual-library set_operating_conditions corner check mapping under pba_retrace
        def _write_corner_lib(path, lib_name, op_name, delay_ps, setup_ps):
            path.write_text(
                f"""
library({lib_name}) {{
  delay_model : table_lookup;
  time_unit : "1ps";
  capacitive_load_unit (1,ff);
  slew_lower_threshold_pct_rise : 20.0;
  slew_upper_threshold_pct_rise : 80.0;
  slew_lower_threshold_pct_fall : 20.0;
  slew_upper_threshold_pct_fall : 80.0;
  input_threshold_pct_rise : 50.0;
  input_threshold_pct_fall : 50.0;
  output_threshold_pct_rise : 50.0;
  output_threshold_pct_fall : 50.0;
  operating_conditions({op_name}) {{ process : 1.0; temperature : 25.0; voltage : 1.2; }}
  lu_table_template(t2) {{
    variable_1 : input_net_transition; variable_2 : total_output_net_capacitance;
    index_1 ("10.0, 100.0"); index_2 ("5.0, 50.0");
  }}
  lu_table_template(c2) {{
    variable_1 : related_pin_transition; variable_2 : constrained_pin_transition;
    index_1 ("10.0, 100.0"); index_2 ("10.0, 100.0");
  }}
  cell(FF1) {{
    area : 4.0;
    ff(IQ, IQN) {{ clocked_on : "CK"; next_state : "D"; }}
    pin(CK) {{ direction : input; capacitance : 2.0; clock : true; }}
    pin(D)  {{ direction : input; capacitance : 2.0;
      timing() {{ related_pin : "CK"; timing_type : setup_rising;
        rise_constraint(c2) {{ values("{setup_ps}, {setup_ps}", "{setup_ps}, {setup_ps}"); }}
        fall_constraint(c2) {{ values("{setup_ps}, {setup_ps}", "{setup_ps}, {setup_ps}"); }} }}
      timing() {{ related_pin : "CK"; timing_type : hold_rising;
        rise_constraint(c2) {{ values("10, 10", "10, 10"); }}
        fall_constraint(c2) {{ values("10, 10", "10, 10"); }} }} }}
    pin(Q)  {{ direction : output;
      timing() {{ related_pin : "CK"; timing_type : rising_edge;
        cell_rise(t2) {{ values("{delay_ps}, {delay_ps}", "{delay_ps}, {delay_ps}"); }}
        cell_fall(t2) {{ values("{delay_ps}, {delay_ps}", "{delay_ps}, {delay_ps}"); }}
        rise_transition(t2) {{ values("15, 25", "20, 30"); }}
        fall_transition(t2) {{ values("15, 25", "20, 30"); }} }} }}
  }}
  cell(BUF1) {{
    area : 2.0;
    pin(A) {{ direction : input; capacitance : 2.0; }}
    pin(Y) {{ direction : output; function : "A";
      timing() {{ related_pin : "A"; timing_sense : positive_unate;
        cell_rise(t2) {{ values("{delay_ps}, {delay_ps}", "{delay_ps}, {delay_ps}"); }}
        cell_fall(t2) {{ values("{delay_ps}, {delay_ps}", "{delay_ps}, {delay_ps}"); }}
        rise_transition(t2) {{ values("15, 25", "20, 30"); }}
        fall_transition(t2) {{ values("15, 25", "20, 30"); }} }} }}
  }}
}}
""",
                encoding="utf-8",
            )

        _write_corner_lib(tmp / "fast.lib", "fast_lib", "fast", 40.0, 50.0)
        _write_corner_lib(tmp / "slow.lib", "slow_lib", "slow", 120.0, 300.0)
        (tmp / "dual_lib.v").write_text(
            """
module dual_lib(input clk, input d, output q);
  wire w1, w2;
  FF1  ff1 (.CK(clk), .D(d),  .Q(w1));
  BUF1 u1  (.A(w1),   .Y(w2));
  FF1  ff2 (.CK(clk), .D(w2), .Q(q));
endmodule
""",
            encoding="utf-8",
        )
        (tmp / "dual_lib.sdc").write_text(
            """
create_clock -name clk -period 2.0 [get_ports clk]
set_input_delay 0.1 -clock clk [get_ports d]
set_output_delay 0.1 -clock clk [get_ports q]
set_operating_conditions -max slow -max_library slow_lib -min fast -min_library fast_lib
""",
            encoding="utf-8",
        )
        out_dual = run_msta(
            tmp,
            f"""
read_liberty {tmp / 'fast.lib'} {tmp / 'slow.lib'}
read_verilog {tmp / 'dual_lib.v'}
current_design dual_lib
read_sdc {tmp / 'dual_lib.sdc'}
set_path_refiner pba_retrace
report_checks -setup -max_paths 2
""",
            name="pba_dual.dofile",
        )
        assert "- setup check (from lib)" in out_dual and "-0.300" in out_dual, (
            f"expected corner slow_lib setup check -0.300 ns under pba_retrace:\n{out_dual}"
        )

        # 2c: Latch with set_max_time_borrow under pba_retrace preserves borrow RAT
        (tmp / "pba_lat.v").write_text(
            """
module pba_lat(input clk, input d, output q);
  wire n1, latq;
  sky130_fd_sc_hd__buf_1   u_in (.A(d), .X(n1));
  sky130_fd_sc_hd__dlxtp_1 u_lat (.D(n1), .GATE(clk), .Q(latq));
  sky130_fd_sc_hd__inv_1   u_inv (.A(latq), .Y(q));
endmodule
""",
            encoding="utf-8",
        )
        (tmp / "pba_lat.sdc").write_text(
            """
create_clock -name clk -period 10.0 [get_ports clk]
set_input_delay -clock clk -max 2.0 [get_ports d]
set_input_delay -clock clk -min 0.5 [get_ports d]
set_output_delay -clock clk -max 1.0 [get_ports q]
set_output_delay -clock clk -min 0.0 [get_ports q]
set_max_time_borrow 0.5 [get_cells u_lat]
""",
            encoding="utf-8",
        )
        out_lat_gba = run_msta(
            tmp,
            f"""
read_liberty {SKY130_LIB}
read_verilog {tmp / 'pba_lat.v'}
current_design pba_lat
read_sdc {tmp / 'pba_lat.sdc'}
set_path_refiner none
report_checks -setup -max_paths 2
""",
            name="pba_lat_gba.dofile",
        )
        out_lat_pba = run_msta(
            tmp,
            f"""
read_liberty {SKY130_LIB}
read_verilog {tmp / 'pba_lat.v'}
current_design pba_lat
read_sdc {tmp / 'pba_lat.sdc'}
set_path_refiner pba_retrace
report_checks -setup -max_paths 2
""",
            name="pba_lat_pba.dofile",
        )
        lat_gba = [b for b in out_lat_gba.split("===== setup path ") if "to latch" in b][0]
        lat_pba = [b for b in out_lat_pba.split("===== setup path ") if "to latch" in b][0]
        m_rg = re.search(r"data required time\s+([0-9.-]+)", lat_gba)
        m_rp = re.search(r"data required time\s+([0-9.-]+)", lat_pba)
        assert m_rg and m_rp and m_rg.group(1) == m_rp.group(1) == "0.500", (lat_gba, lat_pba)
    print("[PASS] Group 2: pba_retrace AOCV depth, corner check mapping & latch time_borrow")


def test_group3_standalone_c_plugin():
    """Group 3: external C99 plugin linking against build/*.o overrides DelayOps & WireOps cleanly."""
    cc_cmd = shlex.split(os.environ.get("CC", "cc"))
    objs = sorted(str(p) for p in (ROOT / "build").glob("*.o") if p.name != "msta_main.o")
    assert objs, "build/*.o missing; build msta first"

    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        (tmp / "cplug.v").write_text(
            """
module cplug(input clk, input d, output q);
  wire n1, n2;
  sky130_fd_sc_hd__dfxtp_1 ff1 (.CLK(clk), .D(d),  .Q(n1));
  sky130_fd_sc_hd__inv_1   u1  (.A(n1),    .Y(n2));
  sky130_fd_sc_hd__dfxtp_1 ff2 (.CLK(clk), .D(n2), .Q(q));
endmodule
""",
            encoding="utf-8",
        )
        (tmp / "cplug.sdc").write_text(
            """
create_clock -name clk -period 1.0 [get_ports clk]
set_input_delay 0.1 -clock clk [get_ports d]
set_output_delay 0.1 -clock clk [get_ports q]
""",
            encoding="utf-8",
        )
        c_src = tmp / "custom_plugin_test.c"
        c_src.write_text(
            r"""
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "msta_lib.h"
#include "msta_net.h"
#include "msta_sdc.h"
#include "msta_plugin.h"
#include "msta_timing.h"

static void ConstEvalArc( const MstaDelayQuery *pQ, MstaDelayResult *pOut )
{
    (void)pQ;
    pOut->Delay = 120.0;
    pOut->OutSlew = 30.0;
}

static double ConstEvalConstraint( const struct MstaTiming *pT, const MstaArc *pArc,
                                   int nInst, int fMax, int fDataRise,
                                   double ClkSlew, double DataSlew )
{
    (void)pT; (void)pArc; (void)nInst; (void)fDataRise; (void)ClkSlew; (void)DataSlew;
    return fMax ? 40.0 : 10.0;
}

static double ConstWireCap( const MstaWireQuery *pQ )
{
    (void)pQ;
    return 10.0;
}

static void ConstWireArc( const MstaWireQuery *pQ, MstaWireResult *pOut )
{
    pOut->WireDelay = 25.0;
    pOut->SinkSlew = pQ->DriverSlew + 5.0;
}

typedef struct {
    unsigned int    Magic;
    MstaElmoreConfig Cfg;
} ElmoreWrapCfg;

static double WrappedElmoreCap( const MstaWireQuery *pQ )
{
    const ElmoreWrapCfg *pW = (const ElmoreWrapCfg *)pQ->pUserData;
    MstaWireQuery Q2 = *pQ;
    assert( pW != NULL && pW->Magic == 0x5A5AA5A5u );
    Q2.pUserData = (void *)&pW->Cfg;
    return Msta_PluginDefaultElmoreWire()->NetEffCap( &Q2 );
}

static void WrappedElmoreArc( const MstaWireQuery *pQ, MstaWireResult *pOut )
{
    const ElmoreWrapCfg *pW = (const ElmoreWrapCfg *)pQ->pUserData;
    MstaWireQuery Q2 = *pQ;
    assert( pW != NULL && pW->Magic == 0x5A5AA5A5u );
    Q2.pUserData = (void *)&pW->Cfg;
    Msta_PluginDefaultElmoreWire()->EvalWireArc( &Q2, pOut );
}

static double HalfCheckScale( const struct MstaTiming *pT, int nInst, int fMax, int fDataRise )
{
    return Msta_PluginVariationDefault()->CheckScale( pT, nInst, fMax, fDataRise ) * 0.5;
}

int main( int argc, char **argv )
{
    const MstaDelayOps DelayOps = {
        MSTA_PLUGIN_ABI_VERSION, "const_delay", NULL, ConstEvalArc, ConstEvalConstraint
    };
    const MstaWireOps WireOps = {
        MSTA_PLUGIN_ABI_VERSION, "const_wire", NULL, ConstWireCap, ConstWireArc
    };
    const MstaDelayOps PartialDelayOps = {
        MSTA_PLUGIN_ABI_VERSION, "partial_delay", NULL, ConstEvalArc, NULL
    };
    const MstaVariationOps PartialVarOps = {
        MSTA_PLUGIN_ABI_VERSION, "half_check_var", NULL, NULL, HalfCheckScale, NULL, NULL
    };
    ElmoreWrapCfg WrapCfg = { 0x5A5AA5A5u, { 0.1, 3.0 } };
    const MstaWireOps WrapWireOps = {
        MSTA_PLUGIN_ABI_VERSION, "wrap_elmore", &WrapCfg, WrappedElmoreCap, WrappedElmoreArc
    };
    MstaLib *pLib = Msta_LibStart();
    MstaDesign *pDes = Msta_DesignStart();
    MstaSdc *pSdc = Msta_SdcStart();
    MstaTiming *pTime;
    MstaPluginSet Set;
    const char *pVer = argv[2];
    const MstaCheck *pFound = NULL;
    double wd = -1.0, ss = -1.0, baseCheck, baseSlack;
    int i, ok;

    assert( argc >= 5 );
    Msta_SdcSetBridgePath( argv[1] );
    ok = Msta_LibRead( pLib, argv[3], 0 );
    assert( ok );
    ok = Msta_DesignReadVerilog( pDes, &pVer, 1, argv[5], 0 );
    assert( ok );
    ok = Msta_DesignFlatten( pDes, pLib, "cplug", 0 );
    assert( ok );
    ok = Msta_SdcReadFile( pSdc, pDes, pLib, argv[4], 0 );
    assert( ok );

    pTime = Msta_TimingStart( pDes, pLib, pSdc );
    Msta_PluginSetInitDefaults( &Set );
    Set.pDelay = &DelayOps;
    Set.pWire = &WireOps;
    Msta_TimingSetPlugins( pTime, &Set );

    /* Must be safe even before Msta_TimingAnalyze (pfIdealNet == NULL) */
    Msta_TimingPinWireDelay( pTime, 0, -1, -1, 1, 1, 0, 50.0, &wd, &ss );
    assert( fabs(wd - 25.0) < 1e-6 && fabs(ss - 55.0) < 1e-6 );

    ok = Msta_TimingAnalyze( pTime, 0 );
    assert( ok );
    for ( i = 0; i < pTime->vChecks.nSize; i++ )
    {
        const MstaCheck *pC = MstaCheckArrayAt( &pTime->vChecks, i );
        if ( pC->fToRegister && !strcmp(Msta_InstName(pDes, pC->InstId), "ff2") )
        {   pFound = pC; break;   }
    }
    assert( pFound && pFound->Setup.fChecked );
    /* Clock net is common root -> +25 ps on launch CLK and +25 ps on capture CLK cancel cleanly.
       Data: launchClk(25) + clk2q(120) + wire_to_u1(25) + u1(120) + wire_to_ff2(25) = 315 ps.
       Required: period(1000) + capClk(25) - setup(40) = 985 ps => Slack = 985 - 315 = 670.0 ps. */
    printf( "CUSTOM_PLUGIN_SETUP=%.6f CUSTOM_PLUGIN_HOLD=%.6f\n",
            pFound->Setup.Slack, pFound->Hold.Slack );
    assert( fabs(pFound->Setup.Slack - 670.0) < 1e-6 );
    /* Hold: Arr(315) - (capClk(25) + hold(10)) = 280.0 ps. */
    assert( fabs(pFound->Hold.Slack - 280.0) < 1e-6 );

    /* Test pUserData forwarding via Elmore decorator wrapper + partial NULL callbacks */
    Msta_PluginSetInitDefaults( &Set );
    Set.pDelay = &PartialDelayOps;
    Set.pWire = &WrapWireOps;
    Msta_TimingSetPlugins( pTime, &Set );
    ok = Msta_TimingAnalyze( pTime, 0 );
    assert( ok );
    pFound = NULL;
    for ( i = 0; i < pTime->vChecks.nSize; i++ )
    {
        const MstaCheck *pC = MstaCheckArrayAt( &pTime->vChecks, i );
        if ( pC->fToRegister && !strcmp(Msta_InstName(pDes, pC->InstId), "ff2") )
        {   pFound = pC; break;   }
    }
    assert( pFound && pFound->Setup.fChecked );
    baseCheck = pFound->Setup.CheckTime;
    baseSlack = pFound->Setup.Slack;

    Set.pVariation = &PartialVarOps;
    Msta_TimingSetPlugins( pTime, &Set );
    ok = Msta_TimingAnalyze( pTime, 0 );
    assert( ok );
    pFound = NULL;
    for ( i = 0; i < pTime->vChecks.nSize; i++ )
    {
        const MstaCheck *pC = MstaCheckArrayAt( &pTime->vChecks, i );
        if ( pC->fToRegister && !strcmp(Msta_InstName(pDes, pC->InstId), "ff2") )
        {   pFound = pC; break;   }
    }
    assert( pFound && pFound->Setup.fChecked );
    assert( fabs(pFound->Setup.CheckTime - baseCheck * 0.5) < 1e-6 );
    assert( fabs(pFound->Setup.Slack - (baseSlack + baseCheck * 0.5)) < 1e-6 );

    Msta_TimingFree( pTime );
    Msta_SdcFree( pSdc );
    Msta_DesignFree( pDes );
    Msta_LibFree( pLib );
    return 0;
}
""",
            encoding="utf-8",
        )
        exe = tmp / "custom_plugin_test"
        comp = [*cc_cmd, "-std=gnu99", "-O2", f"-I{ROOT / 'src'}", str(c_src), *objs, "-o", str(exe), "-lm"]
        res_c = subprocess.run(comp, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)
        assert res_c.returncode == 0, f"standalone C plugin compile failed:\n{res_c.stdout}"
        res_r = subprocess.run(
            [str(exe), str(MSTA), str(tmp / "cplug.v"), str(SKY130_LIB), str(tmp / "cplug.sdc"), str(tmp)],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=60,
        )
        assert res_r.returncode == 0, f"standalone C plugin run failed:\n{res_r.stdout}"
        assert "CUSTOM_PLUGIN_SETUP=670.000000" in res_r.stdout, res_r.stdout
    print("[PASS] Group 3: standalone C99 custom DelayOps + WireOps plugin verified to 1e-6 ps")


def _assert_swap_ok(out, inst, new_cell, expect_incr=True):
    pat = (
        rf"swap_cell:\s*{re.escape(inst)}\s*->\s*{re.escape(new_cell)}\s+"
        rf"mode=(\S+)\s+dirty_nets=(\d+)/(\d+)\s+checks=(\d+).*?verify_max_diff=([0-9.eE+-]+)\s+ps"
    )
    m = re.search(pat, out)
    assert m, f"missing swap_cell summary for {inst}->{new_cell} in:\n{out}"
    mode, dirty, total, chks, diff = m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)), float(m.group(5))
    expected_mode = "incremental" if expect_incr else "full_fallback"
    assert mode == expected_mode, f"expected {expected_mode}, got {mode}: {m.group(0)}"
    assert diff <= 1e-9, f"verify_max_diff {diff} > 1e-9 ps: {m.group(0)}"
    return dirty, total, chks, diff


def test_group4_swap_cell_incremental():
    """Group 4: swap_cell -verify exactness (permuted/shorted pins, elmore_rc, pipe_demo, multi_vt_soc, fallback)."""
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        # 4a: Synthetic permuted-pin + shorted-pin cell swap under both zero_rc and elmore_rc
        (tmp / "swap_syn.lib").write_text(
            """
library(swap_syn) {
  delay_model : table_lookup;
  time_unit : "1ps";
  capacitive_load_unit (1,ff);
  slew_lower_threshold_pct_rise : 20.0;
  slew_upper_threshold_pct_rise : 80.0;
  slew_lower_threshold_pct_fall : 20.0;
  slew_upper_threshold_pct_fall : 80.0;
  input_threshold_pct_rise : 50.0;
  input_threshold_pct_fall : 50.0;
  output_threshold_pct_rise : 50.0;
  output_threshold_pct_fall : 50.0;
  lu_table_template(t2) {
    variable_1 : input_net_transition;
    variable_2 : total_output_net_capacitance;
    index_1 ("10.0, 100.0");
    index_2 ("5.0, 50.0");
  }
  lu_table_template(c2) {
    variable_1 : related_pin_transition;
    variable_2 : constrained_pin_transition;
    index_1 ("10.0, 100.0");
    index_2 ("10.0, 100.0");
  }
  cell(FF1) {
    area : 5.0;
    ff(IQ, IQN) { clocked_on : "CK"; next_state : "D"; }
    pin(CK) { direction : input; capacitance : 2.0; clock : true; }
    pin(D)  { direction : input; capacitance : 2.0;
      timing() { related_pin : "CK"; timing_type : setup_rising;
        rise_constraint(c2) { values("20, 25", "22, 27"); }
        fall_constraint(c2) { values("20, 25", "22, 27"); } }
      timing() { related_pin : "CK"; timing_type : hold_rising;
        rise_constraint(c2) { values("5, 8", "6, 9"); }
        fall_constraint(c2) { values("5, 8", "6, 9"); } } }
    pin(Q)  { direction : output;
      timing() { related_pin : "CK"; timing_type : rising_edge;
        cell_rise(t2) { values("30, 50", "40, 60"); }
        cell_fall(t2) { values("30, 50", "40, 60"); }
        rise_transition(t2) { values("15, 30", "20, 35"); }
        fall_transition(t2) { values("15, 30", "20, 35"); } } }
  }
  cell(AND2_A) {
    area : 2.0;
    pin(A) { direction : input; capacitance : 2.0; }
    pin(B) { direction : input; capacitance : 2.0; }
    pin(Y) { direction : output; function : "A & B";
      timing() { related_pin : "A"; timing_sense : positive_unate;
        cell_rise(t2) { values("40, 80", "50, 90"); }
        cell_fall(t2) { values("35, 75", "45, 85"); }
        rise_transition(t2) { values("15, 35", "20, 40"); }
        fall_transition(t2) { values("15, 35", "20, 40"); } }
      timing() { related_pin : "B"; timing_sense : positive_unate;
        cell_rise(t2) { values("45, 85", "55, 95"); }
        cell_fall(t2) { values("40, 80", "50, 90"); }
        rise_transition(t2) { values("15, 35", "20, 40"); }
        fall_transition(t2) { values("15, 35", "20, 40"); } } }
  }
  /* Permuted pin order (Y, B, A) + higher input cap + faster delay */
  cell(AND2_PERM_FAST) {
    area : 4.0;
    pin(Y) { direction : output; function : "A & B";
      timing() { related_pin : "B"; timing_sense : positive_unate;
        cell_rise(t2) { values("22, 42", "28, 48"); }
        cell_fall(t2) { values("20, 40", "25, 45"); }
        rise_transition(t2) { values("10, 22", "14, 26"); }
        fall_transition(t2) { values("10, 22", "14, 26"); } }
      timing() { related_pin : "A"; timing_sense : positive_unate;
        cell_rise(t2) { values("20, 40", "26, 46"); }
        cell_fall(t2) { values("18, 38", "24, 44"); }
        rise_transition(t2) { values("10, 22", "14, 26"); }
        fall_transition(t2) { values("10, 22", "14, 26"); } } }
    pin(B) { direction : input; capacitance : 6.0; }
    pin(A) { direction : input; capacitance : 5.0; }
  }
}
""",
            encoding="utf-8",
        )
        (tmp / "swap_syn.v").write_text(
            """
module swap_syn(input clk, input d1, input d2, output q1, output q2);
  wire n1, n2, w1, w2, sib;
  FF1 ff_a (.CK(clk), .D(d1), .Q(n1));
  FF1 ff_b (.CK(clk), .D(d2), .Q(n2));
  AND2_A u_sib  (.A(n1), .B(n2), .Y(sib));
  AND2_A u_swap (.A(n1), .B(n2), .Y(w1));
  AND2_A u_tie  (.A(n1), .B(n1), .Y(w2));
  FF1 ff_o1 (.CK(clk), .D(w1),  .Q(q1));
  FF1 ff_o2 (.CK(clk), .D(w2),  .Q(q2));
  FF1 ff_os (.CK(clk), .D(sib), .Q(n_unused_out));
endmodule
""",
            encoding="utf-8",
        )
        (tmp / "swap_syn.sdc").write_text(
            """
create_clock -name clk -period 1000.0 [get_ports clk]
set_input_delay 10.0 -clock clk [get_ports {d1 d2}]
set_output_delay 10.0 -clock clk [get_ports {q1 q2}]
""",
            encoding="utf-8",
        )
        # Verify zero_rc, report_checks-before-swap (qsort invariance!), shorted-pin swap, and elmore_rc
        out_syn = run_msta(
            tmp,
            f"""
read_liberty {tmp / 'swap_syn.lib'}
read_verilog {tmp / 'swap_syn.v'}
current_design swap_syn
read_sdc {tmp / 'swap_syn.sdc'}
report_checks -setup -max_paths 3
swap_cell u_swap AND2_PERM_FAST -verify
swap_cell u_tie AND2_PERM_FAST -verify
set_wire_model elmore_rc -r 0.2 -c 5.0
report_checks -setup -max_paths 2
swap_cell u_swap AND2_A -verify
report_checks -setup -max_paths 1
""",
            name="swap_syn.dofile",
        )
        _assert_swap_ok(out_syn, "u_swap", "AND2_PERM_FAST", expect_incr=True)
        _assert_swap_ok(out_syn, "u_tie", "AND2_PERM_FAST", expect_incr=True)
        _assert_swap_ok(out_syn, "u_swap", "AND2_A", expect_incr=True)

        # 4b: Real sky130 pipe_demo combinational upsize + seq fallback + bad pin error
        pipe_v = ROOT / "testcases" / "synth_sky130" / "netlist" / "pipe_demo.v"
        pipe_sdc = ROOT / "testcases" / "synth_sky130" / "pipe_demo.sdc"
        out_pipe = run_msta(
            tmp,
            f"""
read_liberty {SKY130_LIB}
read_verilog {pipe_v}
current_design pipe_demo
read_sdc {pipe_sdc}
report_checks -setup -max_paths 2
swap_cell _114_ sky130_fd_sc_hd__a21o_2 -verify
swap_cell _109_ sky130_fd_sc_hd__xor2_2 -verify
swap_cell _172_ sky130_fd_sc_hd__dfrtp_2 -verify
""",
            name="swap_pipe.dofile",
        )
        d1, t1, _, _ = _assert_swap_ok(out_pipe, "_114_", "sky130_fd_sc_hd__a21o_2", expect_incr=True)
        assert d1 < t1, f"expected localized dirty cone {d1}/{t1}"
        _assert_swap_ok(out_pipe, "_109_", "sky130_fd_sc_hd__xor2_2", expect_incr=True)
        _assert_swap_ok(out_pipe, "_172_", "sky130_fd_sc_hd__dfrtp_2", expect_incr=False)

        # 4c: multi_vt_soc (8508 instances, LVT -> HVT / RVT swap)
        mv_dir = ROOT / "testcases" / "multi_vt"
        out_mv = run_msta(
            tmp,
            f"""
read_liberty {mv_dir / 'lib' / 'ics55_LLSC_H7CL_typ_tt.lib'} {mv_dir / 'lib' / 'ics55_LLSC_H7CR_typ_tt.lib'} {mv_dir / 'lib' / 'ics55_LLSC_H7CH_typ_tt.lib'}
read_verilog {mv_dir / 'netlist' / 'multi_vt_soc.v'}
current_design soc_top
read_sdc {mv_dir / 'multi_vt_soc.sdc'}
swap_cell u_core._11928_ AOI21X0P5H7H -verify
swap_cell u_core._08405_ NOR2X0P5H7R -verify
""",
            name="swap_mv.dofile",
        )
        d_leaf, t_leaf, c_leaf, _ = _assert_swap_ok(out_mv, "u_core._11928_", "AOI21X0P5H7H", expect_incr=True)
        assert d_leaf < 600 and c_leaf < 100 and t_leaf == 8555, (d_leaf, c_leaf, t_leaf)
        _assert_swap_ok(out_mv, "u_core._08405_", "NOR2X0P5H7R", expect_incr=True)

        # 4d: Multi-library lib/cell swap + vCornerCells realloc rebind + fIgnore cell rejection
        def _write_swap_corner_lib(path, lib_name, op_name, extra_and2_delay, extra_dummies=0):
            dummies = "".join(
                f'  cell(FF1_D{k}) {{ area : 1.0; pin(A) {{ direction : input; capacitance : 1.0; }} }}\n'
                for k in range(extra_dummies)
            )
            extra_cells = ""
            if extra_and2_delay is not None:
                extra_cells = f"""
  cell(AND2_A) {{
    area : 3.0;
    pin(A) {{ direction : input; capacitance : 2.5; }}
    pin(B) {{ direction : input; capacitance : 2.5; }}
    pin(Y) {{ direction : output; function : "A & B";
      timing() {{ related_pin : "A"; timing_sense : positive_unate;
        cell_rise(t2) {{ values("{extra_and2_delay}, {extra_and2_delay}", "{extra_and2_delay}, {extra_and2_delay}"); }}
        cell_fall(t2) {{ values("{extra_and2_delay}, {extra_and2_delay}", "{extra_and2_delay}, {extra_and2_delay}"); }}
        rise_transition(t2) {{ values("10, 20", "15, 25"); }}
        fall_transition(t2) {{ values("10, 20", "15, 25"); }} }}
      timing() {{ related_pin : "B"; timing_sense : positive_unate;
        cell_rise(t2) {{ values("{extra_and2_delay}, {extra_and2_delay}", "{extra_and2_delay}, {extra_and2_delay}"); }}
        cell_fall(t2) {{ values("{extra_and2_delay}, {extra_and2_delay}", "{extra_and2_delay}, {extra_and2_delay}"); }}
        rise_transition(t2) {{ values("10, 20", "15, 25"); }}
        fall_transition(t2) {{ values("10, 20", "15, 25"); }} }} }}
  }}
  cell(IGN_PWR) {{
    area : 0.0;
    pin(VPWR) {{ direction : input; capacitance : 0.0; }}
    pin(VGND) {{ direction : input; capacitance : 0.0; }}
    pin(VPB)  {{ direction : output; capacitance : 0.0; }}
  }}
"""
            else:
                extra_cells = "".join(
                    f'  cell(FF1) {{ area : 5.0; pin(CK) {{ direction : input; }} pin(D) {{ direction : input; }} pin(Q) {{ direction : output; }} }}\n'
                    if k == 0 else
                    f'  cell(AND2_A) {{ area : 2.0; pin(A) {{ direction : input; }} pin(B) {{ direction : input; }} pin(Y) {{ direction : output; }} }}\n'
                    for k in range(2)
                )
            path.write_text(
                f"""
library({lib_name}) {{
  delay_model : table_lookup;
  time_unit : "1ps";
  capacitive_load_unit (1,ff);
  slew_lower_threshold_pct_rise : 20.0; slew_upper_threshold_pct_rise : 80.0;
  slew_lower_threshold_pct_fall : 20.0; slew_upper_threshold_pct_fall : 80.0;
  input_threshold_pct_rise : 50.0; input_threshold_pct_fall : 50.0;
  output_threshold_pct_rise : 50.0; output_threshold_pct_fall : 50.0;
  operating_conditions({op_name}) {{ process : 1.0; temperature : 25.0; voltage : 1.2; }}
  lu_table_template(t2) {{
    variable_1 : input_net_transition; variable_2 : total_output_net_capacitance;
    index_1 ("10.0, 100.0"); index_2 ("5.0, 50.0");
  }}
  {extra_cells}
  {dummies}
}}
""",
                encoding="utf-8",
            )

        _write_swap_corner_lib(tmp / "corner_b.lib", "lib_b", "op_b", 120.0, 0)
        # Re-define AND2_A in several extra libraries so vCornerCells reallocates repeatedly
        extra_reads = []
        for idx in range(16):
            p_ex = tmp / f"corner_ex_{idx}.lib"
            _write_swap_corner_lib(p_ex, f"lib_ex_{idx}", f"op_ex_{idx}", None, 1)
            extra_reads.append(f"read_liberty {p_ex}")

        out_mlib_swap = run_msta(
            tmp,
            f"""
read_liberty {tmp / 'swap_syn.lib'} {tmp / 'corner_b.lib'}
read_verilog {tmp / 'swap_syn.v'}
current_design swap_syn
read_sdc {tmp / 'swap_syn.sdc'}
swap_cell u_swap lib_b/AND2_A -verify
""" + "\n".join(extra_reads) + """
report_checks -setup -max_paths 2
report_summary
""",
            name="swap_mlib.dofile",
        )
        _assert_swap_ok(out_mlib_swap, "u_swap", "lib_b/AND2_A", expect_incr=True)
        assert "setup : WNS" in out_mlib_swap, out_mlib_swap

        out_bad_ign = run_msta(
            tmp,
            f"""
read_liberty {tmp / 'swap_syn.lib'} {tmp / 'corner_b.lib'}
read_verilog {tmp / 'swap_syn.v'}
current_design swap_syn
read_sdc {tmp / 'swap_syn.sdc'}
swap_cell u_swap IGN_PWR
""",
            name="bad_swap_ign.dofile",
            expect_ok=False,
        )
        assert "忽略单元" in out_bad_ign, out_bad_ign

        # 4e: Reject mismatched cell pin signatures cleanly without crashing
        run_msta(
            tmp,
            f"""
read_liberty {SKY130_LIB}
read_verilog {pipe_v}
current_design pipe_demo
read_sdc {pipe_sdc}
swap_cell _114_ sky130_fd_sc_hd__inv_1
""",
            name="bad_swap.dofile",
            expect_ok=False,
        )
    print("[PASS] Group 4: swap_cell -verify (permuted/shorted/corner pins, elmore_rc, pipe_demo, multi_vt_soc, fallback)")


if __name__ == "__main__":
    assert MSTA.exists(), f"missing binary {MSTA}"
    test_group1_elmore_rc_per_fanout()
    test_group2_pba_retrace_aocv()
    test_group3_standalone_c_plugin()
    test_group4_swap_cell_incremental()
    print("==> Phase 3 插件体系与增量 STA 检查全部通过（4 组）")
