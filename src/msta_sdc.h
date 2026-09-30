/**CHeader*************************************************************

  FileName    [msta_sdc.h]

  Synopsis    [时序约束（SDC 子集）：数据模型 + 读入。]

  ---------------------------------------------------------------------
  支持的 SDC 子集
  ---------------------------------------------------------------------
  命令与选项清单见 README.md 的"支持的 SDC 子集"一节，本文件只负责这些命令的
  数据模型与读入。
  单位约定：SDC 时间默认 ns，可用 set_units 改变，内部统一为 ps。
  未建模的命令记录告警并计入忽略计数。

  ---------------------------------------------------------------------
  Tcl 前端
  ---------------------------------------------------------------------
  scripts/sdc_bridge.tcl 用 Tcl 处理变量、source、续行和集合命令，输出 JSON
  命令数组。此处保留集合的对象类型，应用 msta 支持的约束语义。

***********************************************************************/

#ifndef MSTA_SDC_H
#define MSTA_SDC_H

#include "msta_util.h"
#include "msta_net.h"

#define MSTA_UNSET (1e30)       /* 没约束过的量都填这个，判断用 !Msta_IsSet(x) */
#define Msta_IsSet(x) ( (x) < MSTA_UNSET / 2.0 )

/* 报告分组的保留名字：group_path -default 的兜底组、异步检查组。 */
#define MSTA_SDC_GROUP_DEFAULT "**default**"
#define MSTA_SDC_GROUP_ASYNC   "**async**"

/* ---------------- 时钟 ---------------- */
typedef struct {
    MstaId Name;
    double Period;            /* ps */
    double UncertaintySetup;  /* ps，从 setup 要求时间里减掉 */
    double UncertaintyHold;   /* ps，加到 hold 要求时间里 */
    double RiseEdge, FallEdge; /* 一个周期内的边沿相位，ps */
    double SourceLatencyMax, SourceLatencyMin;
    double NetworkLatencyMax, NetworkLatencyMin;
    double SlewMax, SlewMin;   /* set_clock_transition：时钟源上的摆率，ps */
    int    fPropagated;
    MstaId MasterClock;      /* generated clock 的主时钟名；普通时钟为 -1 */
    int    SourceNet;         /* create_clock 挂在哪个全局网络上；-1 是虚拟时钟 */
    MstaId SourceText;        /* 原始写法，报错时回显 */
} MstaClock;
MstaArrayDefine( MstaClock, MstaClockArray )

/* ---------------- 每条被约束网络一份 ---------------- */
typedef struct {
    int      Net;             /* 全局网络号 */
    double   LoadMax, LoadMin; /* set_load, fF */
    int      fSubtractPinLoad; /* -subtract_pin_load：给的是总负载，要减掉脚电容 */
    /* 分对象的 DRC 限制（set_max_transition/-capacitance/-fanout 带对象时）。 */
    double   DrcMaxTransition, DrcMaxCapacitance, DrcMaxFanout;
    double   DrcMinCapacitance;   /* set_min_capacitance 的分对象限制 */
    double   InputSlewMax, InputSlewMin; /* input transition / driving cell, ps */
    double   InputSlewMaxRise, InputSlewMaxFall;
    double   InputSlewMinRise, InputSlewMinFall;
    /* set_driving_cell：外部驱动单元。分析时按端口实际负载查它的延迟与摆率，
       并把延迟加到端口到达上（与 DC/PT/OpenSTA 的输入驱动模型一致）。 */
    MstaId   DrivingCell;     /* 单元名（可用 "库名/cell 名" 限定） */
    MstaId   DrivingPin;      /* -pin：驱动单元的输出脚，未写则取第一个组合弧 */
    MstaId   DrivingFromPin;  /* -from_pin：驱动单元的输入脚 */
    double   DriveInSlewRise, DriveInSlewFall;  /* -input_transition_rise/fall */
    double   DriveMultiply;   /* -multiply_by，默认 1.0 */
    /* set_ideal_network / set_ideal_latency / set_ideal_transition：
       理想网络上的延迟不累计（模型说明见 msta_timing.c 的 PropagateClocks）。 */
    int      fIdeal;            /* 这个网络被 set_ideal_network 标过 */
    int      fIdealNoPropagate; /* -no_propagate：理想属性不往下游网络传 */
    double   IdealLatencyMaxRise, IdealLatencyMaxFall;
    double   IdealLatencyMinRise, IdealLatencyMinFall;
    double   IdealTranMaxRise, IdealTranMaxFall;
    double   IdealTranMinRise, IdealTranMinFall;
} MstaNetCons;
MstaArrayDefine( MstaNetCons, MstaNetConsArray )

/* ---------------- 时钟间不确定度（set_clock_uncertainty -from/-to） ---------------- */
typedef struct {
    MstaId FromClock, ToClock;   /* MSTA_NO_ID = 任意时钟 */
    char   FromRF, ToRF;         /* 0=两个边沿都算，'r'/'f' 限定边沿 */
    int    fSetup, fHold;
    double Value;                /* ps */
} MstaInterClockUnc;
MstaArrayDefine( MstaInterClockUnc, MstaInterClockUncArray )

typedef struct {
    int Net;
    MstaId Clock;
    double Max, Min;
    int ClockFall;
    int DataRise; /* -1=both, 0=fall, 1=rise */
    int RefNetMax, RefNetMin; /* -1=clock source reference */
    int SourceLatencyIncludedMax, SourceLatencyIncludedMin;
    int NetworkLatencyIncludedMax, NetworkLatencyIncludedMin;
} MstaIoDelay;
MstaArrayDefine( MstaIoDelay, MstaIoDelayArray )

/* ---------------- 时钟极性（set_clock_sense） ---------------- */
/* 某个网络上的时钟相对源是不是反相的（-negative），以及这个时钟要不要
   在这里停下来（-stop_propagation）。-clock 限定只对这些时钟生效。 */
typedef struct {
    int    Net;
    MstaId Clock;         /* MSTA_NO_ID = 对所有时钟生效 */
    int    Polarity;      /* +1 / -1；0 = 只写了 -stop_propagation */
    int    fStop;
} MstaClockSense;
MstaArrayDefine( MstaClockSense, MstaClockSenseArray )

/* ---------------- 数据对数据的检查（set_data_check） ---------------- */
/* -from 的数据路径提供参照，-to 的数据按 margin 检查：同一时钟下就是
   "to 要比 from 早（setup）/ 晚（hold）margin"。 */
typedef struct {
    int    FromNet, ToNet;
    MstaId FromText, ToText;   /* 原样名字，报告里用 */
    char   FromRF, ToRF;       /* 0 = 两条边沿都算，'r'/'f' */
    int    fSetup, fHold;
    double Value;              /* ps */
} MstaDataCheck;
MstaArrayDefine( MstaDataCheck, MstaDataCheckArray )

typedef struct {
    int Inst;
    MstaId FromPin;
    MstaId ToPin;
} MstaDisabledArc;
MstaArrayDefine( MstaDisabledArc, MstaDisabledArcArray )

/* ---------------- 例外路径（false / multicycle / 路径预算） ---------------- */

/* -through 列表：Kind==MSTA_SDC_THRU_SEP 的条目开始一个新分组。组内对象取"或"，
  分组之间按路径顺序依次匹配，对应 SDC 里多次 -through 的写法。 */
#define MSTA_SDC_MAX_THRU 16
#define MSTA_SDC_THRU_SEP 1
typedef struct {
    MstaId Text;
    char   Kind;             /* MSTA_SDC_THRU_SEP = 分组分隔；否则 C/I/P/N（0 = 裸名字） */
    char   RF;               /* -rise_through / -fall_through：'r'/'f'，0 = 不限边沿 */
} MstaThruObject;

typedef struct {
    MstaId FromText;          /* 原样存名字，分析时再按名字匹配 */
    MstaId ToText;
    char   FromKind, ToKind; /* C=clock, I=cell, P=port, N=pin/net */
    char   FromRF, ToRF;     /* 0=两个边沿都算，'r'=只看上升，'f'=只看下降 */
    MstaThruObject Thru[MSTA_SDC_MAX_THRU];
    int    nThru;
    int    nSetupCycles;
    int    nHoldShift;       /* 显式 -hold 从 setup 派生的 hold 边沿回退几拍 */
    int    fApplySetup, fApplyHold;
    int    fFalseSetup, fFalseHold;
    int    fMaxDelay, fMinDelay;
    double MaxDelay, MinDelay;
} MstaException;
MstaArrayDefine( MstaException, MstaExceptionArray )

/* ---------------- 路径分组（group_path） ---------------- */
/* 一条 group_path：命中的路径归到这个组里，报告按组出 WNS/TNS。
   -default 是"命名组都不命中时的兜底组"，-weight 只记录（参考工具也只记录）。 */
typedef struct {
    MstaId Name;              /* -name；-default 时是 MSTA_NO_ID */
    double Weight;            /* -weight，未写时 1.0 */
    int    fDefault;          /* 1 = -default 兜底组 */
    MstaId FromText, ToText;
    char   FromKind, ToKind;
    char   FromRF, ToRF;
    MstaThruObject Thru[MSTA_SDC_MAX_THRU];
    int    nThru;
} MstaPathGroup;
MstaArrayDefine( MstaPathGroup, MstaPathGroupArray )

/* ---------------- 时钟门控检查（set_clock_gating_check） ---------------- */
/* 使能脚相对时钟脚的 setup/hold 值。没有对象列表的作为全局默认（Inst = -1），
   带对象列表的按实例覆盖。两个角各自 MSTA_UNSET 表示这一角没设过。 */
typedef struct {
    int    Inst;             /* 实例号；-1 = 全局默认 */
    double Setup, Hold;
    int    fRise, fFall;     /* -rise/-fall：限定使能边沿 */
    int    fHigh, fLow;      /* -high/-low：限定时钟有效电平 */
} MstaClockGatingSdc;
MstaArrayDefine( MstaClockGatingSdc, MstaClockGatingSdcArray )

/* ---------------- 锁存器借时（set_max_time_borrow） ---------------- */
/* 锁存器 D 脚可以比使能脚关闭沿晚到多久。Inst = -1 表示全局默认。 */
typedef struct {
    int    Inst;
    double Value;            /* ps */
} MstaBorrowSdc;
MstaArrayDefine( MstaBorrowSdc, MstaBorrowSdcArray )

/* ---------------- 分对象的 derate（set_timing_derate 带对象时） ---------------- */
/* 实例（'I'）或时钟（'C'）上的 derate。参考工具里分对象的值**覆盖**全局值，
   不是相乘；同一条命令可以只用其中一个边沿（-rise/-fall）。 */
typedef struct {
    char   Kind;              /* 'I' 实例 / 'C' 时钟 */
    int    Inst;              /* Kind='I' 的实例号 */
    MstaId Name;              /* Kind='C' 的时钟名 */
    int    fRise, fFall;      /* 只对某个边沿生效；都为 0 = 两个边沿都算 */
    int    fCellCheck;        /* 这一条是 derate 检查值（-cell_check） */
    double Early, Late;
} MstaObjDerate;
MstaArrayDefine( MstaObjDerate, MstaObjDerateArray )

/* 例外匹配用的端点：可打印名字 + 可选的 "实例/引脚" 形式。
   nInst/nPin 为 -1 表示这个端点没有引脚（端口、时钟、纯组合实例）。 */
typedef struct {
    const char *pText;       /* 网络名 / 实例名 / 端口名 */
    int         nInst;
    int         nPin;
    int         nNet;        /* 这个点在哪个网络上（-1 = 没有） */
    int         fRises;      /* 这个点上的信号边沿：1=上升，0=下降，-1=未知 */
} MstaSdcObject;

/* 找次优路径时要排除的 (网络, 边沿)：命中 -through 的那一段被排除后重算。 */
typedef struct {
    int Net;
    int EdgeMask;            /* 1=上升，2=下降，3=两个边沿都排除 */
} MstaPathExclude;

/* 一个端点在匹配时的全部信息：对象、所属时钟和信号边沿。 */
typedef struct {
    const MstaSdcObject *pObj;
    const char          *pClock;
    int                  fRises;  /* 1=上升沿, 0=下降沿, -1=边沿未知 */
} MstaSdcEndpoint;

typedef struct MstaSdc {
    MstaClockArray     vClocks;
    MstaIntMap         clockMap;      /* 时钟名 nameId -> vClocks 下标 */
    MstaNetConsArray   vNets;
    MstaIoDelayArray   vInputDelays, vOutputDelays;
    MstaClockSenseArray vClockSense;
    MstaDataCheckArray vDataChecks;
    MstaDisabledArcArray vDisabledArcs;
    MstaIntMap         netConsMap;    /* 全局网络号 -> vNets 下标 */
    MstaExceptionArray vExceptions;
    MstaPathGroupArray vPathGroups;
    MstaClockGatingSdcArray vClockGating;
    MstaBorrowSdcArray vBorrow;
    MstaObjDerateArray vObjDerate;
    MstaInterClockUncArray vInterClockUnc;
    int                nCommandsRead;
    int                nCommandsIgnored;
    /* set_timing_derate：全局 OCV 系数，默认 1.0。
       Early 用在 min（hold）角，Late 用在 max（setup）角。 */
    double             DerateEarly, DerateLate;
    double             DerateCheckEarly, DerateCheckLate;
    int                fDerateClock;   /* 1 = derate 时钟路径上的单元延迟 */
    int                fDerateData;    /* 1 = derate 数据路径上的单元延迟 */
    /* 设计规则限制（只有 [current_design] 这种全局写法会记下来）。 */
    double             MaxTransition;   /* ps */
    double             MaxCapacitance;  /* fF */
    double             MinCapacitance;  /* fF：set_min_capacitance 的全局限制 */
    double             MaxFanout;       /* 扇出个数 */
    double             MaxArea;         /* set_max_area 的面积目标 */
    /* set_operating_conditions / set_voltage：选中的工艺角与电压（MSTA_UNSET = 没设）。 */
    MstaId             OpCondMax, OpCondMin;   /* operating condition 名字，MSTA_NO_ID = 用库里默认的 */
    MstaId             OpCondLibraryMax, OpCondLibraryMin; /* 分角选用的 Liberty library */
    double             VoltageMax, VoltageMin; /* set_voltage 给的电压 */
    double             TempMax, TempMin;       /* set_operating_conditions 给的分析温度 */
    double             KFactorDerateLate, KFactorDerateEarly; /* K 因子换算出的延迟系数 */
    double             TimeScalePs;  /* SDC 数值乘此值 -> ps，默认 ns */
    double             CapScaleFf;   /* 0 表示沿用 Liberty 电容单位 */
} MstaSdc;

MstaSdc *Msta_SdcStart( void );
void     Msta_SdcFree( MstaSdc *p );
void     Msta_SdcSetBridgePath( const char *pExecutable );
/* 读一个 .sdc 文件。pDesign 用来把端口/引脚名解析成全局网络号。 */
int      Msta_SdcReadFile( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib,
                           const char *pFileName, int fVerbose );

MstaClock   *Msta_SdcFindClock( MstaSdc *p, const char *pName );
MstaClock   *Msta_SdcClockByIndex( MstaSdc *p, int i );
int          Msta_SdcClockCount( MstaSdc *p );
/* 时钟名 id -> 下标（-1 表示没有这个时钟）。约束里存的是名字，引擎要按下标用。 */
int          Msta_SdcClockIndexOf( MstaSdc *p, MstaId NameId );
MstaNetCons *Msta_SdcNetCons( MstaSdc *p, int nNet );           /* 可能为 NULL */
MstaNetCons *Msta_SdcNetConsOrCreate( MstaSdc *p, int nNet );
/* 理想网络：网络被标过 / 理想属性不往扇出传（见 set_ideal_network）。 */
int          Msta_SdcIsIdealNet( MstaSdc *p, int nNet );
int          Msta_SdcIdealNoPropagate( MstaSdc *p, int nNet );
/* 理想网络上的延迟/摆率：fRise=1/0 取单个边沿，-1 取该分析角最保守的值。
   没设过返回 MSTA_UNSET。 */
double       Msta_SdcIdealLatency( MstaSdc *p, int nNet, int fMax, int fRise );
double       Msta_SdcIdealTransition( MstaSdc *p, int nNet, int fMax, int fRise );
/* set_clock_sense：这根网络上这个时钟的极性（+1/-1，0 = 没设过）；
   *pfStop = 1 表示这个时钟到这里就不再往下传。 */
int          Msta_SdcClockSense( MstaSdc *p, int nNet, int nClock, int *pfStop );
/* 这个分析角上生效的工艺角（名字 / 电压 / 温度），给报告用。 */
void         Msta_SdcOpCondInfo( MstaSdc *p, MstaLib *pLib, int fMax,
                                 const char **ppName, double *pVoltage, double *pTemp );
/* 有没有用过 set_operating_conditions / set_voltage（报告决定要不要打这一行）。 */
int          Msta_SdcOpCondSelected( MstaSdc *p );
/* 当前 setup/max 或 hold/min 分析使用的 Liberty library。 */
MstaId       Msta_SdcOperatingLibrary( MstaSdc *p, int fMax );
/* set_data_check：条数与逐条查询（引擎在传播完之后逐条算 slack）。 */
int          Msta_SdcDataCheckCount( MstaSdc *p );
MstaDataCheck *Msta_SdcDataCheckByIndex( MstaSdc *p, int i );
double       Msta_SdcPortDelay( MstaSdc *p, int nNet, int nClock,
                                int fOutput, int fMax );
int          Msta_SdcPortReferenceNet( MstaSdc *p, int nNet, int nClock,
                                       int fOutput, int fMax );
int          Msta_SdcPortSourceLatencyIncluded( MstaSdc *p, int nNet, int nClock,
                                                int fOutput, int fMax );
int          Msta_SdcPortNetworkLatencyIncluded( MstaSdc *p, int nNet, int nClock,
                                                 int fOutput, int fMax );
/* set_timing_derate 的全局系数：数据路径 / 时钟路径 / 单元检查项。 */
double       Msta_SdcDataDerate( MstaSdc *p, int fMax );
double       Msta_SdcClockDerate( MstaSdc *p, int fMax );
double       Msta_SdcCheckDerate( MstaSdc *p, int fMax );
/* 一条检查用的不确定度：先找 -from/-to 的专用条目，再退回捕获时钟的标量值。 */
double       Msta_SdcClockUncertainty( MstaSdc *p, int nLaunchClock, int nCaptureClock,
                                       int fSetup, int fLaunchRises, int fCaptureRises );
/* 有没有设过 set_max_transition / set_max_fanout / set_max_capacitance。 */
int          Msta_SdcHasDrcLimits( MstaSdc *p );
int          Msta_SdcPortClockFall( MstaSdc *p, int nNet, int nClock,
                                    int fOutput, int fMax );
int          Msta_SdcTimingDisabled( MstaSdc *p, int nInst, MstaId FromPin, MstaId ToPin );
void         Msta_SdcPrintClocks( MstaSdc *p, MstaDesign *pDes, FILE *pFile );

/* 例外匹配：给定起点/终点对象（含 "实例/引脚" 形式）和路径上的对象序列，
   返回该路径的 setup/hold 周期数和是否被 false path 掉。 */
void Msta_SdcFindExceptionPath( MstaSdc *p, MstaDesign *pDes,
                                const MstaSdcEndpoint *pFrom, const MstaSdcEndpoint *pTo,
                                int fSetup, const MstaSdcObject *pObjects, int nObjects,
                                int *pnCycles, int *pfFalse );
double Msta_SdcFindPathDelay( MstaSdc *p, MstaDesign *pDes,
                              const MstaSdcEndpoint *pFrom, const MstaSdcEndpoint *pTo,
                              const MstaSdcObject *pObjects, int nObjects, int fMax );
/* 这条路径被 false path 命中时，为了找次优路径要排除哪些 (网络, 边沿)：
   取命中的那条例外的 -through 点上匹配到的对象。返回个数。 */
int  Msta_SdcPathExclusions( MstaSdc *p, MstaDesign *pDes,
                             const MstaSdcEndpoint *pFrom, const MstaSdcEndpoint *pTo,
                             int fSetup, const MstaSdcObject *pObjects, int nObjects,
                             MstaPathExclude *pOut, int nCap );
int  Msta_SdcNeedsStartpointPartition( MstaSdc *p );

/* group_path：路径分组。分组只影响报告怎么组织、按组统计 WNS/TNS，
   不改变任何 slack。 */
int  Msta_SdcPathGroupCount( MstaSdc *p );
MstaPathGroup *Msta_SdcPathGroupByIndex( MstaSdc *p, int i );
/* 有没有用过 group_path（报告决定要不要打分组那一节）。 */
int  Msta_SdcPathGroupsUsed( MstaSdc *p );
/* 一个端点在哪个分组：按书写顺序找第一条命中的命名组，都没命中再看 -default 组；
   连 -default 也没有就返回 MSTA_NO_ID，由调用方按捕获时钟归组。 */
MstaId Msta_SdcFindPathGroup( MstaSdc *p, MstaDesign *pDes,
                              const MstaSdcEndpoint *pFrom, const MstaSdcEndpoint *pTo,
                              const MstaSdcObject *pObjects, int nObjects );
/* 分组的权重；这个名字不是分组时返回 1.0。 */
double Msta_SdcPathGroupWeight( MstaSdc *p, MstaId NameId );

/* set_clock_gating_check：这个实例上的门控检查值（ps）。*pfSet=0 表示 SDC 里
   没给这一角，交给库里的约束弧（再没有就按 0）。 */
void Msta_SdcClockGatingValue( MstaSdc *p, int nInst, int fMax,
                               double *pValue, int *pfSet );
/* 有没有写过 set_clock_gating_check。 */
int  Msta_SdcHasClockGating( MstaSdc *p );

/* set_max_time_borrow：这个锁存器允许的借时（ps），没设过返回 0。 */
double Msta_SdcMaxTimeBorrow( MstaSdc *p, int nInst );

/* 分对象 derate：数据路径按实例查（含 clk-to-Q），时钟路径按时钟查；
   分对象的值覆盖全局值。fRise = 1/0 指定边沿，-1 表示不分边沿。 */
double Msta_SdcDataDerateInst( MstaSdc *p, int nInst, int fMax, int fRise );
double Msta_SdcCheckDerateInst( MstaSdc *p, int nInst, int fMax );
double Msta_SdcClockDerateClock( MstaSdc *p, int nClock, int fMax );

#endif /* MSTA_SDC_H */
