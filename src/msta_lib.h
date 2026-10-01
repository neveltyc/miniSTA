/**CHeader*************************************************************

  FileName    [msta_lib.h]

  Synopsis    [标准单元库（Liberty）的语义模型：cell / pin / 时序弧 / NLDM 表。]

  ---------------------------------------------------------------------
  Liberty 数据模型
  ---------------------------------------------------------------------
  本模块读取以下信息：

    1) 引脚   pin: 方向、输入电容（驱动下一级时要算负载）、是否时钟脚
    2) 组合弧 timing(): related_pin(输入) -> 本脚(输出), cell_rise/cell_fall 延迟表,
                       rise_transition/fall_transition 输出摆率表
    3) 检查弧 timing(): timing_type = setup_rising/hold_rising 时, 表里给的不是延迟,
                       而是 "数据必须比时钟沿早/晚多少" —— 即 setup/hold 时间
    4) 时序单元 ff()/latch(): 时钟、数据和异步控制脚；异步控制脚参与
                       recovery/removal 检查。

  单位：本层是单位的 "入口"。读入时立刻把 ns/pf 等换算成全局的 ps/fF，
  此后所有模块只看 ps/fF，不再关心原始单位。换算规则见 msta_lib.c 的
  Msta_LibReadLibraryAttrs / Msta_LibTimeScaleOf / Msta_LibCapScaleOf。

***********************************************************************/

#ifndef MSTA_LIB_H
#define MSTA_LIB_H

#include "msta_util.h"

/* ---------------------------------------------------------------------
   枚举
   --------------------------------------------------------------------- */

typedef enum {
    MSTA_DIR_NO = 0,
    MSTA_DIR_INPUT,
    MSTA_DIR_OUTPUT,
    MSTA_DIR_INOUT,
    MSTA_DIR_INTERNAL      /* 单元内部节点，网表里连不到 */
} MstaPinDir;

typedef enum {
    MSTA_SENSE_UNKNOWN = 0,
    MSTA_SENSE_POSITIVE,   /* timing_sense : positive_unate */
    MSTA_SENSE_NEGATIVE,   /* 反相：输出翻转方向与输入相反 */
    MSTA_SENSE_NONUNATE    /* 非单态（如 XOR）：两种都可能 */
} MstaSense;

/* Liberty timing_type 里 STA 会用到的那些值。没列出来的都当成 UNKNOWN 并跳过。 */
typedef enum {
    MSTA_TT_UNKNOWN = 0,
    MSTA_TT_COMBINATIONAL,    /* 省略 timing_type 时的默认值 */
    MSTA_TT_RISE_EDGE,        /* 时钟上升沿 -> Q 有值   (clk2q) */
    MSTA_TT_FALL_EDGE,        /* 时钟下降沿 -> Q 有值 */
    MSTA_TT_RISE_BOTH,        /* 上升/下降沿都有值 */
    MSTA_TT_FALL_BOTH,
    MSTA_TT_SETUP_RISING,     /* 相对时钟上升沿的 setup */
    MSTA_TT_SETUP_FALLING,
    MSTA_TT_HOLD_RISING,
    MSTA_TT_HOLD_FALLING,
    MSTA_TT_CLEAR,            /* 异步复位脚 -> 输出的延迟弧；触发器上不作为组合弧传播 */
    MSTA_TT_PRESET,
    MSTA_TT_RECOVERY_RISING,  /* 复位释放相对时钟沿的恢复时间 */
    MSTA_TT_RECOVERY_FALLING,
    MSTA_TT_REMOVAL_RISING,
    MSTA_TT_REMOVAL_FALLING
} MstaTimingType;

const char *Msta_TimingTypeName( MstaTimingType Type );
MstaTimingType Msta_TimingTypeFromName( const char *pName );

/* ---------------------------------------------------------------------
   NLDM 查表
   --------------------------------------------------------------------- */
/* Liberty 的 table_lookup 模型：二维表，默认行(index_1)是输入摆率、列(index_2)是
   输出负载；实际含义由模板的 variable_1/2 决定，记在下面四个标志里。
   查表 = 双线性插值 + 越界外插。抄自 ABC sclLib.h:Scl_LibLookup 的算法。 */
typedef struct {
    int     nRows;       /* index_1 的长度；0 表示这张表不存在 */
    int     nCols;       /* index_2 的长度 */
    int     fRowIsLoad;  /* index_1 是负载轴 */
    int     fColIsSlew;  /* index_2 是摆率轴（约束表和 load/slew delay 表） */
    int     fRowUsesSecond; /* index_1 取 Msta_TableLookup 的第二个参数 Load，否则取 Slew */
    int     fColUsesFirst;  /* index_2 取第一个参数 Slew，否则取 Load */
    double *pRowIndex;   /* index_1：默认输入摆率(ps)，fRowIsLoad 时为负载(fF)；单调递增 */
    double *pColIndex;   /* index_2：默认输出负载(fF)，fColIsSlew 时为摆率(ps)；单调递增 */
    double *pValues;     /* nRows * nCols, 行优先 */
    char    sTag[32];    /* 表名，如 "delay_template_7x7"，只用于打印 */
} MstaTable;

void   Msta_TableFree( MstaTable *p );
int    Msta_TableExists( const MstaTable *p );
/* 给定输入摆率和输出负载，插值出一个值。表不存在时返回 0。
   约束表借用这两个参数传 (时钟脚摆率, 数据脚摆率)。 */
double Msta_TableLookup( const MstaTable *p, double Slew, double Load );

/* ---------------------------------------------------------------------
   时序弧
   --------------------------------------------------------------------- */
typedef struct {
    MstaId        InPin;      /* 起点引脚名 id；对 setup/hold 弧这是 related_pin(时钟) */
    MstaId        OutPin;     /* 终点引脚名 id；setup/hold 弧这是被检查的数据脚 */
    MstaTimingType Type;
    MstaSense     Sense;
    /* 组合/clk2q 弧用 delay 表；setup/hold 弧用 Constraint 表。
       同一个 timing() 组里两类不会同时出现，所以放在一起不冲突。 */
    MstaTable DelayRise;      /* cell_rise      */
    MstaTable DelayFall;      /* cell_fall      */
    MstaTable TransRise;      /* rise_transition */
    MstaTable TransFall;      /* fall_transition */
    MstaTable ConstraintRise; /* setup/hold 的 rise_constraint */
    MstaTable ConstraintFall; /* setup/hold 的 fall_constraint */
    double    MaxSlewLimit;   /* 本弧输入脚上的 max_transition，-1 表示没写 */
} MstaArc;

/* ---------------------------------------------------------------------
   引脚
   --------------------------------------------------------------------- */
typedef struct {
    MstaId      Name;         /* 名字表里的 id */
    MstaPinDir  Dir;
    double      Cap;          /* 输入电容, fF（负载模型的核心：驱动它就要花时间） */
    double      MaxCap;       /* max_capacitance, -1 表示未写 */
    double      MinCap;       /* min_capacitance, -1 表示未写 */
    double      MaxSlew;      /* max_transition,  -1 表示未写 */
    int         fClock;       /* pin 里 clock : "true" */
    int         fGateClock;   /* clock_gate_clock_pin：时钟门控单元的时钟脚 */
    int         fGateEnable;  /* clock_gate_enable_pin：门控使能脚 */
    int         fGateOut;     /* clock_gate_out_pin：门控后的时钟输出脚 */
    char       *pFunc;        /* function 原文，用于时钟 case 裁剪与报告 */
} MstaPin;

/* ---------------------------------------------------------------------
   寄存器检查项：一个 (时钟脚, 数据脚) 组合一条，是 STA 引擎的 "终点定义"
   --------------------------------------------------------------------- */
typedef struct {
    MstaId ClkPin;            /* 时钟脚 */
    MstaId DataPin;           /* 数据脚（D / SCD / RESET_B ...） */
    MstaId QPin;              /* 由哪个输出脚提供 clk2q */
    int    fClkRises;         /* 1: 上升沿触发(rising_edge/setup_rising) 0: 下降沿 */
    MstaId SetupArc;          /* setup  弧在 vArcs 里的下标, MSTA_NO_ID 表示没有 */
    MstaId HoldArc;           /* hold   弧下标 */
    MstaId ClkToQArc;         /* clk2q  弧下标 */
    MstaId SetupFallArc;      /* 同一个 FF 若同时有 setup_falling 也记下来 */
    MstaId HoldFallArc;
} MstaRegCheck;

typedef struct {
    MstaId ClkPin;
    MstaId AsyncPin;
    MstaId RecoveryArc;
    MstaId RecoveryFallArc;
    MstaId RemovalArc;
    MstaId RemovalFallArc;
    int    fClkRises;
} MstaAsyncCheck;

/* 时钟门控单元（ICG）的使能检查：使能脚相对时钟脚的 setup/hold。
   库里用 clock_gate_enable_pin / clock_gate_clock_pin 标出这两个脚，
   约束值放在使能脚的 setup_rising / hold_rising 弧里。 */
typedef struct {
    MstaId ClkPin;            /* clock_gate_clock_pin */
    MstaId EnablePin;         /* clock_gate_enable_pin */
    int    fClkRises;         /* 1 = 相对时钟上升沿（setup_rising 弧） */
    MstaId SetupArc;          /* 使能脚的 setup 弧下标，MSTA_NO_ID 表示库里没写 */
    MstaId HoldArc;
} MstaGateCheck;

/* ---------------------------------------------------------------------
   数组容器（先成型，下面的结构体成员才用得上）
   --------------------------------------------------------------------- */
MstaArrayDefine( MstaPin, MstaPinArray )
MstaArrayDefine( MstaArc, MstaArcArray )
MstaArrayDefine( MstaRegCheck, MstaRegCheckArray )
MstaArrayDefine( MstaAsyncCheck, MstaAsyncCheckArray )
MstaArrayDefine( MstaGateCheck, MstaGateCheckArray )

/* ---------------------------------------------------------------------
   单元
   --------------------------------------------------------------------- */
typedef struct {
    MstaId Name;              /* cell 名 id */
    MstaId LibName;           /* 来自哪个 library(...)：多库时区分同名 cell */
    char  *pLibName;          /* 打印用名字 */
    double Area;
    double Leakage;
    int    fSequential;       /* 有 ff() 组 —— 只有它才产生 setup/hold 终点 */
    int    fLatch;            /* 有 latch() 组 —— 典型是 ICG 时钟门控，按透明缓冲处理 */
    int    fBlackBox;         /* 库里查不到、只有引脚方向的占位单元 */
    int    fIgnore;           /* 电源脚等不参与时序的单元 */
    MstaPinArray  vPins;
    MstaArcArray  vArcs;
    MstaRegCheckArray vRegs;
    MstaAsyncCheckArray vAsync;
    MstaGateCheckArray vGates;    /* 库里有 clock_gate_enable_pin 才有 */
    /* 单元引脚查询使用线性扫描。 */
} MstaCell;
MstaArrayDefine( MstaCell, MstaCellArray )

/* ---------------------------------------------------------------------
   库
   --------------------------------------------------------------------- */
/* Liberty 的 operating_conditions(...) 组：工艺角的名字与电压/温度/工艺。 */
typedef struct {
    MstaId Name;
    double Voltage, Temperature, Process;   /* -1 表示这个组里没写 */
    /* Liberty 的 K 因子：延迟缩放用（没有就保持 -1）。 */
    double KVolt, KTemp, KProcess;
} MstaOpCond;
MstaArrayDefine( MstaOpCond, MstaOpCondArray )

typedef struct {
    MstaId          Name;         /* library("...") 的名字 */
    char           *pFileName;
    int             nCells;       /* 这个库贡献了多少 cell */
    MstaId          OpCondName;   /* default_operating_conditions，MSTA_NO_ID = 没写 */
    double          NomVoltage, NomTemperature, NomProcess;  /* -1 = 没写 */
    MstaOpCondArray vOpConds;     /* 这个库里声明的所有 operating_conditions */
} MstaLibInfo;
MstaArrayDefine( MstaLibInfo, MstaLibInfoArray )

typedef struct {
    MstaCellArray vCells;
    MstaCellArray vCornerCells; /* 同名单元在其他 library 里的 corner variant */
    char         *pLibName;       /* 第一个库的名字：SDC 的单位按它定 */
    char         *pFileName;
    MstaLibInfoArray vLibs;       /* 读进来的所有库 */
    MstaId         CurLibName;    /* 正在解析哪个库（供 Msta_LibReadCell 用） */
    int            nCornerVariants; /* 从其他 library 读入的同名单元变体 */
    /* 单位换算：读入时把 Liberty 的原始单位乘上这两个系数，之后全用 ps/fF。 */
    double        TimeScale;      /* ns -> ps 通常是 1000 */
    double        CapScale;       /* ff -> fF 是 1, pf -> fF 是 1000 */
    double        DefaultMaxSlew; /* default_max_transition */
    double        DefaultCap;     /* default_input_pin_cap */
    double        CurTimeScale, CurCapScale; /* 当前解析 library 的单位换算 */
    double        CurDefaultMaxSlew, CurDefaultCap;
    int           nCellsRead;
    int           nCellsSkipped;
    /* 名字 id -> cell 下标 的散列。用开放寻址，省一个 map 结构。 */
    int          *pCellHash;
    int           nCellHash;
} MstaLib;

MstaLib *Msta_LibStart( void );
/* 按名字（可再用库名限定）找一个 operating condition；返回它所在的库信息。 */
MstaLibInfo *Msta_LibFindOpCond( MstaLib *pLib, const char *pName, MstaId LibraryName,
                                 MstaOpCond **ppCond );
void     Msta_LibFree( MstaLib *p );
/* 解析一个 Liberty 文件并把里面的 cell 合并进库。返回 0 表示失败。 */
int      Msta_LibRead( MstaLib *p, const char *pFileName, int fVerbose );
/* cell 名 -> 下标 的散列在追加新库后要重建。 */
int      Msta_LibRehashCells( MstaLib *p );

MstaCell *Msta_LibFindCell( MstaLib *p, const char *pCellName );
/* 选角时取指定库里的同名 cell；未提供或该库没有此 cell 时回退默认定义。 */
MstaCell *Msta_LibCellForCorner( MstaLib *p, MstaCell *pCell, MstaId LibraryName );
MstaArc  *Msta_LibArcForCorner( MstaLib *p, MstaCell *pCell, MstaId ArcId,
                                 MstaId LibraryName );
MstaPin  *Msta_LibPinForCorner( MstaLib *p, MstaCell *pCell, MstaId PinId,
                                 MstaId LibraryName );

/* ---- 给时序引擎用的查询 ---- */
int       Msta_CellPinIndexOf( MstaCell *p, MstaId NameId );     /* -1 表示没有这个脚 */
/* 时钟弧按侧输入 case 值裁剪；-1=无翻转，其他为 MstaSense。pCases: -1/0/1。 */
int       Msta_LibClockSense( MstaCell *pCell, const MstaArc *pArc, const signed char *pCases );
/* 组合弧：输入脚 InPin 翻转到输出脚 OutPin。找不到返回 NULL。 */
MstaArc  *Msta_CellCombArc( MstaCell *p, MstaId InPin, MstaId OutPin );
MstaArc  *Msta_CellArcById( MstaCell *p, MstaId ArcId );
/* 库统计信息，report_lib 用 */
void      Msta_LibPrintStats( MstaLib *p, FILE *pFile );
/* 打印一个 cell 的全部时序信息，用来和 Liberty 原文对答案。 */
void      Msta_LibPrintCell( MstaLib *p, FILE *pFile, const char *pCellName );

#endif /* MSTA_LIB_H */
