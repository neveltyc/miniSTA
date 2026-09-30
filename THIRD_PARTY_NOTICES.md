# 第三方代码声明

本工程以 MIT 许可证发布（见 `LICENSE`）。仓库里还包含下列第三方内容，
它们各自按自己的许可证分发，原始许可证正文一并保留在下面。

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
`testcases/opentimer_simple/`

这三个目录下的网表、Liberty 库和 SDC 取自 OpenTimer 的 benchmark
（https://github.com/OpenTimer/OpenTimer），版权归 Tsung-Wei Huang 与
Martin D. F. Wong（The University of Utah / University of Illinois at
Urbana-Champaign）所有，按 MIT 许可证收录，用来验证本工程的 SDC 子集。

MIT License

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
