/**CHeader*************************************************************

  FileName    [msta_cmds.h]

  Synopsis    [命令层：dofile 解析 + 命令表 + 全局状态容器 MstaApp。]

  ---------------------------------------------------------------------
  脚本语言
  ---------------------------------------------------------------------
  dofile 是"一行一条命令，空白分参数，双引号包字符串，# 开头是注释"。
  SDC 文件由 scripts/sdc_bridge.tcl 用 tclsh 处理成同一套命令（见 msta_sdc.c）。

***********************************************************************/

#ifndef MSTA_CMDS_H
#define MSTA_CMDS_H

#include "msta_types.h"
#include "msta_lib.h"

struct MstaDesign;
struct MstaSdc;
struct MstaTiming;

typedef struct {
    MstaLib           *pLib;
    struct MstaDesign *pNet;
    struct MstaSdc    *pSdc;
    struct MstaTiming *pTime;
    FILE              *pOut;      /* 报告输出，默认 stdout */
    int                fQuiet;    /* 少打印 */
    int                fVerbose;  /* 多打印内部细节 */
} MstaApp;

MstaApp *Msta_AppStart( void );
void     Msta_AppFree( MstaApp *p );

/* 执行一段脚本文本（多条命令）。返回 0 表示中途出错。 */
int  Msta_CmdsRun( MstaApp *pApp, const char *pScript );
/* 执行一条命令（已经切好参数）。 */
int  Msta_CmdsRunOne( MstaApp *pApp, int argc, char **argv );
void Msta_CmdsSetQuiet( int fQuiet );
void Msta_CmdsSetOutput( const char *pFileName );
/* 记下 dofile 所在目录：相对路径先按当前目录找，找不到再按该目录找。 */
void Msta_CmdsSetScriptFile( const char *pScriptPath );

#endif /* MSTA_CMDS_H */
