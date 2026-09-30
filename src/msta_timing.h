/**CHeader*************************************************************

  FileName    [msta_timing.h]

  Synopsis    [时序引擎：时钟网络 -> 拓扑序 -> 到达/要求时间 -> slack -> 路径。]

  ---------------------------------------------------------------------
  分析流程
  ---------------------------------------------------------------------
   1  找时钟网络   从 create_clock 挂的那个网络出发，向后(源头方向)把所有
                   只服务于时钟的连线标出来。寄存器时钟脚的插入延迟就在这里算。
   2  定端点        每个时序单元的每条 setup/hold 检查是一个端点；顶层输出脚
                   也算端点（reg2out）。每个 FF 的 Q 脚是起点。
   3  拓扑序        组合环路要能发现并报错；有环路就没法算 arrival。
                   用显式栈做迭代 DFS，不用递归 —— 深链（十万级）会爆栈。
   4  前向到达 / 后向要求
                   每一遍只算一个方向、一个角：
                     max 遍 -> setup 检查（用最坏延迟）
                     min 遍 -> hold  检查（用最好延迟）
   5  路径回溯      每个网络记住"是谁把最大到达时间传进来的"（前驱指针），
                   于是 report_checks 能打印一条完整的路径。

  ---------------------------------------------------------------------
  延迟怎么算（NLDM，单角保守模型）
  ---------------------------------------------------------------------
  一根弧的延迟 = 查表(输入摆率, 输出负载)。Liberty 给了上升/下降两张表，
  当前数据传播按 max/min 角度保留到达时间和转换时间，并使用 Liberty 的 rise/fall
  表参与弧延迟计算。每个起点时钟分别传播一次 arrival。

  当前模型不包含信号完整性、多电压和 CCS/ECSM。

***********************************************************************/

#ifndef MSTA_TIMING_H
#define MSTA_TIMING_H

#include "msta_lib.h"
#include "msta_net.h"
#include "msta_sdc.h"

typedef struct {
    double Slack;
    double Arrival, Required;
    int    fDataRise;         /* 获胜路径的数据边沿：1 = rise，0 = fall */
    /* 要求时间的组成成分，报告层直接拿来打印（不让报告层重新推导一遍） */
    double CheckTime, Uncertainty;
    double Borrow;            /* set_max_time_borrow 给锁存器放宽的那部分 */
    int    fChecked;          /* 0 = 没约束/被 false path 掉 */
    MstaId LaunchClock;       /* 起点时钟名 nameId */
    double LaunchTime;        /* 起点那一拍 */
    double CaptureTime;       /* capture 边沿 */
    int    *pPath;            /* 终点 -> 起点，获胜路径的快照 */
    double *pPathArrival;
    int     nPath;
    /* 跨时钟时最差的那对边沿不一定都在第 0 拍：出发沿在第 k 拍时，报告里的
       路径时间整体平移 k 个出发周期（slack 只跟两个边沿的差有关）。 */
    double LaunchShift;
    /* 这条检查归在哪个路径分组（group_path）。没有 group_path 时按捕获时钟名字
       归组，异步检查组是 **async**。分组只影响报告与按组统计，不影响 slack。 */
    MstaId Group;
} MstaCheckCorner;

/* 一条时序检查（路径端点）。
   fToRegister=1：终点是某个 FF 的数据脚（reg2reg 或 input2reg）
   fToRegister=0：终点是顶层输出脚（reg2out） */
typedef struct {
    MstaId InstId;            /* fToRegister=1 时是那个 FF */
    int    nCheck;            /* 该 cell 的 vRegs 下标 */
    int    nDataPin;          /* 数据脚在 vPins 里的下标 */
    int    nEndNet;           /* 被检查的网络：FF 的数据网络 或 顶层输出网络 */
    int    fToRegister;
    int    fAsync;
    int    fRecovery;
    int    fRemoval;
    int    nAsyncCheck;

    /* 分析结果（一个端点同时有 setup 与 hold 两条检查） */
    MstaCheckCorner Setup, Hold;
    MstaId CaptureClock;      /* 终点时钟名 nameId */
    int    fCutByException;
    int    fPathSaved;        /* 1 = 候选里已经存好逐边沿路径快照，不用再回溯一遍 */
} MstaCheck;
MstaArrayDefine( MstaCheck, MstaCheckArray )

static inline MstaCheckCorner *Msta_CheckCorner( MstaCheck *pCheck, int fSetup )
{
    return fSetup ? &pCheck->Setup : &pCheck->Hold;
}

/* 时钟网络上的一个 FF 时钟脚的信息 */
typedef struct {
    double MaxArrival;        /* ps：最慢那条时钟路的插入延迟 */
    double MinArrival;        /* ps：最快那条 */
    double MaxSlew;           /* ps：该时钟网络的最大/最小摆率 */
    double MinSlew;
    int    fReached;
    int    nThroughGates;     /* 时钟路径经过的组合级数 */
    int    Polarity;          /* +1 / -1：这个点上时钟相对源是不是反相的 */
} MstaClockArr;

/* 逐边沿的路径前驱：每个网络按 max/min × rise/fall 四个槽各记一份，
   这样一条路径上每个点的信号边沿都能沿链反推（-rise_through 之类要用）。 */
typedef struct {
    int Net;                  /* 前驱网络；-1 = 这个点就是起点 */
    int Edge;                 /* 前驱网络上的边沿：1 = 上升，0 = 下降 */
} MstaPrev;

/* 找次优路径时排除的 (网络, 边沿) 个数上限。 */
#define MSTA_MAX_PATH_EXCLUDE 8

/* 跨时钟的边沿对齐结果：在"公共周期"里找出的最差 (出发拍, 捕捉拍)。
   同一对 (时钟, 边沿, 角) 只算一次，避免在每个端点上重复搜索。 */
#define MSTA_MAX_CLOCK_PAIRS 256
typedef struct {
    int LaunchClock, CaptureClock;
    int LaunchRises, CaptureRises, fSetup;
    int LaunchCycle, CaptureCycle;
    int fValid;
} MstaClockPair;

typedef struct {
    double *pArr;              /* [nNets] 到达时间（max 角用于 setup 遍，min 角用于 hold 遍） */
    double *pArrRise, *pArrFall;
    double *pSlew;             /* [nNets] 输出摆率，驱动下一级查表用 */
    double *pSlewRise, *pSlewFall;
    /* 起点信息：这根网络上的数据是从哪个时钟、哪一拍出发的。
       检查时要用它决定 setup/hold 落在哪一拍，报告时要打印 startpoint。 */
    int    *pnLaunchClock;     /* [nNets] 起点时钟在 sdc 里的下标，-1 表示没有 */
    double *pdLaunchEdge;      /* [nNets] 起点那一拍的到达时间（时钟插入延迟或输入延迟） */
    char   *pfLaunchRises;     /* [nNets] 1=上升沿 launch，0=下降沿 */
    /* 路径回溯用的前驱 */
    int    *pPrevNet;          /* [nNets] 前驱网络 */
    int    *pPrevInst;         /* [nNets] 驱动它的那个实例 */
    int    *pPrevPin;          /* [nNets] 该实例的哪个输入脚把最坏到达传进来的 */
    /* 逐边沿前驱（上升/下降各一份） */
    MstaPrev *pPrevRise, *pPrevFall;
} MstaCorner;

typedef struct MstaTiming {
    MstaDesign *pDes;
    MstaLib    *pLib;
    MstaSdc    *pSdc;

    MstaCheckArray vChecks;

    /* 时钟 */
    char   *pfClockNet;        /* [nNets] 该网络是否属于时钟树 */
    /* 同一根网络可以挂多个时钟（create_clock -add），所以"哪个时钟在哪根网络上"
       和时钟到达都按 [时钟][网络] 存，行优先，每行 nNets 个。 */
    MstaClockArr *pClockArr;   /* [nClocks * nNets] 每个时钟在每根时钟网络上的到达 */
    char   *pfClockOfNet;      /* [nClocks * nNets] 该时钟树里有没有这根网络 */
    int     fIdealClocks;      /* 1 = 不做时钟传播，所有 FF 时钟脚都当 0 */
    /* set_ideal_network：显式标过的网络 + 沿组合扇出传下来的相同属性。
       理想网络不累计延迟（模型说明见 PropagateClocks 上方）。 */
    char   *pfIdealNet;        /* [nNets] */

    /* 数据到达 */
    MstaCorner CornerMax, CornerMin;
    /* "找次优路径"用的排除表 */
    MstaPathExclude vPathExclude[MSTA_MAX_PATH_EXCLUDE];
    int     nPathExclude;
    MstaClockPair vClockPairs[MSTA_MAX_CLOCK_PAIRS];
    int     nClockPairs;

    int    *pTopoOrder;        /* [nNets] 拓扑序：网络编号的数组 */
    int     nTopoOrder;
    int    *pState;            /* DFS 用的 0/1/2 标记 */
    int    *pStartClass;
    int     nCombLoops;

    /* 统计 */
    int     nRegisters;
    int     nLatches;         /* 端点里有几个是锁存器 */
    int     nEndpoints;
    int     nUnconstrainedEnds;
    /* 设计规则检查（只有 SDC 里设了限制才有意义） */
    int     nDrcTransitionViol, nDrcCapacitanceViol, nDrcFanoutViol;
    int     nDrcMinCapacitanceViol;
    double  WorstDrcTransition, WorstDrcCapacitance;   /* 实际值 - 限值，正数=违例 */
    double  WorstDrcMinCapacitance;                    /* 限值 - 实际值，正数=违例 */
    int     WorstDrcFanout;
    double  DesignArea;                                /* 所有实例的面积之和 */
    double  WorstSetupSlack, WorstHoldSlack;
    double  TotalSetupSlack, TotalHoldSlack;
    int     nSetupViolations, nHoldViolations;
    int     nExcludedEnds;
    /* set_data_check 的结果：两条数据路径之间的检查，单独统计并计入 WNS/TNS。 */
    int     nDataChecks, nDataCheckSetupViol, nDataCheckHoldViol;
    double  WorstDataCheckSetupSlack, WorstDataCheckHoldSlack;
    MstaId  DataCheckSetupFrom, DataCheckSetupTo, DataCheckHoldFrom, DataCheckHoldTo;
    double  DataCheckSetupValue, DataCheckHoldValue;
    /* set_clock_gating_check 的结果：门控单元使能脚相对时钟沿的检查。 */
    int     nClkGatingChecks;
    double  WorstClkGatingSetupSlack, WorstClkGatingHoldSlack;
    int     ClkGatingSetupInst, ClkGatingHoldInst;      /* 最差那条的实例号 */
    MstaId  ClkGatingSetupPin, ClkGatingHoldPin;        /* 使能脚名 */
    double  ClkGatingSetupValue, ClkGatingHoldValue;
} MstaTiming;

MstaTiming *Msta_TimingStart( MstaDesign *pDes, MstaLib *pLib, MstaSdc *pSdc );
void        Msta_TimingFree( MstaTiming *p );

/* 完整一遍分析：建端点 -> 时钟传播 -> 拓扑序 -> 前向到达 -> 检查。 */
int         Msta_TimingAnalyze( MstaTiming *p, int fVerbose );

/* ---- 查询：报告层只用这些 ---- */
/* 把端点按 slack 从小到大排（setup 或 hold 两种排法）。 */
void        Msta_TimingSortChecks( MstaTiming *p, int fSetup );
/* 起点(fStart=1)/终点的可打印名字：FF 实例名或顶层端口名。
   fMax 选择最大或最小路径回溯，因为两条路径可能有不同的起点。 */
const char *Msta_TimingEndpointName( MstaTiming *p, MstaCheck *pCheck,
                                     int fStart, int fMax );
/* 回溯一条路径：把从终点到起点的网络号按顺序填进 pnNets，返回个数。 */
int         Msta_TimingTracePath( MstaTiming *p, MstaCheck *pCheck, int fMax,
                                  int *pnNets, int nCap );
MstaPinRef *Msta_TimingNetDriver( MstaTiming *p, int nNet );     /* 无驱动返回 NULL */
double      Msta_TimingNetArrival( MstaTiming *p, int nNet, int fMax );
int         Msta_TimingClockNetCount( MstaTiming *p );
/* 时钟树按时钟分别传播：报告层要按 (时钟, 网络) 取到达与归属。 */
int         Msta_TimingClockCount( MstaTiming *p );
int         Msta_TimingNetHasClock( MstaTiming *p, int nClock, int nNet );
const MstaClockArr *Msta_TimingClockArr( MstaTiming *p, int nClock, int nNet );

#endif /* MSTA_TIMING_H */
