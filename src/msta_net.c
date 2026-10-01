/**CFile***************************************************************

  FileName    [msta_net.c]

  Synopsis    [yosys JSON 前端 + 层次展平。]

  分工：
    读入（Msta_DesignReadYosysJson）只把 JSON 的形状搬进 MstaModule，不做化简；
    展平（Msta_DesignFlatten）给"每个模块实例的每个局部网络"分配全局编号，
    并把单元实例落到一张扁平表上，供时序引擎使用。

***********************************************************************/

#include <ctype.h>
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
} MstaFlatten;

/* ---------------------------------------------------------------------
   本文件内部的提前声明（自上而下阅读时先看到接口）
   --------------------------------------------------------------------- */
static MstaModPort *Msta_ModulePortByName( MstaModule *pMod, MstaId PinName );
static int          Msta_GlobalNetForBit( MstaFlatten *pF, MstaModule *pMod,
                                          const int *pMap, int nBit );
static MstaCell    *Msta_BlackBoxCell( MstaFlatten *pF, MstaModule *pMod, MstaModCell *pCell );
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
            Msta_WarnOnce( "a pin is tied to 1'bx/1'bz; msta ties it to constant 0" );
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
            Msta_WarnOnce( "unsupported bit value in the yosys json (treated as constant 0)" );
            nBit = MSTA_BIT_CONST0;
        }
        *MstaIdArrayAppend( pOut ) = nBit;
        if ( nBit >= *pnMaxBit )
            *pnMaxBit = nBit + 1;
    }
}

static MstaModule *Msta_AddModule( MstaDesign *pDes, const char *pName )
{
    MstaModule *pMod = MstaModuleArrayAppend( &pDes->vModules );
    pMod->Name = Msta_NameId( pName );
    MstaModPortArrayInit( &pMod->vPorts );
    MstaModCellArrayInit( &pMod->vCells );
    MstaModConnArrayInit( &pMod->vConns );
    MstaIdArrayInit( &pMod->vBitNames );
    Msta_IntMapSet( &pDes->modMap, pMod->Name, pDes->vModules.nSize - 1 );
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
        Msta_Error( "%s: %s\n", pJsonFile, sError[0] ? sError : "parse failed" );
        return 0;
    }
    pModules = Msta_JsonGet( pRoot, "modules" );
    if ( pModules == NULL )
    {
        Msta_Error( "\"%s\" has no \"modules\": is it a yosys write_json file?\n", pJsonFile );
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
            pNew->Name = Msta_NameId( Msta_JsonKeyAt(pPorts, i) );
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

        /* --- netnames：给每个局部网络一个可读名字，报告里直接能看到 --- */
        for ( i = 0; i < Msta_JsonCount(pNetNames); i++ )
        {
            MJsonValue *pEntry = Msta_JsonAt( pNetNames, i );
            const char *pNetName = Msta_JsonKeyAt( pNetNames, i );
            MstaIdArray vBits;
            int b, nDummy = 0;
            MstaIdArrayInit( &vBits );
            Msta_ReadBits( Msta_JsonGet(pEntry, "bits"), &vBits, &nDummy );
            Msta_EnsureBitNames( pMod, nDummy );
            for ( b = 0; b < vBits.nSize; b++ )
            {
                int nBit = vBits.pData[b];
                if ( nBit >= 0 && pMod->vBitNames.pData[nBit] == MSTA_NO_ID )
                    pMod->vBitNames.pData[nBit] = Msta_NameId( pNetName );
            }
            MstaIdArrayFree( &vBits );
        }
        pMod->nBits = nMaxBit;
        Msta_EnsureBitNames( pMod, nMaxBit );
        nMods++;
    }

    /* 标出被例化过的模块：没人例化的那个就是顶层。 */
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
        Msta_Info( "yosys json \"%s\": %d modules, %d cell statements\n",
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
        Msta_Error( "read_verilog: no file given.\n" );
        return 0;
    }
    if ( !Msta_IsShellSafePath(pWorkDir) )
    {
        Msta_Error( "work directory contains characters msta will not pass to the shell: \"%s\"\n",
                    pWorkDir );
        return 0;
    }
    snprintf( sJson, sizeof(sJson), "%s/netlist.json", pWorkDir );
    snprintf( sLog, sizeof(sLog), "%s/yosys.log", pWorkDir );
    for ( i = 0; i < nFiles; i++ )
    {
        if ( !Msta_IsShellSafePath(ppFiles[i]) )
        {
            Msta_Error( "file name contains characters msta will not pass to the shell: \"%s\"\n",
                        ppFiles[i] );
            return 0;
        }
        nLen += snprintf( sReads + nLen, sizeof(sReads) - (size_t)nLen,
                          " read_verilog \"%s\";", ppFiles[i] );
        if ( nLen >= (int)sizeof(sReads) - 128 )
        {
            Msta_Error( "read_verilog: too many files.\n" );
            return 0;
        }
    }
    snprintf( sCmd, sizeof(sCmd), "yosys -q -l \"%s\" -p '%s write_json \"%s\"'",
              sLog, sReads, sJson );
    if ( fVerbose )
        Msta_Info( "front-end: %s\n", sCmd );
    nRet = system( sCmd );
    if ( nRet != 0 )
    {
        unlink( sJson );
        Msta_Error( "yosys failed (exit %d); see %s\n", nRet, sLog );
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
        Msta_WarnOnce( "module \"%s\" refers to bit %d which is out of range",
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
   方向先留 MSTA_DIR_NO，由 Msta_InferBlackBoxDirs 事后推断。 */
static MstaCell *Msta_BlackBoxCell( MstaFlatten *pF, MstaModule *pMod, MstaModCell *pCell )
{
    MstaDesign *pDes = pF->pDes;
    MstaCell *pNew;
    int i = Msta_IntMapGet( &pDes->blackBoxMap, pCell->Type, -1 );
    int c;
    if ( i >= 0 )
        return MstaCellArray2At( &pDes->vBlackBoxCells, i );

    pNew = MstaCellArray2Append( &pDes->vBlackBoxCells );
    Msta_IntMapSet( &pDes->blackBoxMap, pCell->Type, pDes->vBlackBoxCells.nSize - 1 );
    MstaPinArrayInit( &pNew->vPins );
    MstaArcArrayInit( &pNew->vArcs );
    MstaRegCheckArrayInit( &pNew->vRegs );
    pNew->Name      = pCell->Type;
    pNew->pLibName  = Msta_StrDup( Msta_NameStr(pCell->Type) );
    pNew->fBlackBox = 1;
    for ( c = 0; c < pCell->ConnCount; c++ )
    {
        MstaModConn *pConn = MstaModConnArrayAt( &pMod->vConns, pCell->ConnFirst + c );
        MstaPin *pPin = MstaPinArrayAppend( &pNew->vPins );
        pPin->Name    = pConn->Pin;
        pPin->Dir     = MSTA_DIR_NO;         /* 待推断 */
        pPin->MaxCap  = -1.0;
        pPin->MaxSlew = -1.0;
    }
    pDes->nBlackBoxes++;
    Msta_WarnOnce( "cell \"%s\" is not in the library: treated as a black box with ideal timing",
                   Msta_NameStr(pCell->Type) );
    return pNew;
}

static void Msta_FlattenModule( MstaFlatten *pF, int nModId, const int *pPortMap )
{
    MstaDesign *pDes = pF->pDes;
    MstaModule *pMod = MstaModuleArrayAt( &pDes->vModules, nModId );
    int *pNetMap = (int *)malloc( (size_t)(pMod->nBits > 0 ? pMod->nBits : 1) * sizeof(int) );
    int k, c, i;

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
                if ( pPort->Bits.nSize == 1 )
                    snprintf( sPortName, sizeof(sPortName), "%s", Msta_NameStr(pPort->Name) );
                else
                    snprintf( sPortName, sizeof(sPortName), "%s[%d]",
                              Msta_NameStr(pPort->Name), b );
                pNet->Name = Msta_NameId( sPortName );
                if ( Msta_IntMapGet( &pDes->netNameMap, pNet->Name, -1 ) < 0 )
                    Msta_IntMapSet( &pDes->netNameMap, pNet->Name, pNetMap[nBit] );
            }
        }

    /* 3) 实例 */
    for ( c = 0; c < pMod->vCells.nSize; c++ )
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
                    Msta_WarnOnce( "module \"%s\" has no port \"%s\"",
                                   Msta_NameStr(pChild->Name), Msta_NameStr(pConn->Pin) );
                    continue;
                }
                n = pFormal->Bits.nSize < pConn->Bits.nSize ? pFormal->Bits.nSize : pConn->Bits.nSize;
                if ( pFormal->Bits.nSize != pConn->Bits.nSize )
                    Msta_WarnOnce( "port \"%s.%s\" is %d bits but %d bits are connected",
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
            MstaCell *pLibCell = Msta_LibFindCell( pF->pLib, Msta_NameStr( pCell->Type ) );
            MstaInst *pNew;
            int iConn;
            if ( pLibCell == NULL )
                pLibCell = Msta_BlackBoxCell( pF, pMod, pCell );

            pNew = MstaInstArrayAppend( &pDes->vInsts );
            pNew->Name       = Msta_NameId( sFullName );
            pNew->ModuleName = pMod->Name;
            pNew->pCell      = pLibCell;
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
                    Msta_WarnOnce( "cell \"%s\" has no pin \"%s\"",
                                   Msta_NameStr(pCell->Type), Msta_NameStr(pConn->Pin) );
                    continue;
                }
                if ( pConn->Bits.nSize == 0 )
                    continue;                              /* 悬空脚 */
                if ( pConn->Bits.nSize > 1 )
                    Msta_WarnOnce( "pin \"%s.%s\" is 1 bit but %d bits are connected",
                                   Msta_NameStr(pCell->Type), Msta_NameStr(pConn->Pin), pConn->Bits.nSize );
                pNew->pNets[nPin] = Msta_GlobalNetForBit( pF, pMod, pNetMap, pConn->Bits.pData[0] );
            }
        }
    }
    free( pNetMap );
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
        MstaCell *pCell = MstaCellArray2At( &pDes->vBlackBoxCells, i );
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
                    Msta_WarnOnce( "net \"%s\" has more than one driver", Msta_NameStr(pNet->Name) );
            }
            else if ( pPin->Dir == MSTA_DIR_INPUT || pPin->Dir == MSTA_DIR_INOUT )
                *MstaPinRefArrayAppend( &pNet->vLoads ) = Ref;
        }
    }
}

int Msta_DesignFlatten( MstaDesign *pDes, MstaLib *pLib, const char *pTopName, int fVerbose )
{
    MstaFlatten F;
    int nTop, i;

    if ( pTopName == NULL )
        pTopName = Msta_DesignGuessTop( pDes );
    if ( pTopName == NULL )
    {
        Msta_Error( "cannot guess the top module; say \"current_design <name>\" first.\n" );
        return 0;
    }
    nTop = Msta_IntMapGet( &pDes->modMap, Msta_NameId(pTopName), -1 );
    if ( nTop < 0 )
    {
        Msta_Error( "the netlist has no module named \"%s\".\n", pTopName );
        return 0;
    }
    pDes->TopName = Msta_NameId( pTopName );

    /* 清掉上一次展平的结果，允许换 top 反复展平。 */
    for ( i = 0; i < pDes->vNets.nSize; i++ )
        MstaPinRefArrayFree( &pDes->vNets.pData[i].vLoads );
    MstaNetArrayFree( &pDes->vNets );
    Msta_IntMapFree( &pDes->netNameMap );
    Msta_IntMapInit( &pDes->netNameMap, 1 << 16 );
    for ( i = 0; i < pDes->vInsts.nSize; i++ )
        free( pDes->vInsts.pData[i].pNets );
    MstaInstArrayFree( &pDes->vInsts );

    memset( &F, 0, sizeof(F) );
    F.pDes = pDes;
    F.pLib = pLib;
    pDes->nConst0Net = Msta_NewNet( &F, Msta_NameId("__const0__"), 1 );
    pDes->nConst1Net = Msta_NewNet( &F, Msta_NameId("__const1__"), 2 );

    Msta_FlattenModule( &F, nTop, NULL );
    Msta_InferBlackBoxDirs( pDes );
    Msta_BuildDrivers( pDes );
    free( F.pPath );

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
            Msta_WarnOnce( "%d modules are not instantiated by anyone: say current_design <name>", nFound );
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
    MstaCellArray2Init( &p->vBlackBoxCells );
    Msta_IntMapInit( &p->blackBoxMap, 256 );
    return p;
}

void Msta_DesignFree( MstaDesign *pDes )
{
    int i, k;
    if ( pDes == NULL )
        return;
    for ( i = 0; i < pDes->vModules.nSize; i++ )
    {
        MstaModule *pMod = MstaModuleArrayAt( &pDes->vModules, i );
        for ( k = 0; k < pMod->vPorts.nSize; k++ )
            MstaIdArrayFree( &pMod->vPorts.pData[k].Bits );
        MstaModPortArrayFree( &pMod->vPorts );
        for ( k = 0; k < pMod->vConns.nSize; k++ )
            MstaIdArrayFree( &pMod->vConns.pData[k].Bits );
        MstaModConnArrayFree( &pMod->vConns );
        MstaModCellArrayFree( &pMod->vCells );
        MstaIdArrayFree( &pMod->vBitNames );
    }
    MstaModuleArrayFree( &pDes->vModules );
    for ( i = 0; i < pDes->vNets.nSize; i++ )
        MstaPinRefArrayFree( &pDes->vNets.pData[i].vLoads );
    MstaNetArrayFree( &pDes->vNets );
    for ( i = 0; i < pDes->vInsts.nSize; i++ )
        free( pDes->vInsts.pData[i].pNets );
    MstaInstArrayFree( &pDes->vInsts );
    for ( i = 0; i < pDes->vBlackBoxCells.nSize; i++ )
    {
        MstaCell *pCell = MstaCellArray2At( &pDes->vBlackBoxCells, i );
        free( pCell->pLibName );
        MstaPinArrayFree( &pCell->vPins );
        MstaArcArrayFree( &pCell->vArcs );
        MstaRegCheckArrayFree( &pCell->vRegs );
    }
    MstaCellArray2Free( &pDes->vBlackBoxCells );
    Msta_IntMapFree( &pDes->modMap );
    Msta_IntMapFree( &pDes->netNameMap );
    Msta_IntMapFree( &pDes->blackBoxMap );
    free( pDes );
}
