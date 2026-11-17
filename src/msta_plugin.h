/**CHeader*************************************************************

  FileName    [msta_plugin.h]

  Synopsis    [C99 静态虚函数表插件接口：延迟计算、互连 RC、偏差模型与路径精修。]

  ---------------------------------------------------------------------
  设计目标
  ---------------------------------------------------------------------
  1. 保持零外部依赖（C99 + libm，无需 dlopen / Python / Tcl ABI 负担）：
     教学、科研与自定义实验只需填写下列 4 组函数表之一，通过
     Msta_TimingSetPlugins() 挂进引擎即可替换对应阶段算法；
     表内允许部分覆写，留 NULL 的回调函数指针会自动回退至内置默认实现。
  2. 默认插件零开销，保证 212 条既有回归与 OpenSTA/OpenTimer 对比零漂移：
     - 延迟插件   ：nldm（双线性 NLDM 查表）
     - 线网插件   ：zero_rc（总电容 = 引脚 + set_load，零线延迟）
     - 偏差插件   ：ocv_aocv_cppr（OCV derate + AOCV 深度表 + CPPR）
     - 路径精修   ：none（GBA 结论直接汇报）
  3. 内置两个教学示范插件，可在脚本里一键切换并观察量化变化：
     - elmore_rc  ：按扇出下标区分树状阶梯 Elmore RC 延迟与 Bakoglu 摆率退化
     - pba_retrace：按真实选中路径的逐级摆率与真实路径 AOCV 深度重算端点检查

***********************************************************************/

#ifndef MSTA_PLUGIN_H
#define MSTA_PLUGIN_H

#include "msta_types.h"
#include "msta_lib.h"
#include "msta_net.h"
#include "msta_sdc.h"

#define MSTA_PLUGIN_ABI_VERSION 1

struct MstaTiming;
struct MstaCheck;

/* =====================================================================
   1. 单元延迟插件接口（DelayOps）
   ===================================================================== */

typedef struct {
    struct MstaTiming *pTiming;
    void              *pUserData;  /* 由引擎自动填入当前 MstaDelayOps->pUserData */
    int                nInst;
    int                nInNet;
    int                nOutNet;
    int                nClock;     /* 时钟树传播时有效，数据路径传 -1 */
    const MstaArc     *pArc;       /* 已按 corner 映射好的时序弧 */
    double             InSlew;     /* ps，经线网插件到达输入引脚后的真实摆率 */
    double             EffCap;     /* fF，由线网插件算得的有效负载电容 */
    int                fMax;       /* 1 = max corner, 0 = min corner */
    int                fOutRise;   /* 1 = output rise, 0 = output fall */
    int                fInRise;    /* 1 = input rise, 0 = input fall */
    int                fClockPath; /* 1 = 时钟路径传播 */
} MstaDelayQuery;

typedef struct {
    double Delay;                  /* ps，未乘 OCV/AOCV 系数的原始单元弧延迟 */
    double OutSlew;                /* ps，单元输出驱动端摆率 */
} MstaDelayResult;

/* 回调函数任一成员留 NULL 时自动回退到 nldm 默认实现。 */
typedef struct {
    int         AbiVersion;
    const char *pName;
    void       *pUserData;
    void   (*EvalArc)( const MstaDelayQuery *pQ, MstaDelayResult *pOut );
    double (*EvalConstraint)( const struct MstaTiming *pT, const MstaArc *pArc,
                              int nInst, int fMax, int fDataRise,
                              double ClkSlew, double DataSlew );
} MstaDelayOps;

/* =====================================================================
   2. 互连线模型插件接口（WireOps，区分每一个扇出 WireArc）
   ===================================================================== */

typedef struct {
    struct MstaTiming *pTiming;
    void              *pUserData;  /* 由引擎自动填入当前 MstaWireOps->pUserData */
    int                nNet;       /* 驱动网络下标 */
    int                nLoadInst;  /* 接收扇出的实例下标（-1 表示主输出/仅查询电容） */
    int                nLoadPin;   /* 接收扇出的单元引脚下标（-1 表示主输出/未知） */
    int                nLoadIdx;   /* 在 nNet->vLoads 里的下标（-1 表示非列表内负载） */
    int                fMax;       /* 1 = max corner, 0 = min corner */
    int                fRise;      /* 信号边沿：1 = rise, 0 = fall */
    int                fClockPath; /* 1 = 时钟树传播 */
    double             DriverSlew; /* ps，驱动单元输出端摆率 */
} MstaWireQuery;

typedef struct {
    double WireDelay;              /* ps，从 nNet 驱动端到具体扇出脚 (nLoadInst, nLoadPin) 的延迟 */
    double SinkSlew;               /* ps，信号抵达该扇出脚时的摆率 */
} MstaWireResult;

/* 回调函数留 NULL 时自动回退到 zero_rc 默认实现。 */
typedef struct {
    int         AbiVersion;
    const char *pName;
    void       *pUserData;
    /* 返回不含兜底负载的原始有效总电容（fF）：引脚电容 + 线网电容 + SDC set_load。 */
    double (*NetEffCap)( const MstaWireQuery *pQ );
    /* 评估单条扇出 WireArc：Driver -> (nLoadInst, nLoadPin)。 */
    void   (*EvalWireArc)( const MstaWireQuery *pQ, MstaWireResult *pOut );
} MstaWireOps;

/* 内置 Elmore 梯形 RC 演示模型的可调参数（1 kOhm * 1 fF = 1 ps）。 */
typedef struct {
    double RPerFanout;             /* kOhm，每个扇出段的串联电阻 */
    double CPerFanout;             /* fF，每个扇出段的对地线电容 */
} MstaElmoreConfig;

/* =====================================================================
   3. 工艺偏差 / 不确定性 / CPPR 插件接口（VariationOps）
   ===================================================================== */

/* 任一回调留 NULL 时自动回退到 ocv_aocv_cppr 默认实现。 */
typedef struct {
    int         AbiVersion;
    const char *pName;
    void       *pUserData;
    double (*ArcScale)( const struct MstaTiming *pT, int nInst, int nClock,
                        int nInNet, int nOutNet, int fMax, int fInRise, int fClockPath );
    double (*CheckScale)( const struct MstaTiming *pT, int nInst, int fMax, int fDataRise );
    double (*ClockUncertainty)( const struct MstaTiming *pT, int nLaunchClock,
                                int nCaptureClock, int fSetup,
                                int fLaunchRises, int fCaptureRises );
    double (*CpprCredit)( const struct MstaTiming *pT, int nStart, int nLaunchClock,
                          int nCaptureClock, int nCaptureNet, int fSetup,
                          int nLaunchSource, int nCaptureLocal, int nCaptureSource );
} MstaVariationOps;

/* =====================================================================
   4. 路径精修插件接口（PathRefinerOps）
   ===================================================================== */

/* RefineCheck 留 NULL 等同于 none（直接汇报 GBA 结果）。 */
typedef struct {
    int         AbiVersion;
    const char *pName;
    void       *pUserData;
    void (*RefineCheck)( struct MstaTiming *pT, struct MstaCheck *pCheck, int fSetup );
} MstaPathRefinerOps;

/* =====================================================================
   插件集合容器与内置实现获取接口
   ===================================================================== */

typedef struct {
    const MstaDelayOps       *pDelay;
    const MstaWireOps        *pWire;
    const MstaVariationOps   *pVariation;
    const MstaPathRefinerOps *pRefiner;
} MstaPluginSet;

/* 默认标准实现（零回归基线） */
const MstaDelayOps       *Msta_PluginDelayNldm( void );
const MstaWireOps        *Msta_PluginWireZeroRc( void );
const MstaVariationOps   *Msta_PluginVariationDefault( void );
const MstaPathRefinerOps *Msta_PluginRefinerNone( void );

/* 内置教学/科研演示插件 */
const MstaWireOps        *Msta_PluginDefaultElmoreWire( void );
void                      Msta_PluginInitElmoreWire( MstaWireOps *pOps,
                                                     MstaElmoreConfig *pCfg,
                                                     double RPerFanoutKOhm,
                                                     double CPerFanoutFf );
const MstaPathRefinerOps *Msta_PluginRefinerPbaRetrace( void );

/* 将 pSet 初始化为全部内置默认插件。 */
void Msta_PluginSetInitDefaults( MstaPluginSet *pSet );

#endif /* MSTA_PLUGIN_H */
