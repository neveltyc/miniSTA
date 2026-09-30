/**CHeader*************************************************************

  FileName    [msta_report.h]

  Synopsis    [报告：report_checks / report_summary / report_clock_tree。]

  报告层只做一件事 —— 把 msta_timing 算出来的数字排好版打出来。
  它不改任何状态，也不做任何计算，这样"数字怎么来的"和"数字怎么呈现"
  两件事不会搅在一起。

***********************************************************************/

#ifndef MSTA_REPORT_H
#define MSTA_REPORT_H

#include <stdio.h>
#include "msta_timing.h"

/* 最差的若干条路径。fSetup=1 报 setup（max 角），0 报 hold（min 角）。
   nMaxPaths <= 0 表示只报汇总，不打印路径细节。 */
void Msta_ReportChecks( MstaTiming *p, FILE *pFile, int nMaxPaths, int fSetup );

void Msta_ReportSummary( MstaTiming *p, FILE *pFile );
void Msta_ReportClockTree( MstaTiming *p, FILE *pFile );

#endif /* MSTA_REPORT_H */
