/**CHeader*************************************************************

  FileName    [msta_sdc_int.h]

  Synopsis    [SDC 模块内部头文件：msta_sdc_*.c 之间共享的类型与函数。]

  模块内部头文件，外部模块只用 msta_sdc.h。只被 msta_sdc.c 和 msta_sdc_*.c 包含。
  SDC 模块的文件分工：
    msta_sdc.c         —— 读入与分发：Tcl 桥、JSON、集合展开、分发表、生命周期；
                          也管展开后的参数和本条命令的临时内存
    msta_sdc_parse.c   —— 数值与对象的小工具、选项表与通用解析器、公共子语法
    msta_sdc_clock.c   —— 时钟类命令
    msta_sdc_io.c      —— I/O 与网络属性类命令
    msta_sdc_except.c  —— 路径例外与附加检查类命令
    msta_sdc_env.c     —— 设计规则、derate、工作条件与单位类命令
    msta_sdc_query.c   —— 给时序引擎的查询接口（msta_sdc.h），含例外表索引

***********************************************************************/

#ifndef MSTA_SDC_INT_H
#define MSTA_SDC_INT_H

#include "msta_sdc.h"
#include "msta_lib.h"
#include "msta_util.h"

MstaArrayDefine( int, MstaSdcIntArray )

/* 选项的种类：决定解析器怎样取它的值。选项的值都是紧跟的下一个 Tcl 参数；
   这个参数可以是一个名字、一段 Tcl 列表文本（如 {a b}），也可以是 get_* 集合
   （展开后是多个名字）。 */
typedef enum {
    MSTA_SDC_FLAG,         /* 开关，如 -add：出现就算数 */
    MSTA_SDC_VALUE,        /* 带一个值，如 -period 5、-clock clk：下一个参数只能是一个词，
                              列表文本或展开出多个名字的集合都会使约束作废 */
    MSTA_SDC_NUMBERS,      /* 带一个数值列表，如 -waveform {0 5}：下一个参数的整段文本
                              原样作为值，由处理函数拆开、检查个数 */
    MSTA_SDC_OBJECTS       /* 带一组对象，如 -from {a b}、-clock [get_clocks {a b}]：
                              下一个参数里的所有名字（列表文本按空白拆开，集合展开） */
} MstaSdcOptKind;

/* 选项的附加属性，可以用 | 组合。 */
#define MSTA_SDC_RF      1 /* 也认 -rise_xxx / -fall_xxx 写法，并记下用的是哪个边沿 */
#define MSTA_SDC_REPEAT  2 /* 可以写多次，每次是单独的一组，如 -through、-group（只用于 OBJECTS） */
#define MSTA_SDC_IGNORE  4 /* 认得但不建模：告警后忽略这个选项，约束照常生效 */
#define MSTA_SDC_REJECT  8 /* 认得但不建模：出现就作废整条约束 */

/* 选项表的一行。表以 pName == NULL 的一行结尾。 */
typedef struct {
    const char     *pName;
    MstaSdcOptKind  Kind;
    int             Attr;
} MstaSdcOpt;

/* 位置参数开头的值。 */
typedef enum {
    MSTA_SDC_NO_VALUE,     /* 没有值，位置参数全是对象 */
    MSTA_SDC_NUMBER,       /* 第一个位置参数是数值 */
    MSTA_SDC_WORD          /* 第一个位置参数是一个词（set_case_analysis 的 0/1/zero/one） */
} MstaSdcValueKind;

/* 解析结果。下标为 k 的数组对应选项表的第 k 行；数组都挂在本条命令的
   临时内存上（读下一条命令时一起释放）。 */
typedef struct {
    const MstaSdcOpt *pOpts;   /* 这条命令的选项表 */
    const char   *pName;       /* 命令名（argv[0]），告警用 */
    int           argc;
    char        **argv;
    int          *pAt;         /* 选项出现的位置（argv 下标），0 = 没出现 */
    const char  **ppValue;     /* MSTA_SDC_VALUE / NUMBERS 选项的值 */
    char         *pEdge;       /* MSTA_SDC_RF 选项用的写法：'r' / 'f' / 0 */
    int          *pListOpt;    /* MSTA_SDC_OBJECTS 选项的每次出现，按顺序：选项表下标、 */
    char         *pListEdge;   /*   边沿（'r' / 'f' / 0）、 */
    char       ***pppListWords;/*   对象名数组、 */
    int          *pListCount;  /*   对象个数 */
    int           nLists;
    const char   *pValue;      /* 位置参数开头的值；没有值的命令为 NULL */
    char        **ppObjs;      /* 位置参数里的对象 */
    int           nObjs;
} MstaSdcCmd;

/* 命令的处理函数：拿解析结果做语义——查对象、换算单位、写模型。 */
typedef void (*MstaSdcHandler)( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd );

/* 分发表的一行：一条命令的完整语法加处理函数。 */
typedef struct {
    const char       *pName;
    const MstaSdcOpt *pOpts;      /* 选项表 */
    MstaSdcValueKind  Value;      /* 位置参数开头的值 */
    int               nMinObjs;   /* 对象至少几个 */
    int               nMaxObjs;   /* 对象最多几个，-1 = 不限 */
    MstaSdcHandler    pfHandler;
} MstaSdcCmdDef;

/* 数值的取值范围。 */
#define MSTA_SDC_ANY       0   /* 任意数 */
#define MSTA_SDC_NONNEG    1   /* 不能为负，如负载、摆率、面积 */
#define MSTA_SDC_POSITIVE  2   /* 必须为正，如周期、电压、倍数 */

/* ---------------- msta_sdc.c：展开后的参数与本条命令的临时内存 ---------------- */

/* 临时内存挂在本条命令上，读下一条命令时一起释放。 */
void *Msta_SdcArenaKeep( void *pMem );
char *Msta_SdcArena( const char *pFormat, ... );
/* 对象名来自哪种 Tcl 集合（'P'/'N'/'G'/'C'/'I'/...，裸名字为 0）、集合是否带 -quiet。 */
char Msta_SdcKindOf( const char *pText );
int Msta_SdcIsQuiet( const char *pText );

/* ---------------- msta_sdc_parse.c：小工具、通用解析器、公共子语法 ---------------- */

int Msta_SdcGlobMatch( const char *pPattern, const char *pText );
double Msta_SdcToPs( MstaSdc *p, const char *pText );
int Msta_SdcResolveNetsInto( MstaDesign *pDes, const char *pTarget, MstaSdcIntArray *vOut );
int Msta_SdcResolveNets( MstaDesign *pDes, const char *pTarget, int **ppNets );

void Msta_SdcReject( MstaSdc *p, const MstaSdcCmd *pCmd, const char *pFormat, ... );
void Msta_SdcNote( const MstaSdcCmd *pCmd, const char *pFormat, ... );
void *Msta_SdcArenaZeros( int n, size_t nSize );
int Msta_SdcParseCmd( MstaSdc *p, const MstaSdcCmdDef *pDef, int argc, char **argv,
                      const int *pArg, MstaSdcCmd *pCmd );
int Msta_SdcHasFlag( const MstaSdcCmd *pCmd, const char *pName );
const char *Msta_SdcOptValue( const MstaSdcCmd *pCmd, const char *pName );
char Msta_SdcOptEdge( const MstaSdcCmd *pCmd, const char *pName );
int Msta_SdcOptList( const MstaSdcCmd *pCmd, const char *pName, char ***pppWords );
int Msta_SdcRequire( MstaSdc *p, const MstaSdcCmd *pCmd, const char *pName );

extern const MstaSdcOpt Msta_SdcNoOpts[];
extern const MstaSdcOpt Msta_SdcMinMaxRiseFallOpts[];
int Msta_SdcGetNumber( MstaSdc *p, const MstaSdcCmd *pCmd, const char *pWhat,
                       const char *pText, int Range, double *pValue );
void Msta_SdcMinMaxRiseFall( int fMin, int fMax, int fRise, int fFall, int Sel[2][2] );
void Msta_SdcCmdMinMaxRiseFall( const MstaSdcCmd *pCmd, int Sel[2][2] );
void Msta_SdcStoreMinMaxRiseFall( double Target[2][2], int Sel[2][2], double Value );
void Msta_SdcStoreMinMaxRiseFall4( int Sel[2][2], double Value,
                                   double *pMaxRise, double *pMaxFall,
                                   double *pMinRise, double *pMinFall );
char Msta_SdcCmdEdge( const MstaSdcCmd *pCmd );
int Msta_SdcObjectNets( MstaSdc *p, MstaDesign *pDes, const MstaSdcCmd *pCmd,
                        char **ppNames, int nNames, int **ppNets );
int Msta_SdcObjectClocks( MstaSdc *p, const MstaSdcCmd *pCmd,
                          char **ppNames, int nNames, int **ppClocks );
int Msta_SdcObjectInsts( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, const MstaSdcCmd *pCmd,
                         char **ppNames, int nNames, int fCellNames, int **ppInsts );

/* ---------------- 各条命令：选项表与处理函数（分发表 s_vSdcCommands 引用） ---------------- */

/* msta_sdc_clock.c */
extern const MstaSdcOpt Msta_SdcCreateClockOpts[];
extern const MstaSdcOpt Msta_SdcGeneratedClockOpts[];
extern const MstaSdcOpt Msta_SdcClockUncertaintyOpts[];
extern const MstaSdcOpt Msta_SdcClockLatencyOpts[];
extern const MstaSdcOpt Msta_SdcClockSenseOpts[];
extern const MstaSdcOpt Msta_SdcClockGroupsOpts[];
void Msta_SdcCreateClock( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd );
void Msta_SdcCreateGeneratedClock( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd );
void Msta_SdcSetClockUncertainty( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd );
void Msta_SdcSetClockLatency( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd );
void Msta_SdcSetPropagatedClock( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd );
void Msta_SdcSetClockTransition( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd );
void Msta_SdcSetClockSense( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd );
void Msta_SdcSetClockGroups( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd );

/* msta_sdc_io.c */
extern const MstaSdcOpt Msta_SdcPortDelayOpts[];
extern const MstaSdcOpt Msta_SdcLoadOpts[];
extern const MstaSdcOpt Msta_SdcInputTransitionOpts[];
extern const MstaSdcOpt Msta_SdcDrivingCellOpts[];
extern const MstaSdcOpt Msta_SdcIdealNetworkOpts[];
extern const MstaSdcOpt Msta_SdcDisableTimingOpts[];
void Msta_SdcSetInputDelay( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd );
void Msta_SdcSetOutputDelay( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd );
void Msta_SdcSetLoad( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd );
void Msta_SdcSetInputTransition( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd );
void Msta_SdcSetDrivingCell( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd );
void Msta_SdcSetIdealNetwork( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd );
void Msta_SdcSetIdealLatency( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd );
void Msta_SdcSetIdealTransition( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd );
void Msta_SdcSetCaseAnalysis( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd );
void Msta_SdcSetLogicZero( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd );
void Msta_SdcSetLogicOne( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd );
void Msta_SdcSetLogicDc( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd );
void Msta_SdcSetDisableTiming( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd );

/* msta_sdc_except.c */
extern const MstaSdcOpt Msta_SdcPathExceptionOpts[];
extern const MstaSdcOpt Msta_SdcPathDelayOpts[];
extern const MstaSdcOpt Msta_SdcGroupPathOpts[];
extern const MstaSdcOpt Msta_SdcDataCheckOpts[];
extern const MstaSdcOpt Msta_SdcClockGatingCheckOpts[];
void Msta_SdcSetFalsePath( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd );
void Msta_SdcSetMulticyclePath( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd );
void Msta_SdcSetMaxDelay( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd );
void Msta_SdcSetMinDelay( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd );
void Msta_SdcSetGroupPath( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd );
void Msta_SdcSetDataCheck( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd );
void Msta_SdcSetMaxTimeBorrow( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd );
void Msta_SdcSetClockGatingCheck( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd );

/* msta_sdc_env.c */
extern const MstaSdcOpt Msta_SdcDrcLimitOpts[];
extern const MstaSdcOpt Msta_SdcTimingDerateOpts[];
extern const MstaSdcOpt Msta_SdcOperatingConditionsOpts[];
extern const MstaSdcOpt Msta_SdcVoltageOpts[];
extern const MstaSdcOpt Msta_SdcUnitsOpts[];
void Msta_SdcSetMaxTransition( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd );
void Msta_SdcSetMaxFanout( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd );
void Msta_SdcSetMaxCapacitance( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd );
void Msta_SdcSetMinCapacitance( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd );
void Msta_SdcSetMaxArea( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd );
void Msta_SdcSetTimingDerate( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd );
void Msta_SdcSetOperatingConditions( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd );
void Msta_SdcSetVoltage( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd );
void Msta_SdcSetUnits( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd );
/* 没点名工作条件时用的角（msta_sdc_query.c 报告工作条件时也用）。 */
MstaLibInfo *Msta_SdcDefaultOpCond( MstaLib *pLib, MstaId nLibrary, MstaOpCond **ppCond );

/* ---------------- msta_sdc_query.c ---------------- */

/* 释放例外表索引及查询用的临时数组（Msta_SdcFree 调用）。 */
void Msta_SdcExIndexRelease( MstaSdc *p );

#endif
