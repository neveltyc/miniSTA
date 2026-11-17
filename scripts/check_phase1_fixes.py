#!/usr/bin/env python3
"""Phase 1 bug-fix & invariant regression suite (10 bug groups)."""
from pathlib import Path
import json
import math
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
MSTA = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build" / "msta"
OPENSTA = (
    Path(sys.argv[2])
    if len(sys.argv) > 2
    else ROOT / "vendor" / "opensta-build" / "sta"
)
SKY130_LIB = ROOT / "testcases" / "lib" / "sky130.lib"
ICS55_L_LIB = ROOT / "testcases" / "multi_vt" / "lib" / "ics55_LLSC_H7CL_typ_tt.lib"


def run_cmd(cmd, cwd):
    res = subprocess.run(
        cmd,
        cwd=str(cwd),
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=60,
    )
    return res.returncode, res.stdout


def run_msta(cwd, script_text, name="case.dofile", quiet=True):
    df = Path(cwd) / name
    df.write_text(script_text, encoding="utf-8")
    args = [str(MSTA)] + (["-q"] if quiet else []) + [str(df)]
    rc, out = run_cmd(args, cwd)
    assert rc == 0, f"msta exited {rc} on {name}:\n{out}"
    return out


def parse_msta_wns(out):
    ms = re.search(r"setup\s*:\s*WNS\s+(-?\d+\.\d+)\s+ns", out)
    mh = re.search(r"hold\s*:\s*WNS\s+(-?\d+\.\d+)\s+ns", out)
    setup = float(ms.group(1)) if ms else None
    hold = float(mh.group(1)) if mh else None
    return setup, hold


def run_opensta_wns(cwd, tcl_text):
    if not OPENSTA.is_file():
        return None, None
    f = Path(cwd) / "ref.tcl"
    f.write_text(tcl_text, encoding="utf-8")
    rc, out = run_cmd([str(OPENSTA), "-no_splash", "-no_init", "-exit", str(f)], cwd)
    assert rc == 0, f"OpenSTA failed ({rc}):\n{out}"
    m_max = re.findall(r"^\s*(-?\d+\.\d+)\s+slack\s+\(", out, re.M)
    assert len(m_max) == 2, f"unexpected OpenSTA output:\n{out}"
    return float(m_max[0]), float(m_max[1])


def assert_close(a, b, tol, label):
    assert a is not None and b is not None, f"{label}: missing value ({a}, {b})"
    diff = abs(a - b)
    assert diff <= tol, f"{label}: msta={a:.6f} ref={b:.6f} diff={diff:.6f} > {tol}"


# Shared minimal Liberty with INV, BUF, NAND2, OR2, DFF (rising CLK, pins D/CK/Q)
MIN_LIB = """library(min_phase1) {
  time_unit : "1ns";
  capacitive_load_unit (1, ff);
  lu_table_template(t2x2) {
    variable_1 : input_net_transition;
    variable_2 : total_output_net_capacitance;
    index_1("10, 100");
    index_2("5, 50");
  }
  cell(INV) {
    pin(A) { direction : input; capacitance : 2.0; }
    pin(Y) {
      direction : output; function : "!A";
      timing() {
        related_pin : "A"; timing_sense : negative_unate; timing_type : combinational;
        cell_rise(t2x2)  { index_1("0.01, 0.1"); index_2("5, 50"); values("0.10, 0.20", "0.15, 0.25"); }
        cell_fall(t2x2)  { index_1("0.01, 0.1"); index_2("5, 50"); values("0.20, 0.30", "0.25, 0.35"); }
        rise_transition(t2x2) { index_1("0.01, 0.1"); index_2("5, 50"); values("0.02, 0.04", "0.03, 0.05"); }
        fall_transition(t2x2) { index_1("0.01, 0.1"); index_2("5, 50"); values("0.02, 0.04", "0.03, 0.05"); }
      }
    }
  }
  cell(BUF) {
    pin(A) { direction : input; capacitance : 2.0; }
    pin(X) {
      direction : output; function : "A";
      timing() {
        related_pin : "A"; timing_sense : positive_unate; timing_type : combinational;
        cell_rise(t2x2)  { index_1("0.01, 0.1"); index_2("5, 50"); values("0.10, 0.20", "0.15, 0.25"); }
        cell_fall(t2x2)  { index_1("0.01, 0.1"); index_2("5, 50"); values("0.10, 0.20", "0.15, 0.25"); }
        rise_transition(t2x2) { index_1("0.01, 0.1"); index_2("5, 50"); values("0.02, 0.04", "0.03, 0.05"); }
        fall_transition(t2x2) { index_1("0.01, 0.1"); index_2("5, 50"); values("0.02, 0.04", "0.03, 0.05"); }
      }
    }
  }
  cell(SLOW) {
    pin(A) { direction : input; capacitance : 2.0; }
    pin(X) {
      direction : output; function : "A";
      timing() {
        related_pin : "A"; timing_sense : positive_unate; timing_type : combinational;
        cell_rise(t2x2)  { index_1("0.01, 0.1"); index_2("5, 50"); values("1.00, 1.10", "1.05, 1.15"); }
        cell_fall(t2x2)  { index_1("0.01, 0.1"); index_2("5, 50"); values("1.00, 1.10", "1.05, 1.15"); }
        rise_transition(t2x2) { index_1("0.01, 0.1"); index_2("5, 50"); values("0.02, 0.04", "0.03, 0.05"); }
        fall_transition(t2x2) { index_1("0.01, 0.1"); index_2("5, 50"); values("0.02, 0.04", "0.03, 0.05"); }
      }
    }
  }
  cell(MED) {
    pin(A) { direction : input; capacitance : 2.0; }
    pin(X) {
      direction : output; function : "A";
      timing() {
        related_pin : "A"; timing_sense : positive_unate; timing_type : combinational;
        cell_rise(t2x2)  { index_1("0.01, 0.1"); index_2("5, 50"); values("0.50, 0.60", "0.55, 0.65"); }
        cell_fall(t2x2)  { index_1("0.01, 0.1"); index_2("5, 50"); values("0.50, 0.60", "0.55, 0.65"); }
        rise_transition(t2x2) { index_1("0.01, 0.1"); index_2("5, 50"); values("0.02, 0.04", "0.03, 0.05"); }
        fall_transition(t2x2) { index_1("0.01, 0.1"); index_2("5, 50"); values("0.02, 0.04", "0.03, 0.05"); }
      }
    }
  }
  cell(OR2) {
    pin(A) { direction : input; capacitance : 2.0; }
    pin(B) { direction : input; capacitance : 2.0; }
    pin(X) {
      direction : output; function : "A|B";
      timing() {
        related_pin : "A"; timing_sense : positive_unate; timing_type : combinational;
        cell_rise(t2x2) { index_1("0.01, 0.1"); index_2("5, 50"); values("0.10, 0.20", "0.15, 0.25"); }
        cell_fall(t2x2) { index_1("0.01, 0.1"); index_2("5, 50"); values("0.10, 0.20", "0.15, 0.25"); }
      }
      timing() {
        related_pin : "B"; timing_sense : positive_unate; timing_type : combinational;
        cell_rise(t2x2) { index_1("0.01, 0.1"); index_2("5, 50"); values("0.10, 0.20", "0.15, 0.25"); }
        cell_fall(t2x2) { index_1("0.01, 0.1"); index_2("5, 50"); values("0.10, 0.20", "0.15, 0.25"); }
      }
    }
  }
  cell(DFF) {
    ff(IQ, IQN) { clocked_on : "CK"; next_state : "D"; }
    pin(CK) { direction : input; capacitance : 2.0; clock : true; }
    pin(D) {
      direction : input; capacitance : 2.0;
      timing() {
        related_pin : "CK"; timing_type : setup_rising;
        rise_constraint(scalar) { values("0.10"); }
        fall_constraint(scalar) { values("0.10"); }
      }
      timing() {
        related_pin : "CK"; timing_type : hold_rising;
        rise_constraint(scalar) { values("0.05"); }
        fall_constraint(scalar) { values("0.05"); }
      }
    }
    pin(Q) {
      direction : output; function : "IQ";
      timing() {
        related_pin : "CK"; timing_type : rising_edge;
        cell_rise(scalar) { values("0.20"); }
        cell_fall(scalar) { values("0.20"); }
        rise_transition(scalar) { values("0.03"); }
        fall_transition(scalar) { values("0.03"); }
      }
    }
  }
}
"""


def test_group1_netlist_blackbox_bus(tmp: Path):
    # >8 blackboxes (12 distinct unknown modules) + top-port blackbox direction inference
    (tmp / "min.lib").write_text(MIN_LIB, encoding="utf-8")
    insts = []
    wires = []
    outs = []
    for k in range(12):
        wires.append(f"  wire bw{k};")
        outs.append(f"out_{k}")
        insts.append(f"  BB_MOD_{k} u_bb_{k} (.A(in_p), .Z(bw{k}));")
        insts.append(f"  DFF u_ff_{k} (.CK(clk), .D(bw{k}), .Q(out_{k}));")
    (tmp / "bb.v").write_text(
        f"module top(clk, in_p, {', '.join(outs)}, out_direct);\n"
        f"  input clk, in_p; output {', '.join(outs)}, out_direct;\n"
        + "\n".join(wires)
        + "\n  BB_DIRECT u_bb_top (.A(in_p), .Z(out_direct));\n"
        + "\n".join(insts)
        + "\nendmodule\n",
        encoding="utf-8",
    )
    (tmp / "bb.sdc").write_text(
        "create_clock -name clk -period 10.0 [get_ports clk]\n"
        "set_input_delay -clock clk 1.0 [get_ports in_p]\n"
        f"set_output_delay -clock clk 1.0 [get_ports {{{' '.join(outs)} out_direct}}]\n",
        encoding="utf-8",
    )
    out = run_msta(
        tmp,
        "read_liberty min.lib\nread_verilog bb.v\ncurrent_design top\n"
        "read_sdc bb.sdc\nreport_checks -setup\nreport_checks -hold\n",
        "g1_bb.dofile",
        quiet=False,
    )
    assert "black boxes: 13" in out, out
    s, h = parse_msta_wns(out)
    assert_close(s, 8.0, 1e-3, "group1_bb_setup")
    assert_close(h, 0.95, 1e-3, "group1_bb_hold")

    # Yosys JSON upto=1 + offset=4 bus netnames & top ports
    yosys_json = {
        "modules": {
            "up_top": {
                "attributes": {"top": "00000000000000000000000000000001"},
                "ports": {
                    "clk": {"direction": "input", "bits": [2]},
                    "bus_in": {"direction": "input", "offset": 4, "upto": 1, "bits": [3, 4]},
                    "bus_out": {"direction": "output", "offset": 4, "upto": 1, "bits": [5, 6]},
                },
                "cells": {
                    "u0": {"type": "BUF", "port_directions": {"A": "input", "X": "output"}, "connections": {"A": [3], "X": [7]}},
                    "u_ff0": {"type": "DFF", "port_directions": {"CK": "input", "D": "input", "Q": "output"}, "connections": {"CK": [2], "D": [7], "Q": [5]}},
                    "u_ff1": {"type": "DFF", "port_directions": {"CK": "input", "D": "input", "Q": "output"}, "connections": {"CK": [2], "D": [4], "Q": [6]}},
                },
                "netnames": {
                    "clk": {"bits": [2]},
                    "bus_in": {"offset": 4, "upto": 1, "bits": [3, 4]},
                    "bus_out": {"offset": 4, "upto": 1, "bits": [5, 6]},
                    "w_mid": {"offset": 4, "upto": 1, "bits": [7, 4]},
                },
            }
        }
    }
    (tmp / "upto.json").write_text(json.dumps(yosys_json), encoding="utf-8")
    (tmp / "upto.sdc").write_text(
        "create_clock -name clk -period 10.0 [get_ports clk]\n"
        "set_input_delay -clock clk 2.0 [get_ports {bus_in[4] bus_in[5]}]\n"
        "set_output_delay -clock clk 1.0 [get_ports {bus_out[4] bus_out[5]}]\n",
        encoding="utf-8",
    )
    out2 = run_msta(
        tmp,
        "read_liberty min.lib\nread_json upto.json\ncurrent_design up_top\n"
        "read_sdc upto.sdc\nreport_checks -setup -hold\n",
        "g1_upto.dofile",
    )
    assert "警告：sdc" not in out2, out2
    s2, _ = parse_msta_wns(out2)
    # Bit 4 goes through BUF -> DFF (7 + 0.1 + 0.1 = 2.2), setup slack 10 - 2.2 = 7.8; output bit 4/5: 10 - 1.2 = 8.8
    assert s2 is not None and s2 < 8.5, out2
    print("[PASS] Group 1: >8 blackboxes & port inference + Yosys upto=1/offset bus")


def test_group2_reload_units_comments(tmp: Path):
    (tmp / "min.lib").write_text(MIN_LIB, encoding="utf-8")
    (tmp / "d.v").write_text(
        "module d(clk, a, y); input clk, a; output y; wire q; "
        "BUF u1(.A(a), .X(q)); DFF ff(.CK(clk), .D(q), .Q(y)); endmodule\n",
        encoding="utf-8",
    )
    # sdc_ps switches to ps; sdc_def must use Liberty default ns (NOT leak ps!)
    (tmp / "sdc_ps.sdc").write_text(
        "set_units -time ps -capacitance pF\n"
        "create_clock -name clk -period 10000 [get_ports clk]\n"
        "set_input_delay -clock clk 1000 [get_ports a]\n"
        "set_output_delay -clock clk 1000 [get_ports y]\n",
        encoding="utf-8",
    )
    (tmp / "sdc_def.sdc").write_text(
        "set_input_delay -clock clk 3.0 [get_ports a]\n"
        "set_output_delay -clock clk 1.0 [get_ports y]\n",
        encoding="utf-8",
    )
    out = run_msta(
        tmp,
        "# late read_liberty after current_design, repeated current_design, ; in comments/quotes\n"
        "read_verilog d.v\n"
        "current_design d\n"
        "read_liberty min.lib\n"
        "read_sdc sdc_ps.sdc\n"
        "report_checks -setup\n"
        "current_design d ; # comment with;semicolons\n"
        "read_sdc \"sdc_ps.sdc\" # trailing;note\n"
        "read_sdc sdc_def.sdc\n"
        "report_checks -setup\n",
        "g2.dofile",
    )
    assert "未知命令" not in out, out
    vals = [float(m.group(1)) for m in re.finditer(r"setup\s*:\s*WNS\s+(-?\d+\.\d+)\s+ns", out)]
    assert len(vals) == 2, out
    # First run: input 1.0ns; Second run: 3.0ns overrides 1.0ns -> WNS strictly drops by 2.0ns
    assert_close(vals[0] - vals[1], 2.0, 1e-3, "group2_multi_sdc_unit_isolation")
    print("[PASS] Group 2: late read_liberty + re-current_design + multi-SDC unit isolation + dofile ;")


def test_group3_drc_unset(tmp: Path):
    (tmp / "min.lib").write_text(MIN_LIB, encoding="utf-8")
    (tmp / "drc.v").write_text(
        "module top(clk, a, y1, y2); input clk, a; output y1, y2; "
        "BUF u1(.A(a), .X(y1)); UNKNOWN_BB ubb(.A(a), .Z(y2)); endmodule\n",
        encoding="utf-8",
    )
    (tmp / "drc.sdc").write_text(
        "create_clock -name clk -period 10.0 [get_ports clk]\n"
        "set_input_delay -clock clk 1.0 [get_ports a]\n"
        "set_output_delay -clock clk 1.0 [get_ports {y1 y2}]\n"
        "set_max_fanout 10 [current_design]\n",
        encoding="utf-8",
    )
    out = run_msta(
        tmp,
        "read_liberty min.lib\nread_verilog drc.v\ncurrent_design top\n"
        "read_sdc drc.sdc\nreport_checks -setup\n",
        "g3.dofile",
    )
    assert "min_capacitance 违例 0 处" in out, out
    assert "max_capacitance 违例 0 处" in out, out
    assert "max_transition 违例 0 处" in out, out
    print("[PASS] Group 3: blackbox & unset Liberty DRC guards (0 false violations)")


def test_group4_liberty_opcond_nan(tmp: Path):
    lib = """library(sci_lib) {
  time_unit : "1e-9s";
  capacitive_load_unit (1.0, "PF");
  nom_voltage : 1.0;
  nom_temperature : 25.0;
  default_operating_conditions : cold_neg;
  operating_conditions(cold_neg) {
    voltage : 1.2;
    temperature : -40.0;
    k_volt : -0.5;
    k_temp : 0.001;
  }
  lu_table_template(load_first) {
    variable_1 : total_output_net_capacitance;
    variable_2 : input_transition_time;
    index_1("0.005, 0.050");
    index_2("0.01, 0.10");
  }
  cell(INV) {
    pin(A) { direction : input; capacitance : 0.002; }
    pin(Y) {
      direction : output; function : "!A";
      timing() {
        related_pin : "A"; timing_sense : negative_unate; timing_type : combinational;
        cell_rise(load_first) { values("1.0, 1.0", "1.0, 1.0"); }
        cell_fall(load_first) { values("1.0, 1.0", "1.0, 1.0"); }
      }
    }
  }
  cell(FFN) {
    ff(IQ, IQN) { clocked_on : "!CK"; next_state : "D"; }
    pin(CK) { direction : input; capacitance : 0.002; }
    pin(D) {
      direction : input; capacitance : 0.002;
      timing() {
        related_pin : "CK"; timing_type : setup_falling;
        rise_constraint(scalar) { values("0.20"); }
        fall_constraint(scalar) { values("0.20"); }
      }
    }
    pin(Q) {
      direction : output; function : "IQ";
      timing() {
        related_pin : "CK"; timing_type : falling_edge;
        cell_rise(scalar) { values("0.50"); }
        cell_fall(scalar) { values("0.50"); }
      }
    }
  }
}
"""
    (tmp / "sci.lib").write_text(lib, encoding="utf-8")
    (tmp / "sci.v").write_text(
        "module top(clk, d, q); input clk, d; output q; wire w; "
        "INV u1(.A(d), .Y(w)); FFN ff(.CK(clk), .D(w), .Q(q)); endmodule\n",
        encoding="utf-8",
    )
    (tmp / "sci.sdc").write_text(
        "create_clock -name clk -period 10.0 [get_ports clk]\n"
        "create_clock -name bad1 -period NaN [get_ports clk]\n"
        "create_clock -name bad2 -period 10 -waveform {0 Inf} [get_ports clk]\n"
        "set_input_delay -clock clk nan [get_ports d]\n"
        "set_operating_conditions cold_neg\n"
        "set_input_delay -clock clk -clock_fall 1.0 [get_ports d]\n"
        "set_output_delay -clock clk -clock_fall 1.0 [get_ports q]\n",
        encoding="utf-8",
    )
    out = run_msta(
        tmp,
        "read_liberty sci.lib\nread_verilog sci.v\ncurrent_design top\n"
        "read_sdc sci.sdc\nreport_checks -setup -max_paths 2\n",
        "g4.dofile",
    )
    assert "3 条命令未建模或写错，已忽略" in out, out
    # K factor = 1 + (-0.5)*(1.2-1.0) + 0.001*(-40-25) = 0.835
    # delay(INV)=1.0*0.835=0.835, setup=0.20*0.835=0.167
    # path d->ff/D: launch(negedge)=5.0, arrival=6.0 + 0.835, capture=15.0, req=15.0-0.167=14.833 => slack=7.998
    assert "寄存器 1 个" in out, out
    s, _ = parse_msta_wns(out)
    assert_close(s, 7.998, 2e-3, "group4_neg_kvolt_cold_temp")
    print("[PASS] Group 4: 1e-9s/PF units + !CK + negative k_volt/-40C + NaN/Inf rejection")


def test_group5_multi_arc_when_opensta(tmp: Path):
    # 5a: sky130 xor2_1 + fa_1 + mux2_1 vs OpenSTA
    (tmp / "sky.v").write_text(
        "module top(clk, a, b, cin, sel, q_out);\n"
        "  input clk, a, b, cin, sel; output q_out;\n"
        "  wire xw, fw_c, fw_s, mw, ff_a, ff_b, ff_s;\n"
        "  sky130_fd_sc_hd__dfxtp_1 r_a(.CLK(clk), .D(a), .Q(ff_a));\n"
        "  sky130_fd_sc_hd__dfxtp_1 r_b(.CLK(clk), .D(b), .Q(ff_b));\n"
        "  sky130_fd_sc_hd__dfxtp_1 r_s(.CLK(clk), .D(sel), .Q(ff_s));\n"
        "  sky130_fd_sc_hd__xor2_1 u_xor(.A(ff_a), .B(ff_b), .X(xw));\n"
        "  sky130_fd_sc_hd__fa_1 u_fa(.A(xw), .B(ff_b), .CIN(cin), .COUT(fw_c), .SUM(fw_s));\n"
        "  sky130_fd_sc_hd__mux2_1 u_mux(.A0(fw_c), .A1(fw_s), .S(ff_s), .X(mw));\n"
        "  sky130_fd_sc_hd__dfxtp_1 r_y(.CLK(clk), .D(mw), .Q(q_out));\n"
        "endmodule\n",
        encoding="utf-8",
    )
    (tmp / "sky.sdc").write_text(
        "create_clock -name clk -period 10.0 [get_ports clk]\n"
        "set_clock_transition 0.05 [get_clocks clk]\n"
        "set_input_transition 0.05 [get_ports {a b cin sel}]\n"
        "set_input_delay -clock clk -max 1.0 [get_ports {a b cin sel}]\n"
        "set_input_delay -clock clk -min 0.2 [get_ports {a b cin sel}]\n"
        "set_output_delay -clock clk -max 1.0 [get_ports q_out]\n"
        "set_output_delay -clock clk -min 0.2 [get_ports q_out]\n"
        "set_load 0.005 [get_ports q_out]\n",
        encoding="utf-8",
    )
    out = run_msta(
        tmp,
        f"read_liberty {SKY130_LIB}\nread_verilog sky.v\ncurrent_design top\n"
        "read_sdc sky.sdc\nreport_checks -setup -hold\n",
        "g5a.dofile",
    )
    ms, mh = parse_msta_wns(out)
    rs, rh = run_opensta_wns(
        tmp,
        f"read_liberty {SKY130_LIB}\nread_verilog sky.v\nlink_design top\n"
        "read_sdc sky.sdc\nreport_checks -path_delay max -digits 4\n"
        "report_checks -path_delay min -digits 4\n",
    )
    assert_close(ms, rs, 0.005, "group5_sky130_multi_arc_setup")
    assert_close(mh, rh, 0.005, "group5_sky130_multi_arc_hold")

    # 5b: ics55 AO211X0P5H7L when-condition pruning with set_case_analysis
    (tmp / "ao.v").write_text(
        "module top(clk, a0, a1, b0, c0, y);\n"
        "  input clk, a0, a1, b0, c0; output y;\n"
        "  wire q_b, ao_y;\n"
        "  DFFQX1H7L ff_in(.CK(clk), .D(b0), .Q(q_b));\n"
        "  AO211X0P5H7L u_ao(.A0(a0), .A1(a1), .B0(q_b), .C0(c0), .Y(ao_y));\n"
        "  DFFQX1H7L ff_out(.CK(clk), .D(ao_y), .Q(y));\n"
        "endmodule\n",
        encoding="utf-8",
    )
    s_vals, h_vals = [], []
    for idx, (ca0, ca1) in enumerate([(1, 0), (0, 1)]):
        sdc_p = tmp / f"ao_{idx}.sdc"
        sdc_p.write_text(
            "create_clock -name clk -period 10.0 [get_ports clk]\n"
            "set_clock_transition 0.05 [get_clocks clk]\n"
            "set_input_transition 0.05 [get_ports {a0 a1 b0 c0}]\n"
            "set_input_delay -clock clk 0.5 [get_ports {a0 a1 b0 c0}]\n"
            "set_output_delay -clock clk 0.1 [get_ports y]\n"
            "set_load 0.005 [get_ports y]\n"
            f"set_case_analysis {ca0} [get_ports a0]\n"
            f"set_case_analysis {ca1} [get_ports a1]\n"
            "set_case_analysis 0 [get_ports c0]\n",
            encoding="utf-8",
        )
        out_i = run_msta(
            tmp,
            f"read_liberty {ICS55_L_LIB}\nread_verilog ao.v\ncurrent_design top\n"
            f"read_sdc ao_{idx}.sdc\nreport_checks -setup -hold\n",
            f"g5b_{idx}.dofile",
        )
        msi, mhi = parse_msta_wns(out_i)
        rsi, rhi = run_opensta_wns(
            tmp,
            f"read_liberty {ICS55_L_LIB}\nread_verilog ao.v\nlink_design top\n"
            f"read_sdc ao_{idx}.sdc\nreport_checks -path_delay max -digits 4\n"
            "report_checks -path_delay min -digits 4\n",
        )
        assert_close(msi, rsi, 0.005, f"group5_ao_when_{idx}_setup")
        assert_close(mhi, rhi, 0.005, f"group5_ao_when_{idx}_hold")
        s_vals.append(msi)
        h_vals.append(mhi)
    print(f"[PASS] Group 5: multi-arc + when pruning vs OpenSTA (when setup {s_vals})")


def test_group6_driving_cell_opensta(tmp: Path):
    # miniSTA documents set_driving_cell as adding one full driver stage ("把外部驱动单元当成一级"),
    # whereas OpenSTA subtracts zero-load intrinsic delay on the driving cell itself; check BOTH:
    # (1) output slew propagated to downstream stages (b1/X, b2/X) matches OpenSTA <= 0.002 ns, and
    # (2) WNS matches an explicit inv_1/buf_1 stage in both msta and OpenSTA <= 0.005 ns.
    (tmp / "drv.v").write_text(
        "module top(clk, in_inv, in_buf, y1, y2);\n"
        "  input clk, in_inv, in_buf; output y1, y2;\n"
        "  wire w1, w2;\n"
        "  sky130_fd_sc_hd__buf_1 b1(.A(in_inv), .X(w1));\n"
        "  sky130_fd_sc_hd__buf_1 b2(.A(in_buf), .X(w2));\n"
        "  sky130_fd_sc_hd__dfxtp_1 r1(.CLK(clk), .D(w1), .Q(y1));\n"
        "  sky130_fd_sc_hd__dfxtp_1 r2(.CLK(clk), .D(w2), .Q(y2));\n"
        "endmodule\n",
        encoding="utf-8",
    )
    (tmp / "drv.sdc").write_text(
        "create_clock -name clk -period 10.0 [get_ports clk]\n"
        "set_clock_transition 0.05 [get_clocks clk]\n"
        "set_input_delay -clock clk -max 1.0 [get_ports {in_inv in_buf}]\n"
        "set_input_delay -clock clk -min 0.2 [get_ports {in_inv in_buf}]\n"
        "set_output_delay -clock clk 0.5 [get_ports {y1 y2}]\n"
        "set_load 0.005 [get_ports {y1 y2}]\n"
        "set_driving_cell -lib_cell sky130_fd_sc_hd__inv_1 -pin Y -from_pin A "
        "-input_transition_rise 0.02 -input_transition_fall 0.40 [get_ports in_inv]\n"
        "set_driving_cell -lib_cell sky130_fd_sc_hd__buf_1 -pin X -from_pin A [get_ports in_buf]\n",
        encoding="utf-8",
    )
    out = run_msta(
        tmp,
        f"read_liberty {SKY130_LIB}\nread_verilog drv.v\ncurrent_design top\n"
        "read_sdc drv.sdc\nreport_checks -setup -max_paths 4\nreport_checks -hold -max_paths 4\n",
        "g6.dofile",
    )
    ms, mh = parse_msta_wns(out)
    # Explicit equivalent driver stage (in_inv -> inv_1 -> b1, in_buf -> buf_1 -> b2)
    (tmp / "drv_exp.v").write_text(
        "module top(clk, in_inv, in_buf, y1, y2);\n"
        "  input clk, in_inv, in_buf; output y1, y2;\n"
        "  wire p1, p2, w1, w2;\n"
        "  sky130_fd_sc_hd__inv_1 d1(.A(in_inv), .Y(p1));\n"
        "  sky130_fd_sc_hd__buf_1 d2(.A(in_buf), .X(p2));\n"
        "  sky130_fd_sc_hd__buf_1 b1(.A(p1), .X(w1));\n"
        "  sky130_fd_sc_hd__buf_1 b2(.A(p2), .X(w2));\n"
        "  sky130_fd_sc_hd__dfxtp_1 r1(.CLK(clk), .D(w1), .Q(y1));\n"
        "  sky130_fd_sc_hd__dfxtp_1 r2(.CLK(clk), .D(w2), .Q(y2));\n"
        "endmodule\n",
        encoding="utf-8",
    )
    (tmp / "drv_exp.sdc").write_text(
        "create_clock -name clk -period 10.0 [get_ports clk]\n"
        "set_clock_transition 0.05 [get_clocks clk]\n"
        "set_input_delay -clock clk -max 1.0 [get_ports {in_inv in_buf}]\n"
        "set_input_delay -clock clk -min 0.2 [get_ports {in_inv in_buf}]\n"
        "set_input_transition -rise 0.02 [get_ports in_inv]\n"
        "set_input_transition -fall 0.40 [get_ports in_inv]\n"
        "set_input_transition 0.0 [get_ports in_buf]\n"
        "set_output_delay -clock clk 0.5 [get_ports {y1 y2}]\n"
        "set_load 0.005 [get_ports {y1 y2}]\n",
        encoding="utf-8",
    )
    rs, rh = run_opensta_wns(
        tmp,
        f"read_liberty {SKY130_LIB}\nread_verilog drv_exp.v\nlink_design top\n"
        "read_sdc drv_exp.sdc\nreport_checks -path_delay max -digits 4\n"
        "report_checks -path_delay min -digits 4\n",
    )
    assert_close(ms, rs, 0.005, "group6_driving_cell_explicit_setup")
    assert_close(mh, rh, 0.005, "group6_driving_cell_explicit_hold")
    if OPENSTA.is_file():
        # Verify OpenSTA set_driving_cell downstream gate (b1/X) delay matches msta's b1/X stage delay!
        f = tmp / "ref_slew.tcl"
        f.write_text(
            f"read_liberty {SKY130_LIB}\nread_verilog drv.v\nlink_design top\n"
            "read_sdc drv.sdc\nreport_checks -from [get_ports in_inv] -path_delay max -digits 4\n",
            encoding="utf-8",
        )
        rc, o_out = run_cmd([str(OPENSTA), "-no_splash", "-no_init", "-exit", str(f)], tmp)
        assert rc == 0, o_out
        m_ref_b1 = re.search(r"^\s*([0-9.]+)\s+[0-9.]+\s+[\^v]\s+b1/X\b", o_out, re.M)
        setup_part = out.split("===== setup path", 1)[1] if "===== setup path" in out else out
        m_msta_b1 = re.search(r"^\s*b1/X\s+([0-9.]+)\s+[0-9.]+", setup_part, re.M)
        assert m_ref_b1 and m_msta_b1, (o_out, out)
        assert_close(float(m_msta_b1.group(1)), float(m_ref_b1.group(1)), 0.002, "group6_downstream_slew_delay")
    print("[PASS] Group 6: set_driving_cell INV/BUF + asymmetric slew vs OpenSTA")


def test_group7_budget_and_multicycle(tmp: Path):
    (tmp / "min.lib").write_text(MIN_LIB, encoding="utf-8")
    (tmp / "mc.v").write_text(
        "module top(clk_a, clk_b, in_d, y);\n"
        "  input clk_a, clk_b, in_d; output y;\n"
        "  wire w; BUF u1(.A(in_d), .X(w)); DFF r1(.CK(clk_b), .D(w), .Q(y));\n"
        "endmodule\n",
        encoding="utf-8",
    )
    (tmp / "mc.sdc").write_text(
        "create_clock -name clk_a -period 5.0 [get_ports clk_a]\n"
        "create_clock -name clk_b -period 4.0 [get_ports clk_b]\n"
        "set_input_delay -clock clk_a 1.0 [get_ports in_d]\n"
        "set_output_delay -clock clk_b -max 0.5 [get_ports y]\n"
        "set_output_delay -clock clk_b -min 2.0 [get_ports y]\n"
        "set_multicycle_path 2 -setup -from [get_ports in_d] -to [get_pins r1/D]\n"
        "set_min_delay 0.3 -from [get_ports in_d] -to [get_pins r1/D]\n",
        encoding="utf-8",
    )
    out = run_msta(
        tmp,
        "read_liberty min.lib\nread_verilog mc.v\ncurrent_design top\n"
        "read_sdc mc.sdc\nreport_checks -setup -max_paths 4\nreport_checks -hold -max_paths 4\n",
        "g7.dofile",
    )
    assert "+ min_delay" in out, out
    assert "launch edge" in out, out
    assert "capture clock: clk_b @ 20.000 ns" in out, out  # cross-clock 15->16 + multicycle 2 => 20.000!
    _, h = parse_msta_wns(out)
    assert_close(h, 0.816, 1e-3, "group7_min_delay_hold")
    print("[PASS] Group 7: set_multicycle_path + set_min_delay coexistence & path-budget report")


def test_group8_through_isolation_async(tmp: Path):
    (tmp / "min.lib").write_text(MIN_LIB, encoding="utf-8")
    # 8a: Reconvergent load-pin -through & -rise_through on INV & -from ff/CLK
    (tmp / "pin.v").write_text(
        "module top(clk, a, b, y, y2);\n"
        "  input clk, a, b; output y, y2;\n"
        "  wire q0, s_net, b_inv, f_buf, comb;\n"
        "  DFF ff0(.CK(clk), .D(a), .Q(q0));\n"
        "  BUF u_fast(.A(q0), .X(f_buf));\n"
        "  SLOW u_pre(.A(q0), .X(s_net));\n"
        "  INV u_inv(.A(s_net), .Y(b_inv));\n"
        "  OR2 u_or(.A(f_buf), .B(b_inv), .X(comb));\n"
        "  DFF ff1(.CK(clk), .D(comb), .Q(y));\n"
        "  DFF ff_o(.CK(clk), .D(b), .Q(y2));\n"
        "endmodule\n",
        encoding="utf-8",
    )
    # Test 1: -through u_pre/A cuts slow branch on shared net q0 while fast branch u_fast/A survives!
    (tmp / "p1.sdc").write_text(
        "create_clock -name clk -period 10.0 [get_ports clk]\n"
        "set_input_delay -clock clk 0.1 [get_ports {a b}]\n"
        "set_output_delay -clock clk 0.1 [get_ports {y y2}]\n"
        "set_false_path -through [get_pins u_pre/A]\n",
        encoding="utf-8",
    )
    out1 = run_msta(
        tmp,
        "read_liberty min.lib\nread_verilog pin.v\ncurrent_design top\n"
        "read_sdc p1.sdc\nreport_checks -setup -max_paths 5\nreport_checks -hold -max_paths 5\n",
        "g8a_1.dofile",
    )
    assert "未约束 0 个" in out1, out1
    assert "u_pre" not in out1, out1
    assert "u_fast" in out1, out1
    # Fast branch: ff0(0.2) + u_fast(0.116) + OR2(0.116) + setup(0.1) = 0.532 => slack ~9.47
    s1, _ = parse_msta_wns(out1)
    assert_close(s1, 9.468, 0.05, "group8a_load_pin_through")

    # Test 1b: -rise_through on INV input A cuts ONLY rising A (falling Y), keeping falling A (rising Y)
    (tmp / "p1_inv.sdc").write_text(
        "create_clock -name clk -period 10.0 [get_ports clk]\n"
        "set_input_delay -clock clk 0.1 [get_ports {a b}]\n"
        "set_output_delay -clock clk 0.1 [get_ports {y y2}]\n"
        "set_false_path -rise_through [get_pins u_inv/A]\n",
        encoding="utf-8",
    )
    out1b = run_msta(
        tmp,
        "read_liberty min.lib\nread_verilog pin.v\ncurrent_design top\n"
        "read_sdc p1_inv.sdc\nreport_checks -setup -max_paths 1\n",
        "g8a_1b.dofile",
    )
    assert "未约束 0 个" in out1b, out1b
    assert "u_inv/Y" in out1b and "data edge    : rise (^)" in out1b, out1b

    # Test 2: -from [get_pins ff0/CK] cuts ONLY ff0 paths, leaving ff_o -> y2 intact
    (tmp / "p2.sdc").write_text(
        "create_clock -name clk -period 10.0 [get_ports clk]\n"
        "set_input_delay -clock clk 1.0 [get_ports {a b}]\n"
        "set_output_delay -clock clk 1.0 [get_ports {y y2}]\n"
        "set_false_path -from [get_pins ff0/CK]\n",
        encoding="utf-8",
    )
    out2 = run_msta(
        tmp,
        "read_liberty min.lib\nread_verilog pin.v\ncurrent_design top\n"
        "read_sdc p2.sdc\nreport_checks -setup -max_paths 5\n",
        "g8a_2.dofile",
    )
    assert "路径例外排除 1 个" in out2 and "startpoint : ff0" not in out2 and "endpoint   : ff_o" in out2, out2

    # 8b: multi-through -through A -through B keeps A->!B and !A->B
    # Build diamond: d -> {a1(SLOW), a2(BUF)} -> OR2 -> {b1(SLOW), b2(MED)} -> r_out
    (tmp / "mt2.v").write_text(
        "module top(clk, d, y);\n"
        "  input clk, d; output y;\n"
        "  wire wa1, wa2, wjoin, wb1, wb2, wend;\n"
        "  SLOW a1(.A(d), .X(wa1));\n"
        "  BUF  a2(.A(d), .X(wa2));\n"
        "  OR2  mid(.A(wa1), .B(wa2), .X(wjoin));\n"
        "  SLOW b1(.A(wjoin), .X(wb1));\n"
        "  MED  b2(.A(wjoin), .X(wb2));\n"
        "  OR2  endg(.A(wb1), .B(wb2), .X(wend));\n"
        "  DFF  r_out(.CK(clk), .D(wend), .Q(y));\n"
        "endmodule\n",
        encoding="utf-8",
    )
    (tmp / "mt2.sdc").write_text(
        "create_clock -name clk -period 10.0 [get_ports clk]\n"
        "set_input_delay -clock clk 1.0 [get_ports d]\n"
        "set_output_delay -clock clk 0.5 [get_ports y]\n"
        "set_false_path -through [get_pins a1/X] -through [get_pins b1/X]\n",
        encoding="utf-8",
    )
    out_mt = run_msta(
        tmp,
        "read_liberty min.lib\nread_verilog mt2.v\ncurrent_design top\n"
        "read_sdc mt2.sdc\nreport_checks -setup\n",
        "g8b.dofile",
    )
    # Without a1->b1, worst surviving setup paths are: a1->b2 (SLOW+OR2+MED+OR2) vs a2->b1 (BUF+OR2+SLOW+OR2)!
    # a2->b1 is strictly slower (b1=1.0 > b2=0.5 even though a2=0.1 < a1=1.0? wait: a1+b2=1.0+0.5=1.5 == a2+b1=0.1+1.0=1.1 -> a1+b2 wins! Look: a1(1.0)+mid+b2(0.5)+endg = 1.7 + 1.0 + 0.1 = 2.8 => slack ~7.2!)
    assert "未约束 0 个" in out_mt, out_mt
    assert "a1/X" in out_mt or "b1/X" in out_mt, out_mt
    smt, _ = parse_msta_wns(out_mt)
    assert smt is not None and 7.0 < smt < 7.5, f"unexpected multi-through WNS {smt}:\n{out_mt}"

    # 8c: Setup false path must not alter hold WNS
    (tmp / "iso_base.sdc").write_text(
        "create_clock -name clk -period 10.0 [get_ports clk]\n"
        "set_input_delay -clock clk 1.0 [get_ports d]\n"
        "set_output_delay -clock clk 0.5 [get_ports y]\n",
        encoding="utf-8",
    )
    (tmp / "iso_cut.sdc").write_text(
        "create_clock -name clk -period 10.0 [get_ports clk]\n"
        "set_input_delay -clock clk 1.0 [get_ports d]\n"
        "set_output_delay -clock clk 0.5 [get_ports y]\n"
        "set_false_path -setup -through [get_pins a1/X]\n",
        encoding="utf-8",
    )
    out_ib = run_msta(tmp, "read_liberty min.lib\nread_verilog mt2.v\ncurrent_design top\nread_sdc iso_base.sdc\nreport_checks -setup -hold\n", "g8c_b.dofile")
    out_ic = run_msta(tmp, "read_liberty min.lib\nread_verilog mt2.v\ncurrent_design top\nread_sdc iso_cut.sdc\nreport_checks -setup -hold\n", "g8c_c.dofile")
    _, hb = parse_msta_wns(out_ib)
    _, hc = parse_msta_wns(out_ic)
    assert_close(hb, hc, 1e-6, "group8c_setup_hold_isolation")

    # 8d: Async recovery/removal -through retry (sky130 dfrtp_1)
    (tmp / "async.v").write_text(
        "module top(clk, rst, d, y);\n"
        "  input clk, rst, d; output y;\n"
        "  wire s1, s2, f1, rst_n;\n"
        "  sky130_fd_sc_hd__buf_1 bs1(.A(rst), .X(s1));\n"
        "  sky130_fd_sc_hd__buf_1 bs2(.A(s1), .X(s2));\n"
        "  sky130_fd_sc_hd__buf_1 bf1(.A(rst), .X(f1));\n"
        "  sky130_fd_sc_hd__mux2_1 um(.A0(s2), .A1(f1), .S(d), .X(rst_n));\n"
        "  sky130_fd_sc_hd__dfrtp_1 ff(.CLK(clk), .D(d), .RESET_B(rst_n), .Q(y));\n"
        "endmodule\n",
        encoding="utf-8",
    )
    (tmp / "async.sdc").write_text(
        "create_clock -name clk -period 10.0 [get_ports clk]\n"
        "set_input_delay -clock clk 1.0 [get_ports {rst d}]\n"
        "set_output_delay -clock clk 0.5 [get_ports y]\n"
        "set_false_path -through [get_pins bs1/X]\n",
        encoding="utf-8",
    )
    out_as = run_msta(
        tmp,
        f"read_liberty {SKY130_LIB}\nread_verilog async.v\ncurrent_design top\n"
        "read_sdc async.sdc\nreport_checks -setup -max_paths 5\nreport_checks -hold -max_paths 5\n",
        "g8d.dofile",
    )
    assert "未约束 0 个" in out_as, out_as
    assert "recovery path" in out_as, out_as
    assert "bs1" not in out_as, out_as
    print("[PASS] Group 8: pin-level -through, -from ff/CLK, multi-through, setup/hold isolation, async retry")


def test_group9_seq_genclk_aocv(tmp: Path):
    (tmp / "min.lib").write_text(MIN_LIB, encoding="utf-8")
    (tmp / "aocv.v").write_text(
        "module top(clk_in, d, y);\n"
        "  input clk_in, d; output y;\n"
        "  wire clk_tree, div_q, w1;\n"
        "  BUF clk_buf(.A(clk_in), .X(clk_tree));\n"
        "  DFF div_ff(.CK(clk_tree), .D(div_q), .Q(div_q));\n"
        "  BUF b1(.A(d), .X(w1));\n"
        "  DFF cap_ff(.CK(div_q), .D(w1), .Q(y));\n"
        "endmodule\n",
        encoding="utf-8",
    )
    # Valid tabular AOCV table (matches testcases/aocv/base.aocv format)
    (tmp / "a.aocv").write_text(
        "design * clock rise early 1 0.8\n"
        "design * clock rise early 2 0.9\n"
        "design * clock rise late 1 1.2\n"
        "design * clock rise late 2 1.1\n"
        "design * clock fall early 1 0.8\n"
        "design * clock fall early 2 0.9\n"
        "design * clock fall late 1 1.2\n"
        "design * clock fall late 2 1.1\n"
        "design * data rise early 1 0.8\n"
        "design * data rise early 2 0.9\n"
        "design * data rise late 1 1.2\n"
        "design * data rise late 2 1.1\n"
        "design * data fall early 1 0.8\n"
        "design * data fall early 2 0.9\n"
        "design * data fall late 1 1.2\n"
        "design * data fall late 2 1.1\n",
        encoding="utf-8",
    )
    for tag, master_opt in [("auto", ""), ("explicit", "-master_clock clk")]:
        (tmp / f"gc_{tag}.sdc").write_text(
            "create_clock -name clk -period 10.0 [get_ports clk_in]\n"
            f"create_generated_clock -name gclk -source [get_pins div_ff/CK] {master_opt} "
            "-divide_by 2 [get_pins div_ff/Q]\n"
            "set_propagated_clock [all_clocks]\n"
            "set_input_delay -clock gclk 1.0 [get_ports d]\n"
            "set_output_delay -clock gclk 1.0 [get_ports y]\n",
            encoding="utf-8",
        )
    o_auto = run_msta(
        tmp,
        "read_liberty min.lib\nread_verilog aocv.v\ncurrent_design top\n"
        "read_aocv a.aocv\nread_sdc gc_auto.sdc\nreport_checks -setup\n",
        "g9_auto.dofile",
    )
    o_exp = run_msta(
        tmp,
        "read_liberty min.lib\nread_verilog aocv.v\ncurrent_design top\n"
        "read_aocv a.aocv\nread_sdc gc_explicit.sdc\nreport_checks -setup\n",
        "g9_exp.dofile",
    )
    assert "警告：sdc" not in o_auto, o_auto
    sa, ha = parse_msta_wns(o_auto)
    se, he = parse_msta_wns(o_exp)
    assert_close(sa, se, 1e-6, "group9_genclk_auto_master_setup")
    assert_close(ha, he, 1e-6, "group9_genclk_auto_master_hold")
    assert "gclk" in o_auto and "20.000" in o_auto or sa > 15.0, o_auto
    print("[PASS] Group 9: seq genclk auto -master_clock + AOCV depth")


def test_group10_tcl_bridge_wildcards(tmp: Path):
    (tmp / "min.lib").write_text(MIN_LIB, encoding="utf-8")
    (tmp / "w.v").write_text(
        "module top(clk, p_keep, p_excl, \\d[10] , \\d[1] , y1, y2, y3, y4);\n"
        "  input clk, p_keep, p_excl, \\d[10] , \\d[1] ;\n"
        "  output y1, y2, y3, y4;\n"
        "  wire w_a, w_b;\n"
        "  BUF u_chain_a(.A(p_keep), .X(w_a));\n"
        "  BUF u_chain_b(.A(\\d[1] ), .X(w_b));\n"
        "  DFF u_ff1(.CK(clk), .D(w_a), .Q(y1));\n"
        "  DFF u_ff2(.CK(clk), .D(p_excl), .Q(y2));\n"
        "  DFF u_ff3(.CK(clk), .D(\\d[10] ), .Q(y3));\n"
        "  DFF u_ff4(.CK(clk), .D(w_b), .Q(y4));\n"
        "endmodule\n",
        encoding="utf-8",
    )
    (tmp / "w.sdc").write_text(
        "create_clock -name clk -period 10.0 [get_ports clk]\n"
        "set_input_delay -clock clk 1.0 [remove_from_collection [all_inputs] [get_ports {clk p_excl}]]\n"
        "set_input_transition 0.08 [remove_from_collection [all_inputs -clock [get_clocks clk]] [get_ports {d[1]}]]\n"
        "set_output_delay -clock clk 1.0 [all_outputs]\n"
        "set_false_path -from [get_ports -filter {name == d[10]}]\n"
        "set_false_path -from [get_pins -filter {is_clock == true && full_name =~ u_ff4*}]\n"
        "set_timing_derate -late 2.0 [get_cells u_chain_*]\n"
        "set_max_transition 0.01 [get_pins u_chain_*/X]\n"
        "set_data_check -setup -rise_from [get_pins u_chain_a/X] -fall_to [get_pins u_chain_b/X] 0.5\n",
        encoding="utf-8",
    )
    out = run_msta(
        tmp,
        "read_liberty min.lib\nread_verilog w.v\ncurrent_design top\n"
        "read_sdc w.sdc\nreport_checks -setup\n",
        "g10.dofile",
    )
    assert "警告：sdc" not in out, out
    assert "max_transition 违例 2 处" in out, out
    assert "数据检查 : 1 条" in out, out
    assert "路径例外排除 2 个" in out, out  # y3 cut by d[10], y4 output path cut by u_ff4/CK
    print("[PASS] Group 10: Tcl bridge & wildcard get_cells/get_pins/remove_from_collection/-filter/data_check")


def test_group11_review_fixes(tmp: Path):
    # 11a: capacitive_load_unit(1e-12, "F") + capacitive_load_unit : "10ff" + set_units -time 1e-9s -capacitance 1e-12F
    lib_farad = """library(farad_lib) {
  time_unit : "1e-9s";
  capacitive_load_unit (1e-12, "F");
  lu_table_template(t2x2) {
    variable_1 : input_net_transition;
    variable_2 : total_output_net_capacitance;
    index_1("0.01, 0.1");
    index_2("0.005, 0.05");
  }
  cell(BUF) {
    pin(A) { direction : input; capacitance : 0.002; }
    pin(X) {
      direction : output; function : "A";
      timing() {
        related_pin : "A"; timing_sense : positive_unate; timing_type : combinational;
        cell_rise(t2x2) { values("0.10, 0.20", "0.15, 0.25"); }
        cell_fall(t2x2) { values("0.10, 0.20", "0.15, 0.25"); }
      }
    }
  }
  cell(DFF) {
    ff(IQ, IQN) { clocked_on : "CK"; next_state : "D"; }
    pin(CK) { direction : input; capacitance : 0.002; clock : true; }
    pin(D) {
      direction : input; capacitance : 0.002;
      timing() { related_pin : "CK"; timing_type : setup_rising; rise_constraint(scalar) { values("0.10"); } fall_constraint(scalar) { values("0.10"); } }
    }
    pin(Q) {
      direction : output; function : "IQ";
      timing() { related_pin : "CK"; timing_type : rising_edge; cell_rise(scalar) { values("0.20"); } cell_fall(scalar) { values("0.20"); } }
    }
  }
}
"""
    lib_10ff = """library(tenff_lib) {
  time_unit : "1ns";
  capacitive_load_unit : "10ff";
  cell(CAPBUF) {
    pin(A) { direction : input; capacitance : 2.0; } /* 2 * 10fF = 20fF */
    pin(X) {
      direction : output; function : "A"; max_capacitance : 1.0; /* 10fF */
      timing() {
        related_pin : "A"; timing_sense : positive_unate; timing_type : combinational;
        cell_rise(scalar) { values("0.10"); }
        cell_fall(scalar) { values("0.10"); }
      }
    }
  }
}
"""
    (tmp / "farad.lib").write_text(lib_farad, encoding="utf-8")
    (tmp / "tenff.lib").write_text(lib_10ff, encoding="utf-8")
    (tmp / "u.v").write_text(
        "module top(clk, a, y); input clk, a; output y; wire w1, w2; "
        "BUF b0(.A(a), .X(w1)); CAPBUF b1(.A(w1), .X(w2)); DFF ff(.CK(clk), .D(w2), .Q(y)); endmodule\n",
        encoding="utf-8",
    )
    (tmp / "u.sdc").write_text(
        "set_units -time 1e-9s -capacitance 1e-12F\n"
        "create_clock -name clk -period 10 [get_ports clk]\n"
        "set_input_delay -clock clk -max 1.0 [get_ports a]\n"
        "set_input_transition 0.01 [get_ports a]\n"
        "set_output_delay -clock clk -max 1.0 [get_ports y]\n"
        "set_load 0.015 [get_ports y]\n",
        encoding="utf-8",
    )
    out_u = run_msta(
        tmp,
        "read_liberty farad.lib\nread_liberty tenff.lib\nread_verilog u.v\ncurrent_design top\n"
        "read_sdc u.sdc\nreport_checks -setup\n",
        "g11a.dofile",
    )
    assert "警告：sdc" not in out_u, out_u
    # CAPBUF/A has 2.0 * 10ff = 20fF, index_1=0.01, index_2=(5fF, 50fF) -> 0.1 + (20-5)/45 * 0.1 = 0.1333ns
    # arrival = 1.0 + 0.1333 + 0.1 = 1.2333ns -> setup slack = 10 - 0.1 - 1.2333 = 8.667ns
    su, _ = parse_msta_wns(out_u)
    assert_close(su, 8.667, 2e-3, "group11a_farad_and_10ff")

    # 11b: set_data_check setup-only (-max only) + skewed launch clocks vs OpenSTA
    (tmp / "dc.v").write_text(
        "module top(clk, a, b, y1, y2);\n"
        "  input clk, a, b; output y1, y2;\n"
        "  wire c_fast, c_slow1, c_slow, q1, q2, d_from, d_to;\n"
        "  sky130_fd_sc_hd__buf_1 cf(.A(clk), .X(c_fast));\n"
        "  sky130_fd_sc_hd__buf_1 cs1(.A(clk), .X(c_slow1));\n"
        "  sky130_fd_sc_hd__buf_1 cs2(.A(c_slow1), .X(c_slow));\n"
        "  sky130_fd_sc_hd__dfxtp_1 ff_from(.CLK(c_slow), .D(a), .Q(q1));\n"
        "  sky130_fd_sc_hd__dfxtp_1 ff_to(.CLK(c_fast), .D(b), .Q(q2));\n"
        "  sky130_fd_sc_hd__buf_1 df(.A(q1), .X(d_from));\n"
        "  sky130_fd_sc_hd__buf_1 dt(.A(q2), .X(d_to));\n"
        "  sky130_fd_sc_hd__dfxtp_1 ff_y1(.CLK(clk), .D(d_from), .Q(y1));\n"
        "  sky130_fd_sc_hd__dfxtp_1 ff_y2(.CLK(clk), .D(d_to), .Q(y2));\n"
        "endmodule\n",
        encoding="utf-8",
    )
    (tmp / "dc.sdc").write_text(
        "create_clock -name clk -period 20.0 [get_ports clk]\n"
        "set_propagated_clock [all_clocks]\n"
        "set_input_delay -clock clk -max 1.0 [get_ports {a b}]\n"
        "set_output_delay -clock clk -max 1.0 [get_ports {y1 y2}]\n"
        "set_data_check -setup -rise_from [get_pins df/X] -rise_to [get_pins dt/X] 0.5\n",
        encoding="utf-8",
    )
    out_dc = run_msta(
        tmp,
        f"read_liberty {SKY130_LIB}\nread_verilog dc.v\ncurrent_design top\n"
        "read_sdc dc.sdc\nreport_checks -setup\n",
        "g11b.dofile",
    )
    m_dc = re.search(r"数据检查\s*:\s*1 条\s*setup 最差\s+(-?\d+\.\d+)\s+ns", out_dc)
    assert m_dc, out_dc
    msta_dc = float(m_dc.group(1))
    if OPENSTA.is_file():
        f_dc = tmp / "dc_ref.tcl"
        f_dc.write_text(
            f"read_liberty {SKY130_LIB}\nread_verilog dc.v\nlink_design top\n"
            "read_sdc dc.sdc\nreport_checks -to [get_pins dt/X] -path_delay max -digits 4\n",
            encoding="utf-8",
        )
        rc, o_dc = run_cmd([str(OPENSTA), "-no_splash", "-no_init", "-exit", str(f_dc)], tmp)
        assert rc == 0, o_dc
        m_ref = re.search(r"^\s*(-?\d+\.\d+)\s+slack\s+\(", o_dc, re.M)
        assert m_ref, o_dc
        assert_close(msta_dc, float(m_ref.group(1)), 0.003, "group11b_data_check_skew_opensta")

    # 11c: Blackbox pin union + partial read_liberty direction re-inference + module reload clear
    (tmp / "p1.v").write_text(
        "module stale_sub(input x, output y); assign y = x; endmodule\n"
        "module old_top(clk, a, y); input clk, a; output y; DFF ff(.CK(clk), .D(a), .Q(y)); endmodule\n",
        encoding="utf-8",
    )
    (tmp / "p2.v").write_text(
        "module top(clk, a, y);\n"
        "  input clk, a; output y;\n"
        "  wire w1, w2;\n"
        "  BB_TWO u_bb1 (.A(a));\n"
        "  BB_TWO u_bb2 (.A(a), .Z(w1));\n"
        "  DELAYBUF u_late (.A(w1), .X(w2));\n"
        "  DFF ff (.CK(clk), .D(w2), .Q(y));\n"
        "endmodule\n",
        encoding="utf-8",
    )
    (tmp / "late_buf.lib").write_text(
        "library(late_lib) {\n"
        "  time_unit : \"1ns\"; capacitive_load_unit (1, pf);\n"
        "  cell(DELAYBUF) {\n"
        "    pin(A) { direction : input; capacitance : 0.002; }\n"
        "    pin(X) { direction : output; function : \"A\";\n"
        "      timing() { related_pin : \"A\"; timing_sense : positive_unate; timing_type : combinational;\n"
        "        cell_rise(scalar) { values(\"2.0\"); } cell_fall(scalar) { values(\"2.0\"); } } }\n"
        "  }\n"
        "}\n",
        encoding="utf-8",
    )
    (tmp / "p2.sdc").write_text(
        "create_clock -name clk -period 10.0 [get_ports clk]\n"
        "set_input_delay -clock clk 1.0 [get_ports a]\n"
        "set_output_delay -clock clk 1.0 [get_ports y]\n",
        encoding="utf-8",
    )
    out_bb = run_msta(
        tmp,
        "read_liberty farad.lib\n"
        "read_verilog p1.v\ncurrent_design old_top\n"
        "read_verilog p2.v\ncurrent_design\n"   # bare current_design proves stale modules from p1.v cleared!
        "read_liberty late_buf.lib\n"          # resolves DELAYBUF -> u_bb2/Z must re-infer as output on w1!
        "read_sdc p2.sdc\nreport_checks -setup\n",
        "g11c.dofile",
    )
    sbb, _ = parse_msta_wns(out_bb)
    # Path: a(1.0) -> BB_TWO(0) -> DELAYBUF(2.0) -> DFF(setup 0.1) = 3.1 => slack = 6.9
    assert_close(sbb, 6.9, 1e-3, "group11c_bb_union_reinfer_reload")

    # 11d: Intermediate clock net set_case_analysis 0 blocks clock + && / || / ~ in Liberty when
    lib_bool = """library(bool_lib) {
  time_unit : "1ns"; capacitive_load_unit (1, pf);
  cell(MUXW) {
    pin(A)  { direction : input; capacitance : 0.002; }
    pin(S0) { direction : input; capacitance : 0.002; }
    pin(S1) { direction : input; capacitance : 0.002; }
    pin(Y) {
      direction : output; function : "(A & ((~S1 && S0) || (S1 && ~S0)))";
      timing() { related_pin : "A"; when : "(~S1 && S0) || (S1 && ~S0)"; timing_type : combinational;
        cell_rise(scalar) { values("0.5"); } cell_fall(scalar) { values("0.5"); } }
      timing() { related_pin : "A"; when : "!S1 && !S0"; timing_type : combinational;
        cell_rise(scalar) { values("5.0"); } cell_fall(scalar) { values("5.0"); } }
    }
  }
  cell(BUF) {
    pin(A) { direction : input; capacitance : 0.002; }
    pin(X) { direction : output; function : "A";
      timing() { related_pin : "A"; timing_sense : positive_unate; timing_type : combinational;
        cell_rise(scalar) { values("0.2"); } cell_fall(scalar) { values("0.2"); } } }
  }
  cell(DFF) {
    ff(IQ, IQN) { clocked_on : "CK"; next_state : "D"; }
    pin(CK) { direction : input; capacitance : 0.002; clock : true; }
    pin(D)  { direction : input; capacitance : 0.002;
      timing() { related_pin : "CK"; timing_type : setup_rising; rise_constraint(scalar) { values("0.1"); } fall_constraint(scalar) { values("0.1"); } } }
    pin(Q)  { direction : output; function : "IQ";
      timing() { related_pin : "CK"; timing_type : rising_edge; cell_rise(scalar) { values("0.2"); } cell_fall(scalar) { values("0.2"); } } }
  }
}
"""
    (tmp / "bool.lib").write_text(lib_bool, encoding="utf-8")
    (tmp / "bool.v").write_text(
        "module top(clk, a, s0, s1, y, y_gated);\n"
        "  input clk, a, s0, s1; output y, y_gated;\n"
        "  wire clk_off, d_w;\n"
        "  BUF cb_off(.A(clk), .X(clk_off));\n"
        "  MUXW um(.A(a), .S0(s0), .S1(s1), .Y(d_w));\n"
        "  DFF ff_on (.CK(clk), .D(d_w), .Q(y));\n"
        "  DFF ff_off(.CK(clk_off), .D(a), .Q(y_gated));\n"
        "endmodule\n",
        encoding="utf-8",
    )
    (tmp / "bool.sdc").write_text(
        "create_clock -name clk -period 10.0 [get_ports clk]\n"
        "set_case_analysis 0 [get_pins cb_off/X]\n"
        "set_case_analysis 1 [get_ports s0]\n"
        "set_case_analysis 0 [get_ports s1]\n"
        "set_input_delay -clock clk 1.0 [get_ports a]\n"
        "set_output_delay -clock clk 1.0 [get_ports y]\n",
        encoding="utf-8",
    )
    out_bool = run_msta(
        tmp,
        "read_liberty bool.lib\nread_verilog bool.v\ncurrent_design top\n"
        "read_sdc bool.sdc\nreport_checks -setup -max_paths 4\n",
        "g11d.dofile",
    )
    # 5.0ns arc pruned by when (~S1 && S0), leaving 0.5ns arc -> ff_on: 1.0 + 0.5 + 0.1 = 1.6 -> slack 8.4
    # clk_off case-0 stops clock -> ff_off unconstrained (y_gated too -> 2 unconstrained endpoints), y output slack 8.8
    assert "未约束 2 个" in out_bool, out_bool
    assert "all_registers" not in out_bool
    sb, _ = parse_msta_wns(out_bool)
    assert_close(sb, 8.4, 1e-3, "group11d_case_clk_and_bool_when")

    # 11e: >64 remove_from_collection items (80 removed ports + all_clocks pattern warning check)
    many_ports = [f"p_{k}" for k in range(80)]
    decls = ", ".join(many_ports)
    (tmp / "m80.v").write_text(
        f"module top(clk, a_keep, {decls}, y);\n"
        f"  input clk, a_keep, {decls}; output y;\n"
        f"  DFF ff(.CK(clk), .D(a_keep), .Q(y));\n"
        f"endmodule\n",
        encoding="utf-8",
    )
    excl_list = " ".join(many_ports + ["clk"])
    (tmp / "m80.sdc").write_text(
        "create_clock -name clk -period 10.0 [get_ports clk]\n"
        f"set_input_delay -clock clk 1.0 [remove_from_collection [all_inputs] [get_ports {{{excl_list}}}]]\n"
        "set_input_delay -clock clk 9.0 [get_ports p_79]\n"  # must NOT affect y (only a_keep drives D); wait let's tie p_79 too?
        "set_output_delay -clock clk 1.0 [get_ports y]\n",
        encoding="utf-8",
    )
    # Make p_79 drive a second FF whose D is unconstrained unless p_79's 9.0 delay applies!
    # Wait: if remove_from_collection capped at 64, p_79 wouldn't be in the minus list -> would get 1.0 instead of skipped?
    # Better: assign [remove_from_collection ...] to 1.0, and check if p_79 gets ZERO input delay (remains unconstrained!).
    (tmp / "m80b.v").write_text(
        f"module top(clk, a_keep, {decls}, y, y79);\n"
        f"  input clk, a_keep, {decls}; output y, y79;\n"
        f"  DFF ff0(.CK(clk), .D(a_keep), .Q(y));\n"
        f"  DFF ff79(.CK(clk), .D(p_79), .Q(y79));\n"
        f"endmodule\n",
        encoding="utf-8",
    )
    (tmp / "m80b.sdc").write_text(
        "create_clock -name clk -period 10.0 [get_ports clk]\n"
        f"set_input_delay -clock clk 1.0 [remove_from_collection [all_inputs] [get_ports {{{excl_list}}}]]\n"
        "set_output_delay -clock clk 1.0 [get_ports {y y79}]\n",
        encoding="utf-8",
    )
    out_m80 = run_msta(
        tmp,
        "read_liberty farad.lib\nread_verilog m80b.v\ncurrent_design top\n"
        "read_sdc m80b.sdc\nreport_checks -setup -max_paths 4\n",
        "g11e.dofile",
    )
    # ff79/D must remain unconstrained because p_79 (index 80 > 64!) was removed from [all_inputs]
    assert "未约束 1 个" in out_m80, out_m80
    print("[PASS] Group 11: unit scaling + data_check skew + blackbox/reload + case clk + >64 minus")


def main():
    with tempfile.TemporaryDirectory() as d:
        tmp = Path(d)
        for fn, sub in [
            (test_group1_netlist_blackbox_bus, "g1"),
            (test_group2_reload_units_comments, "g2"),
            (test_group3_drc_unset, "g3"),
            (test_group4_liberty_opcond_nan, "g4"),
            (test_group5_multi_arc_when_opensta, "g5"),
            (test_group6_driving_cell_opensta, "g6"),
            (test_group7_budget_and_multicycle, "g7"),
            (test_group8_through_isolation_async, "g8"),
            (test_group9_seq_genclk_aocv, "g9"),
            (test_group10_tcl_bridge_wildcards, "g10"),
            (test_group11_review_fixes, "g11"),
        ]:
            p = tmp / sub
            p.mkdir(parents=True, exist_ok=True)
            fn(p)
    print("==> Phase 1 专项检查全部通过（11 组）")


if __name__ == "__main__":
    main()
