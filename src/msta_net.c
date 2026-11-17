/**CFile***************************************************************

  FileName    [msta_net.c]

  Synopsis    [yosys JSON 前端 + 层次展平。]

  分工：
    读入（Msta_DesignReadYosysJson）只把 JSON 的形状搬进 MstaModule，不做化简；
    展平（Msta_DesignFlatten）给"每个模块实例的每个局部网络"分配全局编号，
    并把单元实例落到一张扁平表上，供时序引擎使用。

***********************************************************************/

#include <unistd.h>
#include "msta_net.h"
#include "msta_json.h"
#include "msta_util.h"

/* ---------------------------------------------------------------------
   展平过程的状态（先定义，下面的内部接口才用得到）
   --------------------------------------------------------------------- */
typedef struct {
    MstaDesign *pDes;
    MstaLib    *pLib;
    char       *pPath;          /* 当前层次路径，如 "u_core/u_alu" */
    int         nPathLen;
    int         nPathCap;
    int         nDepth;
    int         fError;
} MstaFlatten;

#define MSTA_MAX_HIER_DEPTH 256

/* ---------------------------------------------------------------------
   本文件内部的提前声明（自上而下阅读时先看到接口）
   --------------------------------------------------------------------- */
static MstaModPort *Msta_ModulePortByName( MstaModule *pMod, MstaId PinName );
static int          Msta_GlobalNetForBit( MstaFlatten *pF, MstaModule *pMod,
                                          const int *pMap, int nBit );
static int          Msta_BlackBoxCell( MstaFlatten *pF, MstaModule *pMod, MstaModCell *pCell );
static void         Msta_FlattenModule( MstaFlatten *pF, int nModId, const int *pPortMap );
static int          Msta_NewNet( MstaFlatten *pF, MstaId Name, int fConst );

/* =====================================================================
   1. 读 yosys JSON
   ===================================================================== */

/* JSON 里的一个 bit：整数是模块内的局部网络号；"0"/"1" 是常量。 */
static int Msta_BitFromJson( MJsonValue *pValue )
{
    if ( pValue == NULL )
        return MSTA_BIT_BAD;
    if ( pValue->Kind == MJSON_NUMBER )
        return (int)pValue->Num;
    if ( pValue->Kind == MJSON_STRING )
    {
        if ( !strcmp( pValue->pStr, "0" ) ) return MSTA_BIT_CONST0;
        if ( !strcmp( pValue->pStr, "1" ) ) return MSTA_BIT_CONST1;
        if ( !strcmp( pValue->pStr, "x" ) || !strcmp( pValue->pStr, "z" ) )
        {
            Msta_WarnOnce( "有引脚接到 1'bx/1'bz；msta 按常数 0 处理" );
            return MSTA_BIT_CONST0;
        }
    }
    return MSTA_BIT_BAD;
}

static void Msta_ReadBits( MJsonValue *pArray, MstaIdArray *pOut, int *pnMaxBit )
{
    int i;
    if ( !Msta_JsonIsArray( pArray ) )
        return;
    for ( i = 0; i < Msta_JsonCount(pArray); i++ )
    {
        int nBit = Msta_BitFromJson( Msta_JsonAt(pArray, i) );
        if ( nBit == MSTA_BIT_BAD )
        {
            Msta_WarnOnce( "yosys json 里有不支持的位值（按常数 0 处理）" );
            nBit = MSTA_BIT_CONST0;
        }
        *MstaIdArrayAppend( pOut ) = nBit;
        if ( nBit >= *pnMaxBit )
            *pnMaxBit = nBit + 1;
    }
}

static void Msta_ModuleClear( MstaModule *pMod )
{
    int k;
    for ( k = 0; k < pMod->vPorts.nSize; k++ )
        MstaIdArrayFree( &pMod->vPorts.pData[k].Bits );
    MstaModPortArrayFree( &pMod->vPorts );
    for ( k = 0; k < pMod->vConns.nSize; k++ )
        MstaIdArrayFree( &pMod->vConns.pData[k].Bits );
    MstaModConnArrayFree( &pMod->vConns );
    MstaModCellArrayFree( &pMod->vCells );
    MstaIdArrayFree( &pMod->vBitNames );
    memset( pMod, 0, sizeof(*pMod) );
}

static MstaModule *Msta_AddModule( MstaDesign *pDes, const char *pName )
{
    MstaId Name = Msta_NameId( pName );
    int nOld = Msta_IntMapGet( &pDes->modMap, Name, -1 );
    MstaModule *pMod;
    if ( nOld >= 0 )
    {
        pMod = MstaModuleArrayAt( &pDes->vModules, nOld );
        Msta_ModuleClear( pMod );
        pMod->Name = Name;
    }
    else
    {
        pMod = MstaModuleArrayAppend( &pDes->vModules );
        pMod->Name = Name;
        Msta_IntMapSet( &pDes->modMap, pMod->Name, pDes->vModules.nSize - 1 );
    }
    MstaModPortArrayInit( &pMod->vPorts );
    MstaModCellArrayInit( &pMod->vCells );
    MstaModConnArrayInit( &pMod->vConns );
    MstaIdArrayInit( &pMod->vBitNames );
    return pMod;
}

static void Msta_EnsureBitNames( MstaModule *pMod, int nBits )
{
    while ( pMod->vBitNames.nSize < nBits )
        *MstaIdArrayAppend( &pMod->vBitNames ) = MSTA_NO_ID;
}

int Msta_DesignReadYosysJson( MstaDesign *pDes, const char *pJsonFile, int fVerbose )
{
    char sError[256];
    MJsonValue *pRoot;
    MJsonValue *pModules;
    int m, i, k;
    int nMods = 0, nCellStmts = 0;

    sError[0] = 0;
    pRoot = Msta_JsonParseFile( pJsonFile, sError, (int)sizeof(sError) );
    if ( pRoot == NULL )
    {
        Msta_Error( "%s：%s\n", pJsonFile, sError[0] ? sError : "解析失败" );
        return 0;
    }
    pModules = Msta_JsonGet( pRoot, "modules" );
    if ( pModules == NULL )
    {
        Msta_Error( "\"%s\" 里没有 \"modules\"：它是 yosys write_json 生成的文件吗？\n", pJsonFile );
        Msta_JsonFree( pRoot );
        return 0;
    }

    for ( m = 0; m < Msta_JsonCount(pModules); m++ )
    {
        MJsonValue *pJsonMod = Msta_JsonAt( pModules, m );
        const char *pModName = Msta_JsonKeyAt( pModules, m );
        MstaModule *pMod = Msta_AddModule( pDes, pModName );
        MJsonValue *pPorts = Msta_JsonGet( pJsonMod, "ports" );
        MJsonValue *pCells = Msta_JsonGet( pJsonMod, "cells" );
        MJsonValue *pNetNames = Msta_JsonGet( pJsonMod, "netnames" );
        int nMaxBit = 0;

        /* --- ports：{ 名字: {"direction":..., "bits":[...]} } --- */
        for ( i = 0; i < Msta_JsonCount(pPorts); i++ )
        {
            MJsonValue *pJsonPort = Msta_JsonAt( pPorts, i );
            const char *pDirText = Msta_JsonStr( pJsonPort, "direction", "input" );
            MstaModPort *pNew = MstaModPortArrayAppend( &pMod->vPorts );
            pNew->Name   = Msta_NameId( Msta_JsonKeyAt(pPorts, i) );
            pNew->Offset = Msta_JsonInt( pJsonPort, "offset", 0 );
            pNew->fUpto  = Msta_JsonInt( pJsonPort, "upto", 0 );
            MstaIdArrayInit( &pNew->Bits );
            if      ( !strcmp(pDirText, "output") ) pNew->Dir = MSTA_PORT_OUT;
            else if ( !strcmp(pDirText, "inout")  ) pNew->Dir = MSTA_PORT_INOUT;
            else                                    pNew->Dir = MSTA_PORT_IN;
            Msta_ReadBits( Msta_JsonGet(pJsonPort, "bits"), &pNew->Bits, &nMaxBit );
        }

        /* --- cells：{ 实例名: {"type":..., "connections":{引脚:[bits]}} } --- */
        for ( i = 0; i < Msta_JsonCount(pCells); i++ )
        {
            MJsonValue *pJsonCell = Msta_JsonAt( pCells, i );
            MJsonValue *pConns = Msta_JsonGet( pJsonCell, "connections" );
            MstaModCell *pNew = MstaModCellArrayAppend( &pMod->vCells );
            pNew->Name      = Msta_NameId( Msta_JsonKeyAt(pCells, i) );
            pNew->Type      = Msta_NameId( Msta_JsonStr(pJsonCell, "type", "?") );
            pNew->ConnFirst = pMod->vConns.nSize;
            for ( k = 0; k < Msta_JsonCount(pConns); k++ )
            {
                MstaModConn *pConn = MstaModConnArrayAppend( &pMod->vConns );
                pConn->Pin = Msta_NameId( Msta_JsonKeyAt(pConns, k) );
                MstaIdArrayInit( &pConn->Bits );
                Msta_ReadBits( Msta_JsonAt(pConns, k), &pConn->Bits, &nMaxBit );
            }
            pNew->ConnCount = pMod->vConns.nSize - pNew->ConnFirst;
            nCellStmts++;
        }

        /* --- netnames：给每个局部网络一个可读名字（多位总线带上 [bit] 下标） --- */
        for ( i = 0; i < Msta_JsonCount(pNetNames); i++ )
        {
            MJsonValue *pEntry = Msta_JsonAt( pNetNames, i );
            const char *pNetName = Msta_JsonKeyAt( pNetNames, i );
            int nOffset = Msta_JsonInt( pEntry, "offset", 0 );
            int fUpto = Msta_JsonInt( pEntry, "upto", 0 );
            MstaIdArray vBits;
            int b, nDummy = 0;
            MstaIdArrayInit( &vBits );
            Msta_ReadBits( Msta_JsonGet(pEntry, "bits"), &vBits, &nDummy );
            Msta_EnsureBitNames( pMod, nDummy );
            for ( b = 0; b < vBits.nSize; b++ )
            {
                int nBit = vBits.pData[b];
                if ( nBit >= 0 && pMod->vBitNames.pData[nBit] == MSTA_NO_ID )
                {
                    if ( vBits.nSize == 1 && nOffset == 0 )
                        pMod->vBitNames.pData[nBit] = Msta_NameId( pNetName );
                    else
                    {
                        char sBitName[8192];
                        int nIdx = fUpto ? ( nOffset + vBits.nSize - 1 - b ) : ( nOffset + b );
                        snprintf( sBitName, sizeof(sBitName), "%s[%d]", pNetName, nIdx );
                        pMod->vBitNames.pData[nBit] = Msta_NameId( sBitName );
                    }
                }
            }
            MstaIdArrayFree( &vBits );
        }
        pMod->nBits = nMaxBit;
        Msta_EnsureBitNames( pMod, nMaxBit );
        nMods++;
    }

    /* 标出被例化过的模块：没人例化的那个就是顶层。 */
    for ( i = 0; i < pDes->vModules.nSize; i++ )
        MstaModuleArrayAt( &pDes->vModules, i )->fInstantiated = 0;
    for ( i = 0; i < pDes->vModules.nSize; i++ )
    {
        MstaModule *pMod = MstaModuleArrayAt( &pDes->vModules, i );
        int c;
        for ( c = 0; c < pMod->vCells.nSize; c++ )
        {
            MstaModCell *pCell = MstaModCellArrayAt( &pMod->vCells, c );
            int nSub = Msta_IntMapGet( &pDes->modMap, pCell->Type, -1 );
            if ( nSub >= 0 )
                MstaModuleArrayAt( &pDes->vModules, nSub )->fInstantiated = 1;
        }
    }

    Msta_JsonFree( pRoot );
    if ( fVerbose )
        Msta_Info( "yosys json \"%s\"：模块 %d 个，单元语句 %d 条\n",
                   pJsonFile, nMods, nCellStmts );
    return 1;
}

/* =====================================================================
   2. 调用 yosys：Verilog -> JSON
   ===================================================================== */

/* 文件名里出现 shell 元字符就直接拒绝，不做拼接 —— 防命令注入。 */
static int Msta_IsShellSafePath( const char *pPath )
{
    const char *p;
    for ( p = pPath; *p; p++ )
        if ( strchr( "\"'`$;&|<>()\n\\*", *p ) )
            return 0;
    return 1;
}

int Msta_DesignReadVerilog( MstaDesign *pDes, const char **ppFiles, int nFiles,
                            const char *pWorkDir, int fVerbose )
{
    /* 命令形如
         yosys -q -l <work>/yosys.log -p ' read_verilog "a.v"; write_json "<work>/netlist.json"'
       用 Verilog-2005 模式（不加 -sv）：综合网表里常出现 int / logic 之类的
       信号名，在 -sv 模式下会被当成类型关键字而报语法错。 */
    char sReads[16384];
    char sCmd[24576];
    char sJson[1024], sLog[1024];
    int i, nRet, nLen = 0, fOk;

    if ( nFiles <= 0 || pWorkDir == NULL )
    {
        Msta_Error( "read_verilog：没有给出文件\n" );
        return 0;
    }
    if ( !Msta_IsShellSafePath(pWorkDir) )
    {
        Msta_Error( "工作目录里有 msta 不会传给 shell 的字符：\"%s\"\n",
                    pWorkDir );
        return 0;
    }
    snprintf( sJson, sizeof(sJson), "%s/netlist.json", pWorkDir );
    snprintf( sLog, sizeof(sLog), "%s/yosys.log", pWorkDir );
    for ( i = 0; i < nFiles; i++ )
    {
        if ( !Msta_IsShellSafePath(ppFiles[i]) )
        {
            Msta_Error( "文件名里有 msta 不会传给 shell 的字符：\"%s\"\n",
                        ppFiles[i] );
            return 0;
        }
        nLen += snprintf( sReads + nLen, sizeof(sReads) - (size_t)nLen,
                          " read_verilog \"%s\";", ppFiles[i] );
        if ( nLen >= (int)sizeof(sReads) - 128 )
        {
            Msta_Error( "read_verilog：文件太多\n" );
            return 0;
        }
    }
    snprintf( sCmd, sizeof(sCmd), "yosys -q -l \"%s\" -p '%s write_json \"%s\"'",
              sLog, sReads, sJson );
    if ( fVerbose )
        Msta_Info( "前端：%s\n", sCmd );
    nRet = system( sCmd );
    if ( nRet != 0 )
    {
        unlink( sJson );
        Msta_Error( "yosys 运行失败（退出码 %d）；详见 %s\n", nRet, sLog );
        return 0;
    }
    fOk = Msta_DesignReadYosysJson( pDes, sJson, fVerbose );
    unlink( sJson );
    unlink( sLog );
    return fOk;
}

/* =====================================================================
   3. 展平
   ===================================================================== */

static int Msta_PathPush( MstaFlatten *pF, const char *pName )
{
    int nSaved = pF->nPathLen;
    int nAdd = (int)strlen(pName);
    while ( pF->nPathLen + nAdd + 2 > pF->nPathCap )
    {
        pF->nPathCap = pF->nPathCap ? pF->nPathCap * 2 : 4096;
        pF->pPath = (char *)realloc( pF->pPath, (size_t)pF->nPathCap );
        assert( pF->pPath );
    }
    if ( pF->nPathLen )
        pF->pPath[pF->nPathLen++] = '/';
    memcpy( pF->pPath + pF->nPathLen, pName, (size_t)nAdd );
    pF->nPathLen += nAdd;
    pF->pPath[pF->nPathLen] = 0;
    return nSaved;
}

static void Msta_PathPop( MstaFlatten *pF, int nSaved )
{
    pF->nPathLen = nSaved;
    pF->pPath[nSaved] = 0;
}

/* 新建一根全局网络，返回它的编号。名字进 netNameMap，SDC 靠它找回网络。 */
static int Msta_NewNet( MstaFlatten *pF, MstaId Name, int fConst )
{
    MstaDesign *pDes = pF->pDes;
    MstaNet *pNet = MstaNetArrayAppend( &pDes->vNets );
    pNet->Name = Name;
    pNet->fConst = fConst;
    pNet->Driver.InstId = MSTA_NO_ID;
    pNet->Driver.PinId  = MSTA_NO_ID;
    MstaPinRefArrayInit( &pNet->vLoads );
    if ( Msta_IntMapGet( &pDes->netNameMap, Name, -1 ) < 0 )
        Msta_IntMapSet( &pDes->netNameMap, Name, pDes->vNets.nSize - 1 );
    return pDes->vNets.nSize - 1;
}

/* 把"本模块的某个 bit"变成全局网络号。
     pMap[nBit] >= 0 -> 该 bit 已经有全局号（端口由父模块给，或本模块已建好）
     pMap == NULL 或该项为 -1（内部网络，或父模块没接的端口）-> 按层次路径新建一根网络
   常量 bit 永远指向两根常量网络。 */
static int Msta_GlobalNetForBit( MstaFlatten *pF, MstaModule *pMod, const int *pMap, int nBit )
{
    MstaDesign *pDes = pF->pDes;
    char sName[8192];
    char sFallback[32];
    const char *pLocal;

    if ( nBit == MSTA_BIT_CONST0 ) return pDes->nConst0Net;
    if ( nBit == MSTA_BIT_CONST1 ) return pDes->nConst1Net;
    if ( nBit < 0 || nBit >= pMod->nBits )
    {
        Msta_WarnOnce( "模块 \"%s\" 引用的位 %d 超出范围",
                       Msta_NameStr(pMod->Name), nBit );
        return pDes->nConst0Net;
    }
    if ( pMap && pMap[nBit] >= 0 )
        return pMap[nBit];

    pLocal = ( nBit < pMod->vBitNames.nSize && pMod->vBitNames.pData[nBit] != MSTA_NO_ID )
           ? Msta_NameStr( pMod->vBitNames.pData[nBit] ) : NULL;
    /* yosys 没给名字的中间网络用 n<编号> 顶上，保证网络名唯一、报告里也可读。 */
    if ( pLocal == NULL )
    {
        snprintf( sFallback, sizeof(sFallback), "n%d", nBit );
        pLocal = sFallback;
    }
    if ( pF->nPathLen )
        snprintf( sName, sizeof(sName), "%s/%s", pF->pPath, pLocal );
    else
        snprintf( sName, sizeof(sName), "%s", pLocal );
    return Msta_NewNet( pF, Msta_NameId( sName ), 0 );
}

/* 库里查不到的单元：造一个占位 cell。引脚取第一次遇到该类型时的那些连接名，
   方向先留 MSTA_DIR_NO，由 Msta_InferBlackBoxDirs 事后推断。返回 vBlackBoxCells 下标。 */
static int Msta_BlackBoxCell( MstaFlatten *pF, MstaModule *pMod, MstaModCell *pCell )
{
    MstaDesign *pDes = pF->pDes;
    MstaCell *pNew;
    int i = Msta_IntMapGet( &pDes->blackBoxMap, pCell->Type, -1 );
    int c, nId;
    if ( i >= 0 )
    {
        MstaCell *pExisting = MstaCellArrayAt( &pDes->vBlackBoxCells, i );
        for ( c = 0; c < pCell->ConnCount; c++ )
        {
            MstaModConn *pConn = MstaModConnArrayAt( &pMod->vConns, pCell->ConnFirst + c );
            if ( Msta_CellPinIndexOf( pExisting, pConn->Pin ) < 0 )
            {
                MstaPin *pPin = MstaPinArrayAppend( &pExisting->vPins );
                pPin->Name    = pConn->Pin;
                pPin->Dir     = MSTA_DIR_NO;     /* 待推断 */
                pPin->MaxCap  = -1.0;
                pPin->MinCap  = -1.0;
                pPin->MaxSlew = -1.0;
            }
        }
        return i;
    }

    pNew = MstaCellArrayAppend( &pDes->vBlackBoxCells );
    nId  = pDes->vBlackBoxCells.nSize - 1;
    Msta_IntMapSet( &pDes->blackBoxMap, pCell->Type, nId );
    MstaPinArrayInit( &pNew->vPins );
    MstaArcArrayInit( &pNew->vArcs );
    MstaRegCheckArrayInit( &pNew->vRegs );
    MstaAsyncCheckArrayInit( &pNew->vAsync );
    MstaGateCheckArrayInit( &pNew->vGates );
    pNew->Name      = pCell->Type;
    pNew->pLibName  = Msta_StrDup( Msta_NameStr(pCell->Type) );
    pNew->fBlackBox = 1;
    for ( c = 0; c < pCell->ConnCount; c++ )
    {
        MstaModConn *pConn = MstaModConnArrayAt( &pMod->vConns, pCell->ConnFirst + c );
        MstaPin *pPin;
        if ( Msta_CellPinIndexOf( pNew, pConn->Pin ) >= 0 )
            continue;
        pPin = MstaPinArrayAppend( &pNew->vPins );
        pPin->Name    = pConn->Pin;
        pPin->Dir     = MSTA_DIR_NO;         /* 待推断 */
        pPin->MaxCap  = -1.0;
        pPin->MinCap  = -1.0;
        pPin->MaxSlew = -1.0;
    }
    pDes->nBlackBoxes++;
    Msta_WarnOnce( "单元 \"%s\" 不在库里：按理想时序的黑盒处理",
                   Msta_NameStr(pCell->Type) );
    return nId;
}

/* 把非黑盒实例绑定到库主表或 corner 变体表下标，避免库数组扩容后指针悬垂。 */
static void Msta_BindInstLibCell( MstaInst *pInst, MstaLib *pLib, MstaCell *pCell )
{
    pInst->pCell       = pCell;
    pInst->fBlackBox   = 0;
    pInst->fCornerCell = 0;
    pInst->CellId      = MSTA_NO_ID;
    if ( pLib == NULL || pCell == NULL )
        return;
    if ( pLib->vCells.nSize > 0 && pCell >= pLib->vCells.pData &&
         pCell < pLib->vCells.pData + pLib->vCells.nSize )
        pInst->CellId = (MstaId)( pCell - pLib->vCells.pData );
    else if ( pLib->vCornerCells.nSize > 0 && pCell >= pLib->vCornerCells.pData &&
              pCell < pLib->vCornerCells.pData + pLib->vCornerCells.nSize )
    {
        pInst->fCornerCell = 1;
        pInst->CellId      = (MstaId)( pCell - pLib->vCornerCells.pData );
    }
}

static void Msta_FlattenModule( MstaFlatten *pF, int nModId, const int *pPortMap )
{
    MstaDesign *pDes = pF->pDes;
    MstaModule *pMod = MstaModuleArrayAt( &pDes->vModules, nModId );
    int *pNetMap;
    int k, c, i;

    if ( pF->fError )
        return;
    if ( pF->nDepth > MSTA_MAX_HIER_DEPTH )
    {
        Msta_Error( "展平层次超过 %d 层（可能存在模块递归例化）\n", MSTA_MAX_HIER_DEPTH );
        pF->fError = 1;
        return;
    }
    pF->nDepth++;

    pNetMap = (int *)malloc( (size_t)(pMod->nBits > 0 ? pMod->nBits : 1) * sizeof(int) );
    assert( pNetMap );

    /* 1) 本模块实例的每个局部网络 -> 全局网络号 */
    for ( k = 0; k < pMod->nBits; k++ )
        pNetMap[k] = Msta_GlobalNetForBit( pF, pMod, pPortMap, k );

    /* 2) 顶层端口打方向标记（SDC 的 get_ports 要用） */
    if ( pF->nPathLen == 0 )
        for ( i = 0; i < pMod->vPorts.nSize; i++ )
        {
            MstaModPort *pPort = MstaModPortArrayAt( &pMod->vPorts, i );
            int b;
            for ( b = 0; b < pPort->Bits.nSize; b++ )
            {
                int nBit = pPort->Bits.pData[b];
                MstaNet *pNet;
                char sPortName[8192];
                if ( nBit < 0 || nBit >= pMod->nBits )
                    continue;
                pNet = MstaNetArrayAt( &pDes->vNets, pNetMap[nBit] );
                pNet->fTopPort = 1;
                pNet->Dir = pPort->Dir;
                if ( pPort->Bits.nSize == 1 && pPort->Offset == 0 )
                    snprintf( sPortName, sizeof(sPortName), "%s", Msta_NameStr(pPort->Name) );
                else
                {
                    int nIdx = pPort->fUpto
                             ? ( pPort->Offset + pPort->Bits.nSize - 1 - b )
                             : ( pPort->Offset + b );
                    snprintf( sPortName, sizeof(sPortName), "%s[%d]",
                              Msta_NameStr(pPort->Name), nIdx );
                }
                pNet->Name = Msta_NameId( sPortName );
                if ( Msta_IntMapGet( &pDes->netNameMap, pNet->Name, -1 ) < 0 )
                    Msta_IntMapSet( &pDes->netNameMap, pNet->Name, pNetMap[nBit] );
            }
        }

    /* 3) 实例 */
    for ( c = 0; c < pMod->vCells.nSize && !pF->fError; c++ )
    {
        MstaModCell *pCell = MstaModCellArrayAt( &pMod->vCells, c );
        int nSubId = Msta_IntMapGet( &pDes->modMap, pCell->Type, -1 );
        char sFullName[8192];

        if ( pF->nPathLen )
            snprintf( sFullName, sizeof(sFullName), "%s/%s", pF->pPath, Msta_NameStr(pCell->Name) );
        else
            snprintf( sFullName, sizeof(sFullName), "%s", Msta_NameStr(pCell->Name) );

        if ( nSubId >= 0 )
        {
            /* ---- 子模块：先算 "子模块端口的局部网络 -> 父模块的全局网络" ---- */
            MstaModule *pChild = MstaModuleArrayAt( &pDes->vModules, nSubId );
            int *pChildMap = (int *)malloc( (size_t)(pChild->nBits > 0 ? pChild->nBits : 1) * sizeof(int) );
            int nSavedPath, iConn;
            for ( k = 0; k < pChild->nBits; k++ )
                pChildMap[k] = -1;

            for ( iConn = 0; iConn < pCell->ConnCount; iConn++ )
            {
                MstaModConn *pConn = MstaModConnArrayAt( &pMod->vConns, pCell->ConnFirst + iConn );
                MstaModPort *pFormal = Msta_ModulePortByName( pChild, pConn->Pin );
                int b, n;
                if ( pFormal == NULL )
                {
                    Msta_WarnOnce( "模块 \"%s\" 没有端口 \"%s\"",
                                   Msta_NameStr(pChild->Name), Msta_NameStr(pConn->Pin) );
                    continue;
                }
                n = pFormal->Bits.nSize < pConn->Bits.nSize ? pFormal->Bits.nSize : pConn->Bits.nSize;
                if ( pFormal->Bits.nSize != pConn->Bits.nSize )
                    Msta_WarnOnce( "端口 \"%s.%s\" 是 %d 位，却连了 %d 位",
                                   Msta_NameStr(pChild->Name), Msta_NameStr(pFormal->Name),
                                   pFormal->Bits.nSize, pConn->Bits.nSize );
                for ( b = 0; b < n; b++ )       /* 两端都是低位在前，按下标配对 */
                {
                    int nChildBit = pFormal->Bits.pData[b];
                    if ( nChildBit < 0 || nChildBit >= pChild->nBits )
                        continue;
                    pChildMap[nChildBit] = Msta_GlobalNetForBit( pF, pMod, pNetMap,
                                                                 pConn->Bits.pData[b] );
                }
            }
            nSavedPath = Msta_PathPush( pF, Msta_NameStr( pCell->Name ) );
            Msta_FlattenModule( pF, nSubId, pChildMap );
            Msta_PathPop( pF, nSavedPath );
            free( pChildMap );
            continue;
        }

        /* ---- 叶子单元 ---- */
        {
            MstaCell *pLibCell = ( pF->pLib != NULL )
                               ? Msta_LibFindCell( pF->pLib, Msta_NameStr( pCell->Type ) )
                               : NULL;
            MstaInst *pNew;
            int iConn;
            if ( pLibCell == NULL )
            {
                MstaId nCellId = Msta_BlackBoxCell( pF, pMod, pCell );
                pLibCell = MstaCellArrayAt( &pDes->vBlackBoxCells, nCellId );
                pNew = MstaInstArrayAppend( &pDes->vInsts );
                pNew->Name        = Msta_NameId( sFullName );
                pNew->ModuleName  = pMod->Name;
                pNew->pCell       = pLibCell;
                pNew->CellId      = nCellId;
                pNew->fBlackBox   = 1;
                pNew->fCornerCell = 0;
            }
            else
            {
                pNew = MstaInstArrayAppend( &pDes->vInsts );
                pNew->Name       = Msta_NameId( sFullName );
                pNew->ModuleName = pMod->Name;
                Msta_BindInstLibCell( pNew, pF->pLib, pLibCell );
            }
            pNew->nPins      = pLibCell->vPins.nSize;
            pNew->pNets      = (MstaId *)malloc( (size_t)(pNew->nPins > 0 ? pNew->nPins : 1) * sizeof(MstaId) );
            for ( k = 0; k < pNew->nPins; k++ )
                pNew->pNets[k] = MSTA_NO_ID;

            for ( iConn = 0; iConn < pCell->ConnCount; iConn++ )
            {
                MstaModConn *pConn = MstaModConnArrayAt( &pMod->vConns, pCell->ConnFirst + iConn );
                int nPin = Msta_CellPinIndexOf( pLibCell, pConn->Pin );
                if ( nPin < 0 )
                {
                    Msta_WarnOnce( "单元 \"%s\" 没有引脚 \"%s\"",
                                   Msta_NameStr(pCell->Type), Msta_NameStr(pConn->Pin) );
                    continue;
                }
                if ( pConn->Bits.nSize == 0 )
                    continue;                              /* 悬空脚 */
                if ( pConn->Bits.nSize > 1 )
                    Msta_WarnOnce( "引脚 \"%s.%s\" 是 1 位，却连了 %d 位",
                                   Msta_NameStr(pCell->Type), Msta_NameStr(pConn->Pin), pConn->Bits.nSize );
                pNew->pNets[nPin] = Msta_GlobalNetForBit( pF, pMod, pNetMap, pConn->Bits.pData[0] );
            }
        }
    }
    free( pNetMap );
    pF->nDepth--;
}

/* 黑盒方向推断：一根网络没有已知驱动、又只挂着一个"方向未知"的脚，
   就认为那个脚是输出。规则简单，而且完全可解释。 */
static void Msta_InferBlackBoxDirs( MstaDesign *pDes )
{
    int nNets = pDes->vNets.nSize;
    int *pKnownDrivers = (int *)calloc( (size_t)nNets, sizeof(int) );
    int *pUnknownCount = (int *)calloc( (size_t)nNets, sizeof(int) );
    int *pUnknownInst  = (int *)calloc( (size_t)nNets, sizeof(int) );
    int *pUnknownPin   = (int *)calloc( (size_t)nNets, sizeof(int) );
    int i, c;
    assert( pKnownDrivers && pUnknownCount && pUnknownInst && pUnknownPin );

    for ( i = 0; i < nNets; i++ )
    {
        MstaNet *pNet = MstaNetArrayAt( &pDes->vNets, i );
        if ( pNet->fConst ||
             ( pNet->fTopPort && ( pNet->Dir == MSTA_PORT_IN || pNet->Dir == MSTA_PORT_INOUT ) ) )
            pKnownDrivers[i]++;
    }
    for ( i = 0; i < pDes->vInsts.nSize; i++ )
    {
        MstaInst *pInst = MstaInstArrayAt( &pDes->vInsts, i );
        for ( c = 0; c < pInst->nPins && c < pInst->pCell->vPins.nSize; c++ )
        {
            int nNet = pInst->pNets[c];
            MstaPin *pPin = MstaPinArrayAt( &pInst->pCell->vPins, c );
            if ( nNet < 0 || nNet >= nNets )
                continue;
            if ( pPin->Dir == MSTA_DIR_OUTPUT )
                pKnownDrivers[nNet]++;
            else if ( pPin->Dir == MSTA_DIR_NO )
            {
                pUnknownCount[nNet]++;
                pUnknownInst[nNet] = i;
                pUnknownPin[nNet]  = c;
            }
        }
    }
    for ( i = 0; i < nNets; i++ )
        if ( pKnownDrivers[i] == 0 && pUnknownCount[i] == 1 )
        {
            MstaInst *pInst = MstaInstArrayAt( &pDes->vInsts, pUnknownInst[i] );
            MstaPin *pPin = MstaPinArrayAt( &pInst->pCell->vPins, pUnknownPin[i] );
            pPin->Dir = MSTA_DIR_OUTPUT;
        }
    /* 剩下的未知方向脚一律当输入：不推断错，只漏推断。 */
    for ( i = 0; i < pDes->vBlackBoxCells.nSize; i++ )
    {
        MstaCell *pCell = MstaCellArrayAt( &pDes->vBlackBoxCells, i );
        for ( c = 0; c < pCell->vPins.nSize; c++ )
            if ( pCell->vPins.pData[c].Dir == MSTA_DIR_NO )
                pCell->vPins.pData[c].Dir = MSTA_DIR_INPUT;
    }
    free( pKnownDrivers ); free( pUnknownCount );
    free( pUnknownInst );  free( pUnknownPin );
}

/* 建立每根网络的 driver/loads。 */
static void Msta_BuildDrivers( MstaDesign *pDes )
{
    int i, c;
    for ( i = 0; i < pDes->vInsts.nSize; i++ )
    {
        MstaInst *pInst = MstaInstArrayAt( &pDes->vInsts, i );
        MstaCell *pCell = pInst->pCell;
        for ( c = 0; c < pInst->nPins && c < pCell->vPins.nSize; c++ )
        {
            int nNet = pInst->pNets[c];
            MstaPin *pPin = MstaPinArrayAt( &pCell->vPins, c );
            MstaNet *pNet;
            MstaPinRef Ref;
            if ( nNet == MSTA_NO_ID || nNet < 0 || nNet >= pDes->vNets.nSize )
                continue;
            pNet = MstaNetArrayAt( &pDes->vNets, nNet );
            Ref.InstId = (MstaId)i;
            Ref.PinId  = (MstaId)c;
            if ( pPin->Dir == MSTA_DIR_OUTPUT )
            {
                if ( pNet->Driver.InstId == MSTA_NO_ID )
                    pNet->Driver = Ref;
                else if ( !pNet->fConst )
                    Msta_WarnOnce( "网络 \"%s\" 有不止一个驱动", Msta_NameStr(pNet->Name) );
            }
            else if ( pPin->Dir == MSTA_DIR_INPUT || pPin->Dir == MSTA_DIR_INOUT )
                *MstaPinRefArrayAppend( &pNet->vLoads ) = Ref;
        }
    }
}

static void Msta_FreeBlackBoxes( MstaDesign *pDes )
{
    int i, k;
    for ( i = 0; i < pDes->vBlackBoxCells.nSize; i++ )
    {
        MstaCell *pCell = MstaCellArrayAt( &pDes->vBlackBoxCells, i );
        free( pCell->pLibName );
        for ( k = 0; k < pCell->vPins.nSize; k++ )
            free( pCell->vPins.pData[k].pFunc );
        MstaPinArrayFree( &pCell->vPins );
        MstaArcArrayFree( &pCell->vArcs );
        MstaRegCheckArrayFree( &pCell->vRegs );
        MstaAsyncCheckArrayFree( &pCell->vAsync );
        MstaGateCheckArrayFree( &pCell->vGates );
    }
    MstaCellArrayFree( &pDes->vBlackBoxCells );
    Msta_IntMapFree( &pDes->blackBoxMap );
    Msta_IntMapInit( &pDes->blackBoxMap, 256 );
    pDes->nBlackBoxes = 0;
}

void Msta_DesignClearFlattened( MstaDesign *pDes )
{
    int i;
    for ( i = 0; i < pDes->vNets.nSize; i++ )
        MstaPinRefArrayFree( &pDes->vNets.pData[i].vLoads );
    MstaNetArrayFree( &pDes->vNets );
    Msta_IntMapFree( &pDes->netNameMap );
    Msta_IntMapInit( &pDes->netNameMap, 1 << 16 );
    for ( i = 0; i < pDes->vInsts.nSize; i++ )
        free( pDes->vInsts.pData[i].pNets );
    MstaInstArrayFree( &pDes->vInsts );
    Msta_FreeBlackBoxes( pDes );
}

void Msta_DesignClearModules( MstaDesign *pDes )
{
    int i;
    for ( i = 0; i < pDes->vModules.nSize; i++ )
        Msta_ModuleClear( MstaModuleArrayAt( &pDes->vModules, i ) );
    MstaModuleArrayFree( &pDes->vModules );
    MstaModuleArrayInit( &pDes->vModules );
    Msta_IntMapFree( &pDes->modMap );
    Msta_IntMapInit( &pDes->modMap, 1 << 14 );
}

void Msta_DesignRefreshInstCells( MstaDesign *pDes, MstaLib *pLib )
{
    int i, k, fResolvedAny = 0;
    for ( i = 0; i < pDes->vInsts.nSize; i++ )
    {
        MstaInst *pInst = MstaInstArrayAt( &pDes->vInsts, i );
        if ( pInst->fBlackBox )
        {
            MstaCell *pOldCell = NULL;
            MstaCell *pResolved;
            if ( pInst->CellId >= 0 && pInst->CellId < pDes->vBlackBoxCells.nSize )
                pOldCell = MstaCellArrayAt( &pDes->vBlackBoxCells, pInst->CellId );
            if ( pOldCell == NULL )
                continue;
            pResolved = ( pLib != NULL )
                      ? Msta_LibFindCell( pLib, Msta_NameStr( pOldCell->Name ) ) : NULL;
            if ( pResolved != NULL && !pResolved->fIgnore )
            {
                int nNewPins = pResolved->vPins.nSize;
                MstaId *pNewNets = (MstaId *)malloc( (size_t)( nNewPins > 0 ? nNewPins : 1 ) * sizeof(MstaId) );
                assert( pNewNets );
                for ( k = 0; k < nNewPins; k++ )
                    pNewNets[k] = MSTA_NO_ID;
                for ( k = 0; k < pInst->nPins && k < pOldCell->vPins.nSize; k++ )
                {
                    int nDst = Msta_CellPinIndexOf( pResolved, pOldCell->vPins.pData[k].Name );
                    if ( nDst >= 0 && nDst < nNewPins )
                        pNewNets[nDst] = pInst->pNets[k];
                }
                free( pInst->pNets );
                pInst->pNets = pNewNets;
                pInst->nPins = nNewPins;
                Msta_BindInstLibCell( pInst, pLib, pResolved );
                fResolvedAny = 1;
            }
            else
            {
                if ( pOldCell->vPins.nSize > pInst->nPins )
                {
                    MstaId *pNewNets = (MstaId *)realloc(
                        pInst->pNets, (size_t)pOldCell->vPins.nSize * sizeof(MstaId) );
                    assert( pNewNets );
                    for ( k = pInst->nPins; k < pOldCell->vPins.nSize; k++ )
                        pNewNets[k] = MSTA_NO_ID;
                    pInst->pNets = pNewNets;
                    pInst->nPins = pOldCell->vPins.nSize;
                }
                pInst->pCell = pOldCell;
            }
        }
        else
            pInst->pCell = Msta_InstCell( pDes, pLib, pInst );
    }
    if ( fResolvedAny )
    {
        char *pfBbUsed = pDes->vBlackBoxCells.nSize > 0
                       ? (char *)calloc( (size_t)pDes->vBlackBoxCells.nSize, 1 ) : NULL;
        int nRemaining = 0;
        for ( i = 0; i < pDes->vNets.nSize; i++ )
        {
            MstaNet *pNet = MstaNetArrayAt( &pDes->vNets, i );
            pNet->Driver.InstId = MSTA_NO_ID;
            pNet->Driver.PinId  = MSTA_NO_ID;
            pNet->vLoads.nSize  = 0;
        }
        for ( i = 0; i < pDes->vInsts.nSize; i++ )
        {
            MstaInst *pInst = MstaInstArrayAt( &pDes->vInsts, i );
            if ( pInst->fBlackBox && pfBbUsed &&
                 pInst->CellId >= 0 && pInst->CellId < pDes->vBlackBoxCells.nSize &&
                 !pfBbUsed[pInst->CellId] )
            {
                pfBbUsed[pInst->CellId] = 1;
                nRemaining++;
            }
        }
        free( pfBbUsed );
        pDes->nBlackBoxes = nRemaining;
        for ( i = 0; i < pDes->vBlackBoxCells.nSize; i++ )
        {
            MstaCell *pBbCell = MstaCellArrayAt( &pDes->vBlackBoxCells, i );
            for ( k = 0; k < pBbCell->vPins.nSize; k++ )
                pBbCell->vPins.pData[k].Dir = MSTA_DIR_NO;
        }
        Msta_InferBlackBoxDirs( pDes );
        Msta_BuildDrivers( pDes );
    }
}

int Msta_DesignFlatten( MstaDesign *pDes, MstaLib *pLib, const char *pTopName, int fVerbose )
{
    MstaFlatten F;
    int nTop;

    if ( pTopName == NULL )
        pTopName = Msta_DesignGuessTop( pDes );
    if ( pTopName == NULL )
    {
        Msta_Error( "无法推断顶层模块；请先执行 \"current_design <name>\"\n" );
        return 0;
    }
    nTop = Msta_IntMapGet( &pDes->modMap, Msta_NameId(pTopName), -1 );
    if ( nTop < 0 )
    {
        Msta_Error( "网表里没有名为 \"%s\" 的模块\n", pTopName );
        return 0;
    }
    pDes->TopName = Msta_NameId( pTopName );

    /* 清掉上一次展平的结果，允许换 top 反复展平。 */
    Msta_DesignClearFlattened( pDes );

    memset( &F, 0, sizeof(F) );
    F.pDes = pDes;
    F.pLib = pLib;
    pDes->nConst0Net = Msta_NewNet( &F, Msta_NameId("__const0__"), 1 );
    pDes->nConst1Net = Msta_NewNet( &F, Msta_NameId("__const1__"), 2 );

    Msta_FlattenModule( &F, nTop, NULL );
    free( F.pPath );
    if ( F.fError )
    {
        Msta_DesignClearFlattened( pDes );
        return 0;
    }
    Msta_DesignRefreshInstCells( pDes, pLib );
    Msta_InferBlackBoxDirs( pDes );
    Msta_BuildDrivers( pDes );

    if ( fVerbose )
        Msta_DesignPrintStats( pDes, stdout );
    return 1;
}

/* =====================================================================
   4. 查询
   ===================================================================== */

static MstaModPort *Msta_ModulePortByName( MstaModule *pMod, MstaId PinName )
{
    int i;
    if ( PinName == MSTA_NO_ID )
        return NULL;
    for ( i = 0; i < pMod->vPorts.nSize; i++ )
        if ( pMod->vPorts.pData[i].Name == PinName )
            return MstaModPortArrayAt( &pMod->vPorts, i );
    return NULL;
}

const char *Msta_DesignGuessTop( MstaDesign *pDes )
{
    int i, nFound = 0, nLast = -1;
    for ( i = 0; i < pDes->vModules.nSize; i++ )
        if ( !pDes->vModules.pData[i].fInstantiated )
        {   nFound++; nLast = i;  }
    if ( nFound != 1 )
    {
        if ( nFound > 1 )
            Msta_WarnOnce( "有 %d 个模块没有被例化：请用 current_design <name> 指定顶层", nFound );
        return NULL;
    }
    return Msta_NameStr( pDes->vModules.pData[nLast].Name );
}

/* 走 netNameMap，所以是 O(1)；网络名带层次路径，例如 "u_core/u_alu/n42"。 */
int Msta_DesignNetByName( MstaDesign *pDes, const char *pNetName )
{
    return Msta_IntMapGet( &pDes->netNameMap, Msta_NameId(pNetName), -1 );
}

/* 实例的层次路径名 -> 下标。展平后实例名是唯一的，所以找到即返回。 */
int Msta_DesignFindInstByName( MstaDesign *pDes, const char *pInstName )
{
    MstaId Name = Msta_NameId( pInstName );
    int i;
    for ( i = 0; i < pDes->vInsts.nSize; i++ )
        if ( MstaInstArrayAt( &pDes->vInsts, i )->Name == Name )
            return i;
    return -1;
}

/* 实例的某个脚连到哪根网络。 */
int Msta_DesignInstPinNet( MstaDesign *pDes, int nInst, const char *pPinName )
{
    MstaInst *pInst;
    MstaId PinName;
    int nPin;
    if ( nInst < 0 || nInst >= pDes->vInsts.nSize )
        return -1;
    pInst = MstaInstArrayAt( &pDes->vInsts, nInst );
    PinName = Msta_NameId( pPinName );
    nPin = Msta_CellPinIndexOf( pInst->pCell, PinName );
    if ( nPin < 0 )
        return -1;
    return pInst->pNets[nPin];
}

/* 在位把实例 nInst 替换为同引脚签名的新单元 pNewCell：
   1) 严格校验引脚数、引脚名与方向一一对应（允许排列顺序不同）；
   2) 一次性按 pMap 重映所有关联网络的 Driver.PinId 与 vLoads[j].PinId，
      即使两脚短接同一网络也绝不会重复重映，并保留 vLoads 原顺序；
   3) 不符合兼容性要求时完全不改动现有网表，返回 0。 */
int Msta_DesignSwapInstCell( MstaDesign *pDes, MstaLib *pLib, int nInst, MstaCell *pNewCell,
                             int *pPinMapOut, int nPinCap )
{
    MstaInst *pInst;
    MstaCell *pOldCell;
    int nPins, k, j;
    int vMapStack[64];
    char vUsedStack[64];
    int *pMap = vMapStack;
    char *pUsed = vUsedStack;
    MstaId *pNewNets;

    if ( pDes == NULL || nInst < 0 || nInst >= pDes->vInsts.nSize || pNewCell == NULL )
        return 0;
    pInst = MstaInstArrayAt( &pDes->vInsts, nInst );
    pOldCell = pInst->pCell;
    if ( pInst->fBlackBox || pOldCell == NULL || pNewCell->fBlackBox )
    {
        Msta_Error( "swap_cell：实例 \"%s\" 涉及黑盒单元，无法替换\n",
                    Msta_NameStr(pInst->Name) );
        return 0;
    }
    if ( pOldCell->fIgnore || pNewCell->fIgnore )
    {
        Msta_Error( "swap_cell：单元 \"%s\" 是忽略单元（无时序模型），无法替换\n",
                    Msta_NameStr( pNewCell->fIgnore ? pNewCell->Name : pOldCell->Name ) );
        return 0;
    }
    if ( pOldCell->fSequential != pNewCell->fSequential ||
         pOldCell->fLatch != pNewCell->fLatch )
    {
        Msta_Error( "swap_cell：\"%s\" -> \"%s\" 时序/锁存类型不一致，拒绝替换\n",
                    Msta_NameStr(pOldCell->Name), Msta_NameStr(pNewCell->Name) );
        return 0;
    }
    nPins = pOldCell->vPins.nSize;
    if ( pInst->nPins != nPins || pNewCell->vPins.nSize != nPins )
    {
        Msta_Error( "swap_cell：\"%s\" (%d pins) 与 \"%s\" (%d pins) 引脚数不匹配\n",
                    Msta_NameStr(pOldCell->Name), nPins,
                    Msta_NameStr(pNewCell->Name), pNewCell->vPins.nSize );
        return 0;
    }
    if ( nPins > 64 )
    {
        pMap = (int *)malloc( (size_t)nPins * sizeof(int) );
        pUsed = (char *)calloc( (size_t)nPins, 1 );
        assert( pMap && pUsed );
    }
    else
        memset( pUsed, 0, (size_t)nPins );

    for ( k = 0; k < nPins; k++ )
    {
        MstaPin *pOldPin = MstaPinArrayAt( &pOldCell->vPins, k );
        int nDst = Msta_CellPinIndexOf( pNewCell, pOldPin->Name );
        MstaPin *pNewPin;
        if ( nDst < 0 || nDst >= nPins || pUsed[nDst] )
        {
            Msta_Error( "swap_cell：目标单元 \"%s\" 缺少或重复引脚 \"%s\"\n",
                        Msta_NameStr(pNewCell->Name), Msta_NameStr(pOldPin->Name) );
            if ( nPins > 64 ) { free( pMap ); free( pUsed ); }
            return 0;
        }
        pNewPin = MstaPinArrayAt( &pNewCell->vPins, nDst );
        if ( pNewPin->Dir != pOldPin->Dir )
        {
            Msta_Error( "swap_cell：引脚 \"%s\" 方向在 \"%s\" 与 \"%s\" 间不一致\n",
                        Msta_NameStr(pOldPin->Name), Msta_NameStr(pOldCell->Name),
                        Msta_NameStr(pNewCell->Name) );
            if ( nPins > 64 ) { free( pMap ); free( pUsed ); }
            return 0;
        }
        pMap[k] = nDst;
        pUsed[nDst] = 1;
    }

    pNewNets = (MstaId *)malloc( (size_t)( nPins > 0 ? nPins : 1 ) * sizeof(MstaId) );
    assert( pNewNets );
    for ( k = 0; k < nPins; k++ )
        pNewNets[pMap[k]] = pInst->pNets[k];

    /* 按去重后的关联网络一次性刷新 Driver/vLoads 中的 PinId。 */
    for ( k = 0; k < nPins; k++ )
    {
        int nNet = pInst->pNets[k], fDup = 0;
        MstaNet *pNet;
        if ( nNet == MSTA_NO_ID || nNet < 0 || nNet >= pDes->vNets.nSize )
            continue;
        for ( j = 0; j < k; j++ )
            if ( pInst->pNets[j] == nNet ) { fDup = 1; break; }
        if ( fDup )
            continue;
        pNet = MstaNetArrayAt( &pDes->vNets, nNet );
        if ( pNet->Driver.InstId == nInst &&
             pNet->Driver.PinId >= 0 && pNet->Driver.PinId < nPins )
            pNet->Driver.PinId = (MstaId)pMap[pNet->Driver.PinId];
        for ( j = 0; j < pNet->vLoads.nSize; j++ )
        {
            MstaPinRef *pRef = MstaPinRefArrayAt( &pNet->vLoads, j );
            if ( pRef->InstId == nInst && pRef->PinId >= 0 && pRef->PinId < nPins )
                pRef->PinId = (MstaId)pMap[pRef->PinId];
        }
    }

    if ( pPinMapOut != NULL )
    {
        int nCopy = nPins < nPinCap ? nPins : nPinCap;
        for ( k = 0; k < nCopy; k++ )
            pPinMapOut[k] = pMap[k];
    }

    free( pInst->pNets );
    pInst->pNets = pNewNets;
    pInst->nPins = nPins;
    Msta_BindInstLibCell( pInst, pLib, pNewCell );

    if ( nPins > 64 ) { free( pMap ); free( pUsed ); }
    return 1;
}

const char *Msta_NetName( MstaDesign *pDes, MstaId nNet )
{
    if ( nNet < 0 || nNet >= pDes->vNets.nSize )
        return "(no-net)";
    return Msta_NameStr( MstaNetArrayAt(&pDes->vNets, nNet)->Name );
}

const char *Msta_InstName( MstaDesign *pDes, MstaId nInst )
{
    if ( nInst < 0 || nInst >= pDes->vInsts.nSize )
        return "(no-inst)";
    return Msta_NameStr( MstaInstArrayAt(&pDes->vInsts, nInst)->Name );
}

void Msta_DesignPrintStats( MstaDesign *pDes, FILE *pFile )
{
    int i, nFF = 0, nBlack = 0, nUndriven = 0, nTopPorts = 0, nConst = 0;
    for ( i = 0; i < pDes->vInsts.nSize; i++ )
    {
        MstaCell *pCell = MstaInstArrayAt( &pDes->vInsts, i )->pCell;
        if ( pCell->fSequential ) nFF++;
        if ( pCell->fBlackBox )   nBlack++;
    }
    for ( i = 0; i < pDes->vNets.nSize; i++ )
    {
        MstaNet *pNet = MstaNetArrayAt( &pDes->vNets, i );
        if ( pNet->fConst )
        {   nConst++;  continue;  }
        if ( pNet->fTopPort )
            nTopPorts++;
        else if ( pNet->Driver.InstId == MSTA_NO_ID && pNet->vLoads.nSize > 0 )
            nUndriven++;
    }
    fprintf( pFile, "Design       : %s\n", Msta_NameStr( pDes->TopName ) );
    fprintf( pFile, "Modules      : %d parsed    instances after flatten: %d\n",
             pDes->vModules.nSize, pDes->vInsts.nSize );
    fprintf( pFile, "Nets         : %d (top ports: %d, constant nets: %d)\n",
             pDes->vNets.nSize, nTopPorts, nConst );
    fprintf( pFile, "Registers    : %d    black boxes: %d    undriven nets with loads: %d\n",
             nFF, nBlack, nUndriven );
}

MstaDesign *Msta_DesignStart( void )
{
    MstaDesign *p = (MstaDesign *)calloc( 1, sizeof(MstaDesign) );
    assert( p );
    MstaModuleArrayInit( &p->vModules );
    Msta_IntMapInit( &p->modMap, 1 << 14 );
    Msta_IntMapInit( &p->netNameMap, 1 << 16 );
    MstaNetArrayInit( &p->vNets );
    MstaInstArrayInit( &p->vInsts );
    MstaCellArrayInit( &p->vBlackBoxCells );
    Msta_IntMapInit( &p->blackBoxMap, 256 );
    return p;
}

void Msta_DesignFree( MstaDesign *pDes )
{
    if ( pDes == NULL )
        return;
    Msta_DesignClearModules( pDes );
    Msta_DesignClearFlattened( pDes );
    Msta_IntMapFree( &pDes->modMap );
    Msta_IntMapFree( &pDes->netNameMap );
    Msta_IntMapFree( &pDes->blackBoxMap );
    free( pDes );
}
