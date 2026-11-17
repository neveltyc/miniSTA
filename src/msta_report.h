/**CHeader*************************************************************

  FileName    [msta_report.h]

  Synopsis    [报告与教学诊断命令：report_checks / summary / clock_tree / explain_arc / report_net / dump_graph。]

  报告层负责把 msta_timing 算出来的数字排好版打出来，只做展示与排序，不改任何时序结果。

***********************************************************************/

#ifndef MSTA_REPORT_H
#define MSTA_REPORT_H

#include <stdio.h>
#include "msta_timing.h"

#define MSTA_RPT_TRAN        (1 << 0)
#define MSTA_RPT_CAP         (1 << 1)
#define MSTA_RPT_CLOCK_PATH  (1 << 2)

/* 最差的若干条路径。fSetup=1 报 setup（max 角），0 报 hold（min 角）。 */
void Msta_ReportChecks( MstaTiming *p, FILE *pFile, int nMaxPaths, int fSetup );
/* 扩展路径报告：Flags 为 MSTA_RPT_TRAN / MSTA_RPT_CAP / MSTA_RPT_CLOCK_PATH 组合。 */
void Msta_ReportChecksEx( MstaTiming *p, FILE *pFile, int nMaxPaths, int fSetup, int Flags );

void Msta_ReportSummary( MstaTiming *p, FILE *pFile );
void Msta_ReportClockTree( MstaTiming *p, FILE *pFile );

/* 教学诊断报告：逐弧 NLDM 双线性查表拆解、网络级 AT/RAT/Slack 汇总、Graphviz DOT 导出。 */
int  Msta_ReportExplainArc( MstaTiming *p, FILE *pFile,
                            const char *pInstName, const char *pInPin,
                            const char *pOutPin, int fMax );
int  Msta_ReportNet( MstaTiming *p, FILE *pFile, const char *pNetName );
int  Msta_ReportDumpGraph( MstaTiming *p, FILE *pFile, int nMaxNodes );

#endif /* MSTA_REPORT_H */
