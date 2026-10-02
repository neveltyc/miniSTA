# 第三方代码声明

本工程以 MIT 许可证发布（见 `LICENSE`）。仓库里还包含下列第三方内容，
它们各自按自己的许可证分发，不受本工程 MIT 许可证约束；原始许可证正文或
出处一并列在下面，相应文件的开头也保留了上游的版权与许可声明。

## ABC（sclLiberty）

文件：`src/abc_scl_liberty_tree.inc`

该文件是 ABC `src/map/scl/sclLiberty.c` 中 Liberty 语法树解析部分的最小独立版本
（https://github.com/berkeley-abc/abc）。函数名、结构体名和控制流刻意与原文件保持
一致，便于对照阅读；具体改动列在文件头注释里。

ABC: System for Sequential Synthesis and Verification
http://www.eecs.berkeley.edu/~alanmi/abc/

Copyright (c) The Regents of the University of California. All rights reserved.

Permission is hereby granted, without written agreement and without license or
royalty fees, to use, copy, modify, and distribute this software and its
documentation for any purpose, provided that the above copyright notice and
the following two paragraphs appear in all copies of this software.

IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO ANY PARTY FOR
DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES ARISING OUT OF
THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF
CALIFORNIA HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING,
BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE. THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS" BASIS,
AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO PROVIDE MAINTENANCE,
SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.

## OpenTimer benchmark

目录：`testcases/opentimer_c17/`、`testcases/opentimer_s27/`、
`testcases/opentimer_simple/`；文件：`testcases/sta_compare/netlist.v`、
`testcases/sta_compare/opentimer.sdc`

前三个目录下的网表、Liberty 库和 SDC 取自 OpenTimer 的 benchmark
（https://github.com/OpenTimer/OpenTimer），版权归 Tsung-Wei Huang 与
Martin D. F. Wong（The University of Utah / University of Illinois at
Urbana-Champaign）所有，按 MIT 许可证收录，用来验证本工程的 SDC 子集。
`testcases/sta_compare/netlist.v` 与 `opentimer.sdc` 改写自 OpenTimer 的
`example/simple/simple.v` 与 `simple.sdc`，改动写在各自的文件头里。

MIT License

Copyright (c) 2018-2021 Tsung-Wei Huang and Martin D. F. Wong
The University of Utah, UT, USA
The University of Illinois at Urbana-Champaign, IL, USA

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## ICsprout 55 PDK（icsprout55-pdk）

文件：`testcases/multi_vt/lib/ics55_LLSC_H7CL_typ_tt.lib`、
`testcases/multi_vt/lib/ics55_LLSC_H7CR_typ_tt.lib`、
`testcases/multi_vt/lib/ics55_LLSC_H7CH_typ_tt.lib`

这三个库是 ICsprout 55 PDK 标准单元库的子集
（https://github.com/openecos-projects/icsprout55-pdk，release `v1.10.102`，
typical 角 `typ_tt_1p2_25`），按 Apache License 2.0 分发，版权归 ICsprout
Integrated Circuit Co., Ltd.（2025）；原始许可证头保留在每个文件开头。

改动：只保留 `testcases/multi_vt` 用到的单元，并去掉 `internal_power` /
`leakage_power` 组（本工程不建模功耗）；时序表和约束未做改动。

完整库不进仓库，需要重新综合网表时用 `scripts/fetch_ics55_liberty.sh` 下载到
`vendor/`。

    Apache License
    Version 2.0, January 2004
    http://www.apache.org/licenses/

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use these files except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.

## PicoRV32

文件：`testcases/multi_vt/rtl/picorv32.v`（同目录有它自己的 `picorv32_COPYING`）

PicoRV32 版权归 Claire Xenia Wolf <claire@yosyshq.com> 所有（2015-2021），
按 ISC 许可证分发，许可证正文保存在文件旁边。
`testcases/multi_vt/netlist/multi_vt_soc.v` 是这些 RTL 的综合结果，其中包含
PicoRV32 的逻辑，文件开头保留了 PicoRV32 的版权与许可声明。

## SkyWater SKY130 标准单元库（sky130_fd_sc_hd）

文件：`testcases/lib/sky130.lib`

SkyWater SKY130 PDK 高密度标准单元库 `sky130_fd_sc_hd` 的 `tt_025C_1v80` 角
（https://github.com/google/skywater-pdk-libs-sky130_fd_sc_hd），按 Apache
License 2.0 分发。仓库里的网表（`testcases/synth_sky130/`、`testcases/wrp/`、
`testcases/sdc_features/`、`testcases/sta_compare/` 以及下面几份综合网表）只引用
该库的单元名。

    Copyright 2020 The SkyWater PDK Authors

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.

## OpenCores AC 97 Controller

文件：`testcases/ac97/netlist/ac97_ctrl.v`

OpenCores "AC 97 Controller IP Core"（https://opencores.org/projects/ac97，
镜像 https://github.com/freecores/ac97）的 RTL 经逻辑综合得到的门级网表，
综合工具与日期见文件里的原生成信息。上游源文件的版权与许可声明原文
（`rtl/verilog/ac97_top.v`）如下，同样保留在网表文件开头：

    //// Copyright (C) 2000-2002 Rudolf Usselmann                    ////
    ////                         www.asics.ws                        ////
    ////                         rudi@asics.ws                       ////
    ////                                                             ////
    //// This source file may be used and distributed without        ////
    //// restriction provided that this copyright statement is not   ////
    //// removed from the file and that any derivative work contains ////
    //// the original copyright notice and the associated disclaimer.////
    ////                                                             ////
    ////     THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY     ////
    //// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED   ////
    //// TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS   ////
    //// FOR A PARTICULAR PURPOSE. IN NO EVENT SHALL THE AUTHOR      ////
    //// OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,         ////
    //// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES    ////
    //// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE   ////
    //// GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR        ////
    //// BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF  ////
    //// LIABILITY, WHETHER IN  CONTRACT, STRICT LIABILITY, OR TORT  ////
    //// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT  ////
    //// OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE         ////
    //// POSSIBILITY OF SUCH DAMAGE.                                 ////

## OpenCores Ethernet MAC 10/100 Mbps（ethmac）

文件：`testcases/eth_sky130/netlist/ethernet_sky130.v`

OpenCores "Ethernet MAC 10/100 Mbps"（https://opencores.org/projects/ethmac，
镜像 https://github.com/freecores/ethmac）的 RTL 经逻辑综合得到的 sky130 门级
网表，综合工具与日期见文件里的原生成信息。上游作者为 Igor Mohor、Novan
Hartadi、Mahmud Galela、Bill Dittenhofer、Olof Kindgren；上游按 GNU Lesser
General Public License 2.1 或更高版本（LGPL-2.1-or-later）分发，许可证全文见
`licenses/LGPL-2.1.txt`，对应的源代码可从上面的上游地址获得。上游源文件的版权
与许可声明原文（`eth_clockgen.v`，其余文件的版权年份为 2001、2001, 2002 或
2001, 2011）如下，同样保留在网表文件开头：

    //// Copyright (C) 2001 Authors                                   ////
    ////                                                              ////
    //// This source file may be used and distributed without         ////
    //// restriction provided that this copyright statement is not    ////
    //// removed from the file and that any derivative work contains  ////
    //// the original copyright notice and the associated disclaimer. ////
    ////                                                              ////
    //// This source file is free software; you can redistribute it   ////
    //// and/or modify it under the terms of the GNU Lesser General   ////
    //// Public License as published by the Free Software Foundation; ////
    //// either version 2.1 of the License, or (at your option) any   ////
    //// later version.                                               ////
    ////                                                              ////
    //// This source is distributed in the hope that it will be       ////
    //// useful, but WITHOUT ANY WARRANTY; without even the implied   ////
    //// warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR      ////
    //// PURPOSE.  See the GNU Lesser General Public License for more ////
    //// details.                                                     ////
    ////                                                              ////
    //// You should have received a copy of the GNU Lesser General    ////
    //// Public License along with this source; if not, download it   ////
    //// from http://www.opencores.org/lgpl.shtml                     ////
    ////                                                              ////

## T-Head openE902

文件：`testcases/e902_sky130/netlist/opene902.v`

T-Head（平头哥）openE902（https://github.com/XUANTIE-RV/opene902，原
https://github.com/T-head-Semi/opene902）的 RTL 经逻辑综合得到的 sky130 门级
网表，综合工具与日期见文件里的原生成信息。上游按 Apache License 2.0 分发，
源文件的版权与许可声明原文如下，同样保留在网表文件开头：

    Copyright 2018-2021 T-Head Semiconductor Co., Ltd.

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
