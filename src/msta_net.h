/**CHeader*************************************************************

  FileName    [msta_net.h]

  Synopsis    [网表层：yosys 前端 + 层次展平，产出 STA 引擎要用的扁平网表。]

  ---------------------------------------------------------------------
  Verilog 由 yosys 读，msta 读 JSON
  ---------------------------------------------------------------------
  门级 Verilog 的语法细节很多：转义标识符 \\name、总线 [7:0] 与 A[3] 两种写法、
  assign 别名、拼接、参数化实例、generate 展开。这些交给 yosys 的前端，它用
  `write_json` 导出的中间格式字段很少。分工：
      yosys:  Verilog -> JSON
      msta:   JSON -> 时序图 -> STA
  `read_json` 命令可直接读取 Yosys 的 JSON。

  ---------------------------------------------------------------------
  展平（flatten）
  ---------------------------------------------------------------------
  JSON 里每个模块自己给网络编号（0/1 是常量，>=2 是真网络），编号只在模块内
  有效。展平就是给"每个模块实例"的每个局部网络分配一个全局编号，并把层次路径
  拼进网络名，于是报告里能直接看到 u_core/u_alu/n42 这样的名字。
  assign 别名不需要 msta 处理：yosys 已经把两端合成同一个 bit 了。

***********************************************************************/

#ifndef MSTA_NET_H
#define MSTA_NET_H

#include "msta_util.h"
#include "msta_lib.h"

/* yosys JSON 里一个 bit 的表示：>=0 是模块内的网络编号，负数表示常量。 */
#define MSTA_BIT_CONST0   (-1)
#define MSTA_BIT_CONST1   (-2)
#define MSTA_BIT_BAD      (-3)

typedef enum {
    MSTA_PORT_NO = 0,
    MSTA_PORT_IN,
    MSTA_PORT_OUT,
    MSTA_PORT_INOUT
} MstaPortDir;

MstaArrayDefine( MstaId, MstaIdArray )

/* ============ JSON 里读出来的形状（按模块存） ============ */

typedef struct {
    MstaId      Name;       /* 端口名 */
    int         Dir;        /* MstaPortDir */
    MstaIdArray Bits;       /* 端口的每一位 -> 该模块的局部网络号（低位在前） */
} MstaModPort;

/* 一个实例的一次引脚连接。Pin == MSTA_NO_ID 表示 yosys 没给出引脚名（不会发生）。 */
typedef struct {
    MstaId      Pin;
    MstaIdArray Bits;       /* 引脚每一位 -> 局部网络号或常量 */
} MstaModConn;

typedef struct {
    MstaId Name;            /* 实例名 */
    MstaId Type;            /* 单元名 或 子模块名 */
    int    ConnFirst;
    int    ConnCount;
} MstaModCell;

/* 上面三个类型各自的数组，MstaModule 里要用，所以必须先成型。 */
MstaArrayDefine( MstaModPort, MstaModPortArray )
MstaArrayDefine( MstaModConn, MstaModConnArray )
MstaArrayDefine( MstaModCell, MstaModCellArray )

typedef struct {
    MstaId          Name;
    int             fInstantiated;   /* 被别的模块例化过 -> 不是顶层 */
    int             nBits;           /* 本模块用过的最大网络号 + 1 */
    MstaModPortArray vPorts;
    MstaModCellArray vCells;
    MstaModConnArray vConns;
    MstaIdArray      vBitNames;      /* 局部网络号 -> 显示名（取 netnames 里第一个） */
} MstaModule;

MstaArrayDefine( MstaModule, MstaModuleArray )
MstaArrayDefine( MstaIdArray, MstaIdArrayArray )

/* ============ 展平后的形状：时序引擎唯一会读的东西 ============ */

typedef struct {
    MstaId InstId;        /* MstaDesign.vInsts 下标 */
    MstaId PinId;         /* pCell->vPins 下标 */
} MstaPinRef;
MstaArrayDefine( MstaPinRef, MstaPinRefArray )

typedef struct {
    MstaId          Name;     /* 含层次路径的网络名 */
    int             fConst;   /* 1=常量0, 2=常量1 */
    int             fCaseValue; /* 0=未设置，1=逻辑0，2=逻辑1 */
    int             fTopPort;
    int             Dir;
    MstaPinRef      Driver;   /* InstId == MSTA_NO_ID 表示没有驱动（输入脚/悬空） */
    MstaPinRefArray vLoads;
} MstaNet;
MstaArrayDefine( MstaNet, MstaNetArray )

typedef struct {
    MstaId     Name;        /* 含层次路径的实例名 */
    MstaId     ModuleName;
    MstaCell  *pCell;       /* 库里的单元；库里没有则是 msta 造的黑盒占位单元 */
    MstaId    *pNets;       /* 与 pCell->vPins 对齐：每个脚连到的全局网络号 */
    int        nPins;
} MstaInst;
MstaArrayDefine( MstaInst, MstaInstArray )
MstaArrayDefine( MstaCell, MstaCellArray2 )

/* 结构体带上 tag 名，别的模块（如 msta_cmds.h）可以先只声明指针。 */
typedef struct MstaDesign {
    /* 模块表（来自 JSON） */
    MstaModuleArray vModules;
    MstaIntMap      modMap;         /* 模块名 nameId -> vModules 下标 */

    /* 展平结果 */
    MstaId          TopName;
    MstaNetArray    vNets;
    MstaIntMap      netNameMap;     /* 网络名 nameId -> vNets 下标，SDC 解析目标时用 */
    MstaInstArray   vInsts;
    int             nConst0Net;     /* 两个常量网络的全局编号 */
    int             nConst1Net;

    /* 库里查不到的单元：造占位 cell（不塞进 pLib，免得污染库统计） */
    MstaCellArray2  vBlackBoxCells;
    MstaIntMap      blackBoxMap;
    int             nBlackBoxes;
    int             nFlattenedModules;   /* 模块实例数，用于说明展平规模 */
} MstaDesign;

/* ---- 读入 ---- */
MstaDesign *Msta_DesignStart( void );
void        Msta_DesignFree( MstaDesign *p );

/* 读 yosys 的 write_json 产物。 */
int         Msta_DesignReadYosysJson( MstaDesign *p, const char *pJsonFile, int fVerbose );

/* 调用 yosys 把 Verilog 转成 JSON 再读进来。pVerilogFiles 是以 NULL 结尾的数组。
   pWorkDir 为中间 JSON 与 yosys 日志所在的目录。 */
int         Msta_DesignReadVerilog( MstaDesign *p, const char **ppVerilogFiles, int nFiles,
                                    const char *pWorkDir, int fVerbose );

/* ---- 展平与查询 ---- */
int         Msta_DesignFlatten( MstaDesign *p, MstaLib *pLib, const char *pTopName, int fVerbose );
const char *Msta_DesignGuessTop( MstaDesign *p );
const char *Msta_NetName( MstaDesign *p, MstaId nNet );
const char *Msta_InstName( MstaDesign *p, MstaId nInst );
/* 网络名可能带层次路径，例如 "u_core/u_alu/n42"；也接受顶层端口名。 */
int         Msta_DesignNetByName( MstaDesign *p, const char *pNetName );
/* 实例的层次路径名 -> 实例下标。线性查找，只给 SDC 用，不在热路径上。 */
int         Msta_DesignFindInstByName( MstaDesign *p, const char *pInstName );
/* 某实例某个脚连到的网络号；引脚名如 "D" / "Q"。找不到返回 -1。 */
int         Msta_DesignInstPinNet( MstaDesign *p, int nInst, const char *pPinName );
void        Msta_DesignPrintStats( MstaDesign *p, FILE *pFile );

#endif /* MSTA_NET_H */
