/**CFile***************************************************************

  FileName    [msta_sdc.c]

  Synopsis    [SDC 读入与分发：Tcl 桥、JSON 读入、集合展开、分发表，以及 MstaSdc 的生命周期。]

  Tcl 前端负责 SDC 语法、变量、集合和 source；这里起 tclsh 跑 scripts/sdc_bridge.tcl，
  读回它输出的 JSON 命令数组，把每条命令里的集合标记展开成对象名，再按分发表
  s_vSdcCommands 交给通用解析器（msta_sdc_parse.c）和各条命令的处理函数
  （msta_sdc_clock.c / msta_sdc_io.c / msta_sdc_except.c / msta_sdc_env.c）。
  展开后的参数与本条命令的临时内存也归本文件管：解析与处理函数通过
  msta_sdc_int.h 里的 Msta_SdcArena / Msta_SdcKindOf 等函数使用它们。
  未建模的命令记录告警并计入忽略计数。时序引擎用的查询接口在 msta_sdc_query.c。

***********************************************************************/

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include "msta_sdc_int.h"
#include "msta_json.h"

/* 展开后的一条命令：每个词一项。
   pKinds 与 scripts/sdc_bridge.tcl 的集合类型标记保持一致。
   小写类型 = 该集合用了 -quiet（方言写法，即其他工具支持、SDC 1.8 里没有的非标准写法），
   压掉"没匹配到对象"的提示。pArg 记每个词来自命令的第几个 Tcl 参数：一个参数
   可能是展开成多个名字的集合，选项的值要按参数取。 */
typedef struct {
    char **ppText;
    char  *pKinds;
    char  *pQuiet;
    int   *pArg;
    int    nSize;
    int    nCap;
} MstaSdcArgList;

static MstaSdcArgList s_Args;
static int  *s_pArgHash;
static int   s_nArgHashCap;
static void **s_ppArena;
static int    s_nArenaUsed, s_nArenaCap;
static char s_pBridgePath[4096] = "scripts/sdc_bridge.tcl";

void *Msta_SdcArenaKeep( void *pMem )
{
    if ( s_nArenaUsed == s_nArenaCap )
    {
        s_nArenaCap = s_nArenaCap ? 2 * s_nArenaCap : 256;
        s_ppArena = (void **)realloc( s_ppArena, (size_t)s_nArenaCap * sizeof(void *) );
        assert( s_ppArena );
    }
    s_ppArena[s_nArenaUsed++] = pMem;
    return pMem;
}

static void Msta_SdcArenaReset( void )
{
    int i;
    for ( i = 0; i < s_nArenaUsed; i++ )
        free( s_ppArena[i] );
    s_nArenaUsed = 0;
}

char *Msta_SdcArena( const char *pFormat, ... )
{
    va_list Args;
    char *pBuf;
    int nLen;
    va_start( Args, pFormat );
    nLen = vsnprintf( NULL, 0, pFormat, Args );
    va_end( Args );
    pBuf = (char *)malloc( (size_t)nLen + 1 );
    assert( pBuf );
    va_start( Args, pFormat );
    vsnprintf( pBuf, (size_t)nLen + 1, pFormat, Args );
    va_end( Args );
    return (char *)Msta_SdcArenaKeep( pBuf );
}

static void Msta_SdcArgListPush( MstaSdcArgList *pL, char *pText, char Kind, char fQuiet )
{
    if ( pL->nSize == pL->nCap )
    {
        pL->nCap = pL->nCap ? 2 * pL->nCap : 64;
        pL->ppText = (char **)realloc( pL->ppText, (size_t)pL->nCap * sizeof(char *) );
        pL->pKinds = (char *)realloc( pL->pKinds, (size_t)pL->nCap );
        pL->pQuiet = (char *)realloc( pL->pQuiet, (size_t)pL->nCap );
        pL->pArg   = (int *)realloc( pL->pArg, (size_t)pL->nCap * sizeof(int) );
        assert( pL->ppText && pL->pKinds && pL->pQuiet && pL->pArg );
    }
    pL->ppText[pL->nSize] = pText;
    pL->pKinds[pL->nSize] = Kind;
    pL->pQuiet[pL->nSize] = fQuiet;
    pL->pArg[pL->nSize]   = 0;
    pL->nSize++;
}

static void Msta_SdcArgListFree( MstaSdcArgList *pL )
{
    free( pL->ppText );
    free( pL->pKinds );
    free( pL->pQuiet );
    free( pL->pArg );
    memset( pL, 0, sizeof(MstaSdcArgList) );
}

static unsigned Msta_SdcPtrHash( const void *pPtr )
{
    return (unsigned)( ( (uintptr_t)pPtr >> 3 ) * 2654435761u );
}

static void Msta_SdcBuildArgHash( void )
{
    int i;
    s_nArgHashCap = 16;
    while ( s_nArgHashCap < 2 * s_Args.nSize )
        s_nArgHashCap *= 2;
    free( s_pArgHash );
    s_pArgHash = (int *)calloc( (size_t)s_nArgHashCap, sizeof(int) );
    assert( s_pArgHash );
    for ( i = 0; i < s_Args.nSize; i++ )
    {
        unsigned h = Msta_SdcPtrHash( s_Args.ppText[i] ) & (unsigned)( s_nArgHashCap - 1 );
        while ( s_pArgHash[h] != 0 && s_Args.ppText[s_pArgHash[h] - 1] != s_Args.ppText[i] )
            h = ( h + 1 ) & (unsigned)( s_nArgHashCap - 1 );
        if ( s_pArgHash[h] == 0 )
            s_pArgHash[h] = i + 1;
    }
}

static int Msta_SdcArgIndex( const char *pText )
{
    unsigned h;
    if ( s_pArgHash == NULL )
        return -1;
    h = Msta_SdcPtrHash( pText ) & (unsigned)( s_nArgHashCap - 1 );
    while ( s_pArgHash[h] != 0 )
    {
        if ( s_Args.ppText[s_pArgHash[h] - 1] == pText )
            return s_pArgHash[h] - 1;
        h = ( h + 1 ) & (unsigned)( s_nArgHashCap - 1 );
    }
    return -1;
}

static void Msta_SdcScratchFree( void )
{
    Msta_SdcArenaReset();
    free( s_ppArena );
    s_ppArena = NULL;
    s_nArenaCap = 0;
    Msta_SdcArgListFree( &s_Args );
    free( s_pArgHash );
    s_pArgHash = NULL;
    s_nArgHashCap = 0;
}

/* 从可执行文件的位置推出 Tcl 桥接脚本的路径。 */
void Msta_SdcSetBridgePath( const char *pExecutable )
{
    char sResolved[4096], sCandidate[4096];
    char *pSlash;
    if ( pExecutable == NULL || strchr(pExecutable,'/') == NULL ||
         realpath(pExecutable,sResolved) == NULL )
        return;
    pSlash = strrchr(sResolved,'/');
    if ( pSlash == NULL ) return;
    *pSlash = 0;
    snprintf(sCandidate,sizeof(sCandidate),"%s/../scripts/sdc_bridge.tcl",sResolved);
    if ( access(sCandidate,R_OK) == 0 )
        snprintf(s_pBridgePath,sizeof(s_pBridgePath),"%s",sCandidate);
}

/* 取 Tcl 集合参数记录的对象类型。 */
char Msta_SdcKindOf( const char *pText )
{
    int i = Msta_SdcArgIndex( pText );
    return i >= 0 ? s_Args.pKinds[i] : 0;
}

/* =====================================================================
   读入与分发
   ===================================================================== */

static int Msta_SdcRunOne( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, int argc, char **argv,
                           const int *pArg );

/* 集合是否用了 -quiet：用了就不报 "没匹配到对象"。 */
int Msta_SdcIsQuiet( const char *pText )
{
    int i = Msta_SdcArgIndex( pText );
    return i >= 0 ? s_Args.pQuiet[i] : 0;
}

/* all_* 标记里打包的选项：按 \x1f 切开，返回下一个（就地截断）。 */
static char *Msta_SdcNextOption( char **ppText )
{
    char *p = *ppText, *pEnd;
    if ( p == NULL || *p == 0 )
        return NULL;
    pEnd = strchr(p,'\037');
    if ( pEnd ) *pEnd++ = 0;
    else pEnd = p + strlen(p);
    *ppText = pEnd;
    return p;
}

/* 集合减法（remove_from_collection）：minus= 后面跟名字或模式，命中就跳过。
   一条集合标记里可能挂多条，按 \x1f 分隔。 */
#define MSTA_SDC_MAX_MINUS 64
static int Msta_SdcCollectMinus( char *pOptions, const char **pMinus )
{
    int n = 0;
    char *pText = pOptions;
    while ( pText != NULL )
    {
        char *pOpt = Msta_SdcNextOption( &pText );
        if ( pOpt == NULL )
            break;
        if ( !strncmp(pOpt,"minus=",6) && n < MSTA_SDC_MAX_MINUS )
            pMinus[n++] = pOpt + 6;
    }
    return n;
}

/* 名字是否落在 minus 列表里（支持 * ? 模式）。 */
static int Msta_SdcIsMinus( const char *pName, const char **pMinus, int nMinus )
{
    int i;
    for ( i = 0; i < nMinus; i++ )
        if ( !strcmp(pMinus[i],pName) || Msta_SdcGlobMatch(pMinus[i],pName) )
            return 1;
    return 0;
}

/* 这个网络是不是某个时钟的源网络（all_inputs -no_clocks 这种方言写法用）。 */
static int Msta_SdcIsClockSourceNet( MstaSdc *p, int nNet )
{
    int i;
    for ( i = 0; i < p->vClocks.nSize; i++ )
        if ( MstaClockArrayAt(&p->vClocks,i)->SourceNet == nNet )
            return 1;
    return 0;
}

/* 这个端口有没有挂在该时钟（或它的源网络）上的 I/O 延迟约束。 */
static int Msta_SdcPortHasClockDelay( MstaSdc *p, int nNet, char *pClock, int fOutput )
{
    MstaIoDelayArray *pArr = fOutput ? &p->vOutputDelays : &p->vInputDelays;
    MstaClock *pNamed = pClock ? Msta_SdcFindClock(p,pClock) : NULL;
    int i;
    for ( i = 0; i < pArr->nSize; i++ )
    {
        MstaIoDelay *pEntry = MstaIoDelayArrayAt(pArr,i);
        if ( pEntry->Net != nNet )
            continue;
        if ( pNamed == NULL || pEntry->Clock == pNamed->Name ||
             (pEntry->Clock == MSTA_NO_ID &&
              (pEntry->RefNetMax >= 0 || pEntry->RefNetMin >= 0)) )
            return 1;
    }
    return 0;
}

/* 集合查询与时序引擎使用相同的 case 条件时钟弧 sense。 */
static int Msta_SdcClockArcSense( MstaDesign *pDes, MstaInst *pInst, MstaArc *pArc )
{
    int i, Sense;
    signed char *Cases = (signed char *)malloc((size_t)pInst->pCell->vPins.nSize);
    assert(Cases);
    for ( i = 0; i < pInst->pCell->vPins.nSize; i++ )
    {
        int n = i < pInst->nPins ? pInst->pNets[i] : -1;
        MstaNet *pNet = n >= 0 ? MstaNetArrayAt(&pDes->vNets,n) : NULL;
        int v = pNet ? (pNet->fCaseValue ? pNet->fCaseValue : pNet->fConst) : 0;
        Cases[i] = v == 1 || v == 2 ? (signed char)(v - 1) : -1;
    }
    Sense = Msta_LibClockSense(pInst->pCell,pArc,Cases);
    free(Cases);
    return Sense;
}

/* all_registers -clock：把某个时钟树上的网络标出来（pfOnNet），顺便记极性
   （+1/-1，反相缓冲器与 set_clock_sense 都会翻）。规则与 msta_timing.c 的
   AttachClockIds 一致：从时钟源网络沿组合弧前推，遇到时序单元、-stop_propagation
   或本时钟派生的生成时钟源就停（生成时钟源不算在树上）。调用方负责把两个数组清零。 */
static void Msta_SdcMarkClockNets( MstaDesign *pDes, MstaSdc *pSdc, MstaClock *pClock,
                                   char *pfOnNet, char *pPolarity )
{
    int nNets = pDes->vNets.nSize;
    int *pQueue, nQueue = 0, q;
    int SourceStop = 0, SourceSense;
    if ( nNets <= 0 || pClock->SourceNet < 0 )
        return;
    pQueue = (int *)malloc( (size_t)(2 * nNets) * sizeof(int) );
    assert( pQueue );
    pfOnNet[pClock->SourceNet] = 1;
    SourceSense = Msta_SdcClockSense(pSdc,pClock->SourceNet,
                                     Msta_SdcClockIndexOf(pSdc,pClock->Name),&SourceStop);
    pPolarity[pClock->SourceNet] = SourceSense ? (char)SourceSense : 1;
    pQueue[nQueue++] = pClock->SourceNet;
    for ( q = 0; q < nQueue; q++ )
    {
        MstaNet *pNet = MstaNetArrayAt( &pDes->vNets, pQueue[q] );
        int i, Stop = 0, c = Msta_SdcClockIndexOf(pSdc,pClock->Name);
        (void)Msta_SdcClockSense(pSdc,pQueue[q],c,&Stop);
        for ( i = 0; i < pSdc->vClocks.nSize; i++ )
            if ( pSdc->vClocks.pData[i].MasterClock == pClock->Name &&
                 pSdc->vClocks.pData[i].SourceNet == pQueue[q] ) Stop = 1;
        if ( Stop ) continue;
        for ( i = 0; i < pNet->vLoads.nSize; i++ )
        {
            MstaPinRef *pRef = MstaPinRefArrayAt( &pNet->vLoads, i );
            MstaInst *pLoad = MstaInstArrayAt( &pDes->vInsts, pRef->InstId );
            MstaCell *pCell = pLoad->pCell;
            MstaPin *pInPin = MstaPinArrayAt( &pCell->vPins, pRef->PinId );
            int k;
            if ( pCell->fSequential )
                continue;                       /* 时序单元的时钟脚就是树梢 */
            for ( k = 0; k < pLoad->nPins && k < pCell->vPins.nSize; k++ )
            {
                MstaPin *pOutPin = MstaPinArrayAt( &pCell->vPins, k );
                MstaArc *pArc;
                int nOut = pLoad->pNets[k];
                int Pol = pPolarity[pQueue[q]];
                int nSet, fStop = 0, Sense, Joined;
                if ( pOutPin->Dir != MSTA_DIR_OUTPUT || nOut < 0 )
                    continue;
                pArc = Msta_CellCombArc( pCell, pInPin->Name, pOutPin->Name );
                if ( pArc == NULL )
                    continue;
                if ( Msta_SdcTimingDisabled( pSdc, pRef->InstId, pInPin->Name, pOutPin->Name ) )
                    continue;
                Sense = Msta_SdcClockArcSense(pDes,pLoad,pArc);
                if ( Sense < 0 ) continue;
                if ( Sense == MSTA_SENSE_NEGATIVE ) Pol = -Pol;
                else if ( Sense == MSTA_SENSE_NONUNATE ) Pol = 0;
                nSet = Msta_SdcClockSense( pSdc, nOut, Msta_SdcClockIndexOf(pSdc,pClock->Name),
                                           &fStop );
                if ( nSet != 0 )
                    Pol = nSet;
                /* 合并极性：set_clock_sense 的显式值优先；否则首次到达取本路极性，
                   另一条路径带着不同极性再到达时记 0（non-unate，正反相都可能）。 */
                Joined = nSet ? nSet : (!pfOnNet[nOut] || pPolarity[nOut] == Pol ? Pol : 0);
                if ( !pfOnNet[nOut] || pPolarity[nOut] != Joined )
                {
                    pfOnNet[nOut] = 1;
                    pPolarity[nOut] = (char)Joined;
                    pQueue[nQueue++] = nOut;
                }
            }
        }
    }
    for ( q = 0; q < pSdc->vClocks.nSize; q++ )
        if ( pSdc->vClocks.pData[q].MasterClock == pClock->Name &&
             pSdc->vClocks.pData[q].SourceNet >= 0 ) pfOnNet[pSdc->vClocks.pData[q].SourceNet] = 0;
    free( pQueue );
}

/* 这个实例是不是"由某个时钟驱动"的寄存器：-rise_clock/-fall_clock 还要看
   有效边沿（库里的 fClkRises 经时钟树极性翻转）。 */
static int Msta_SdcRegMatchesClock( MstaInst *pInst, MstaCell *pCell,
                                    const char *pfOnNet, const char *pPolarity,
                                    int fRise, int fFall )
{
    int k;
    for ( k = 0; k < pCell->vRegs.nSize; k++ )
    {
        MstaRegCheck *pReg = MstaRegCheckArrayAt( &pCell->vRegs, k );
        int nPin = Msta_CellPinIndexOf( pCell, pReg->ClkPin );
        int nClkNet;
        int fRises;
        if ( nPin < 0 )
            continue;
        nClkNet = pInst->pNets[nPin];
        if ( nClkNet < 0 || !pfOnNet[nClkNet] )
            continue;
        if ( !fRise && !fFall )
            return 1;
        /* 边沿限定集合只选能确定触发源沿的寄存器；双标签路径由 -rise_to/-fall_to 裁剪。 */
        if ( pPolarity[nClkNet] == 0 ) continue;
        fRises = pReg->fClkRises ? ( pPolarity[nClkNet] > 0 ) : ( pPolarity[nClkNet] < 0 );
        if ( ( fRise && fRises ) || ( fFall && !fRises ) )
            return 1;
    }
    return 0;
}

/* 展开 all_inputs / all_outputs / all_registers / all_clocks / design 标记。 */
static int Msta_SdcExpandAll( MstaSdc *pSdc, MstaDesign *pDes, char *pSpec,
                              MstaSdcArgList *pL )
{
    char *pOptions = strchr(pSpec,'\035');
    int i, j;
    const char *pMinus[MSTA_SDC_MAX_MINUS];
    int nMinus = 0;
    if ( pOptions ) *pOptions++ = 0;
    if ( pOptions != NULL )
        nMinus = Msta_SdcCollectMinus( pOptions, pMinus );

    if ( !strcmp(pSpec,"design") )
    {
        Msta_SdcArgListPush( pL, (char *)"design", 'D', 0 );
        return 0;
    }
    if ( !strcmp(pSpec,"all_clocks") )
    {
        for ( j = 0; j < pSdc->vClocks.nSize; j++ )
        {
            if ( Msta_SdcIsMinus(Msta_NameStr(Msta_SdcClockByIndex(pSdc,j)->Name),pMinus,nMinus) )
                continue;
            Msta_SdcArgListPush( pL, (char *)Msta_NameStr( Msta_SdcClockByIndex(pSdc,j)->Name ),
                                 'C', 0 );
        }
        return 0;
    }
    if ( !strcmp(pSpec,"inputs") || !strcmp(pSpec,"outputs") )
    {
        int fInput = !strcmp(pSpec,"inputs");
        int fNoClocks = 0;
        char *pClock = NULL, *pOpt = pOptions;
        while ( (pOpt = Msta_SdcNextOption(&pOptions)) != NULL )
        {
            if ( !strncmp(pOpt,"minus=",6) ) continue;      /* 集合减法，见上面 */
            if ( !strncmp(pOpt,"clock=",6) ) pClock = pOpt + 6;
            else if ( !strcmp(pOpt,"no_clocks") ) fNoClocks = 1;
            else
            {
                /* -level_sensitive / -edge_triggered 会筛掉一部分端口，没建模就不能当没看见。 */
                Msta_WarnOnce( "all_%s option \"%s\" is not modeled; command skipped", pSpec, pOpt );
                return -1;
            }
        }
        for ( j = 0; j < pDes->vNets.nSize; j++ )
        {
            MstaNet *pNet = MstaNetArrayAt( &pDes->vNets, j );
            if ( !pNet->fTopPort || pNet->Dir != ( fInput ? MSTA_PORT_IN : MSTA_PORT_OUT ) )
                continue;
            if ( fNoClocks && Msta_SdcIsClockSourceNet(pSdc,j) )
                continue;
            if ( pClock && !Msta_SdcPortHasClockDelay(pSdc,j,pClock,!fInput) )
                continue;
            if ( Msta_SdcIsMinus(Msta_NetName(pDes,j),pMinus,nMinus) )
                continue;
            Msta_SdcArgListPush( pL, (char *)Msta_NetName( pDes, j ), 'P', 0 );
        }
        return 0;
    }
    if ( !strcmp(pSpec,"registers") )
    {
        enum { REG_CELLS, REG_DATA, REG_CLOCK, REG_ASYNC, REG_OUTPUT } Kind = REG_CELLS;
        char *pOpt = pOptions;
        const char *pFilterClock = NULL;
        int fRise = 0, fFall = 0, fFiltered = 0;
        char *pfOnNet = NULL, *pPolarity = NULL;
        while ( (pOpt = Msta_SdcNextOption(&pOptions)) != NULL )
        {
            if      ( !strncmp(pOpt,"minus=",6) )   continue;
            else if ( !strcmp(pOpt,"cells") )       Kind = REG_CELLS;
            else if ( !strcmp(pOpt,"data_pins") )   Kind = REG_DATA;
            else if ( !strcmp(pOpt,"clock_pins") )  Kind = REG_CLOCK;
            else if ( !strcmp(pOpt,"async_pins") )  Kind = REG_ASYNC;
            else if ( !strcmp(pOpt,"output_pins") ) Kind = REG_OUTPUT;
            else if ( !strncmp(pOpt,"clock=",6) )      pFilterClock = pOpt + 6;
            else if ( !strncmp(pOpt,"rise_clock=",11) ) { pFilterClock = pOpt + 11; fRise = 1; }
            else if ( !strncmp(pOpt,"fall_clock=",11) ) { pFilterClock = pOpt + 11; fFall = 1; }
            else
            {
                Msta_WarnOnce( "all_registers option \"%s\" is not modeled; command skipped", pOpt );
                return -1;
            }
        }
        /* -clock/-rise_clock/-fall_clock：按时钟树过滤（时钟树在解析期就能走出来，
           规则见 Msta_SdcMarkClockNets）。 */
        if ( pFilterClock != NULL )
        {
            MstaClock *pClock = Msta_SdcFindClock( pSdc, pFilterClock );
            if ( pClock == NULL )
            {
                Msta_WarnOnce( "all_registers -clock: unknown clock \"%s\"; command skipped",
                               pFilterClock );
                return -1;
            }
            pfOnNet = (char *)calloc( (size_t)pDes->vNets.nSize, 1 );
            pPolarity = (char *)calloc( (size_t)pDes->vNets.nSize, 1 );
            assert( pfOnNet && pPolarity );
            Msta_SdcMarkClockNets( pDes, pSdc, pClock, pfOnNet, pPolarity );
            fFiltered = 1;
        }
        for ( i = 0; i < pDes->vInsts.nSize; i++ )
        {
            MstaInst *pInst = MstaInstArrayAt( &pDes->vInsts, i );
            MstaCell *pCell = pInst->pCell;
            int k;
            if ( pCell->vRegs.nSize == 0 && pCell->vAsync.nSize == 0 )
                continue;
            if ( Msta_SdcIsMinus(Msta_InstName(pDes,i),pMinus,nMinus) )
                continue;
            if ( fFiltered &&
                 !Msta_SdcRegMatchesClock(pInst,pCell,pfOnNet,pPolarity,fRise,fFall) )
                continue;
            if ( Kind == REG_CELLS )
            {
                Msta_SdcArgListPush( pL, (char *)Msta_InstName( pDes, i ), 'I', 0 );
                continue;
            }
            if ( Kind == REG_ASYNC )
            {
                for ( k = 0; k < pCell->vAsync.nSize; k++ )
                {
                    MstaAsyncCheck *pAsync = MstaAsyncCheckArrayAt( &pCell->vAsync, k );
                    char *pName = Msta_SdcArena( "%s/%s", Msta_InstName(pDes,i),
                                                 Msta_NameStr(pAsync->AsyncPin) );
                    Msta_SdcArgListPush( pL, pName, 'N', 0 );
                }
                continue;
            }
            for ( k = 0; k < pCell->vRegs.nSize; k++ )
            {
                MstaRegCheck *pReg = MstaRegCheckArrayAt( &pCell->vRegs, k );
                MstaId Pin = ( Kind == REG_DATA )  ? pReg->DataPin
                           : ( Kind == REG_CLOCK ) ? pReg->ClkPin
                                                   : pReg->QPin;
                char *pName = Msta_SdcArena( "%s/%s", Msta_InstName(pDes,i),
                                             Msta_NameStr(Pin) );
                Msta_SdcArgListPush( pL, pName, 'N', 0 );
            }
        }
        free( pfOnNet );
        free( pPolarity );
        return 0;
    }
    return -1;
}

/* ---------------- get_* -of_objects 的对象关系 ---------------- */

/* 一个名字是否命中模式（模式可能写成"引脚名"或"实例/引脚"两种形式）。 */
static int Msta_SdcNameHit( const char *pPattern, const char *pName, const char *pAlt )
{
    if ( Msta_SdcGlobMatch( pPattern, pName ) )
        return 1;
    return pAlt != NULL && Msta_SdcGlobMatch( pPattern, pAlt );
}

/* 把 @e/@d/@f/@@ 换回控制字符。 */
static void Msta_SdcUnescapeMarker( char *pText )
{
    char *pSrc, *pDst;
    for ( pSrc = pDst = pText; *pSrc; pSrc++ )
        if ( *pSrc == '@' && pSrc[1] != 0 )
        {
            pSrc++;
            *pDst++ = ( *pSrc == 'e' ) ? '\036'
                     : ( *pSrc == 'd' ) ? '\035'
                     : ( *pSrc == 'f' ) ? '\037' : *pSrc;
        }
        else *pDst++ = *pSrc;
    *pDst = 0;
}

/* 把 -of_objects 里的父对象展开成"名字 + 类型"列表。父对象本身也可能是个集合
   （比如 all_registers），所以这里复用 all_* 与 get_* 两套展开。 */
static int Msta_SdcExpandOfObjects( MstaSdc *pSdc, MstaDesign *pDes, char *pOfMarker,
                                    const char *pBody, char Kind, const char *pMinus[],
                                    int nMinus, MstaSdcArgList *pL );

static int Msta_SdcExpandParents( MstaSdc *pSdc, MstaDesign *pDes, char *pMarker,
                                  MstaSdcArgList *pParents )
{
    if ( pMarker[0] == '\036' && pMarker[1] == 'A' )
    {
        if ( Msta_SdcExpandAll( pSdc, pDes, pMarker + 2, pParents ) < 0 )
            return 0;
        return pParents->nSize;
    }
    if ( pMarker[0] == '\036' )
    {
        char *pBody = pMarker + 2;
        char *pOptions = strchr( pBody, '\035' );
        char Kind = (char)toupper( (unsigned char)pMarker[1] );
        char *pPart;
        if ( pOptions != NULL )
        {
            /* 父集合自己也是 -of_objects（或者带 minus=）：递归展开它。 */
            char *pOf = NULL, *pOpt;
            const char *pMinusParent[MSTA_SDC_MAX_MINUS];
            int nMinusParent;
            *pOptions++ = 0;
            nMinusParent = Msta_SdcCollectMinus( pOptions, pMinusParent );
            for ( pOpt = strtok(pOptions,"\037"); pOpt != NULL; pOpt = strtok(NULL,"\037") )
                if ( !strncmp(pOpt,"of=",3) ) pOf = pOpt + 3;
            if ( pOf != NULL )
            {
                Msta_SdcUnescapeMarker( pOf );
                if ( Msta_SdcExpandOfObjects( pSdc, pDes, pOf, pBody, Kind,
                                              pMinusParent, nMinusParent, pParents ) < 0 )
                    return 0;
                return pParents->nSize;
            }
        }
        if ( pOptions != NULL ) *pOptions = 0;
        for ( pPart = strtok(pBody, "\037"); pPart != NULL; pPart = strtok(NULL, "\037") )
            Msta_SdcArgListPush( pParents, pPart, Kind, 0 );
        return pParents->nSize;
    }
    /* 裸名字列表（空格分隔）：当实例名处理。 */
    {
        char *pCopy = Msta_SdcArena( "%s", pMarker );
        char *pPart;
        for ( pPart = strtok(pCopy, " \t"); pPart != NULL; pPart = strtok(NULL, " \t") )
            Msta_SdcArgListPush( pParents, pPart, 'I', 0 );
        return pParents->nSize;
    }
}

/* 往结果里加一个对象，要求命中模式（pAlt 是同一个对象的另一种写法）。 */
static int Msta_SdcEmitRelated( char Kind, const char *pName, const char *pAlt,
                                const char *pPattern, const char *pMinus[], int nMinus,
                                MstaSdcArgList *pL )
{
    if ( !Msta_SdcNameHit( pPattern, pName, pAlt ) )
        return 0;
    if ( Msta_SdcIsMinus( pName, pMinus, nMinus ) )
        return 0;
    Msta_SdcArgListPush( pL, (char *)pName, Kind, 0 );
    return 1;
}

/* 一个实例按目标类型产出的对象（引脚/网络/实例/时钟）。 */
static int Msta_SdcRelatedOfInst( MstaSdc *pSdc, MstaDesign *pDes, int nInst, MstaId nOnlyPin,
                                  char Kind, const char *pPattern, const char *pMinus[],
                                  int nMinus, MstaSdcArgList *pL )
{
    MstaInst *pInst = MstaInstArrayAt( &pDes->vInsts, nInst );
    const char *pInstName = Msta_InstName( pDes, nInst );
    int k, nAdded = 0;
    for ( k = 0; k < pInst->nPins && k < pInst->pCell->vPins.nSize; k++ )
    {
        MstaPin *pPin = MstaPinArrayAt( &pInst->pCell->vPins, k );
        int nNet = pInst->pNets[k];
        char *pPinName;
        if ( nNet < 0 )
            continue;
        if ( nOnlyPin != MSTA_NO_ID && pPin->Name != nOnlyPin )
            continue;
        if ( Kind == 'N' )
        {
            int rc = Msta_SdcEmitRelated( 'N', Msta_NetName(pDes,nNet), NULL, pPattern,
                                          pMinus, nMinus, pL );
            nAdded += rc;
            continue;
        }
        if ( Kind != 'G' ) continue;
        pPinName = Msta_SdcArena( "%s/%s", pInstName, Msta_NameStr(pPin->Name) );
        {
            int rc = Msta_SdcEmitRelated( 'G', pPinName, Msta_NameStr(pPin->Name), pPattern,
                                          pMinus, nMinus, pL );
            nAdded += rc;
        }
    }
    if ( Kind == 'I' )
    {
        int rc = Msta_SdcEmitRelated( 'I', pInstName, NULL, pPattern, pMinus, nMinus,
                                      pL );
        nAdded += rc;
    }
    else if ( Kind == 'C' )
    {
        int c;
        for ( c = 0; c < pSdc->vClocks.nSize; c++ )
        {
            MstaClock *pClock = Msta_SdcClockByIndex( pSdc, c );
            int j;
            for ( j = 0; j < pInst->nPins && j < pInst->pCell->vPins.nSize; j++ )
            {
                int rc;
                if ( pInst->pNets[j] != pClock->SourceNet ) continue;
                rc = Msta_SdcEmitRelated( 'C', Msta_NameStr(pClock->Name), NULL, pPattern,
                                          pMinus, nMinus, pL );
                nAdded += rc;
            }
        }
    }
    return nAdded;
}

/* 一个网络/引脚/端口按目标类型产出的对象。 */
static int Msta_SdcRelatedOfNet( MstaSdc *pSdc, MstaDesign *pDes,
                                 int nNet, char Kind, const char *pPattern,
                                 const char *pMinus[], int nMinus, MstaSdcArgList *pL )
{
    int nAdded = 0, i;
    if ( Kind == 'N' )
        return Msta_SdcEmitRelated( 'N', Msta_NetName(pDes,nNet), NULL, pPattern,
                                    pMinus, nMinus, pL );
    if ( Kind == 'P' )
    {
        MstaNet *pNet = MstaNetArrayAt( &pDes->vNets, nNet );
        if ( pNet->fTopPort )
            return Msta_SdcEmitRelated( 'P', Msta_NetName(pDes,nNet), NULL, pPattern,
                                        pMinus, nMinus, pL );
        return 0;
    }
    if ( Kind == 'C' )
    {
        for ( i = 0; i < pSdc->vClocks.nSize; i++ )
        {
            MstaClock *pClock = Msta_SdcClockByIndex( pSdc, i );
            int rc;
            if ( pClock->SourceNet != nNet ) continue;
            rc = Msta_SdcEmitRelated( 'C', Msta_NameStr(pClock->Name), NULL, pPattern,
                                      pMinus, nMinus, pL );
            nAdded += rc;
        }
        return nAdded;
    }
    if ( Kind == 'G' || Kind == 'I' )
    {
        /* 网络上的驱动脚与负载脚；get_cells 时换成它们所属的实例。 */
        MstaNet *pNet = MstaNetArrayAt( &pDes->vNets, nNet );
        for ( i = -1; i < pNet->vLoads.nSize; i++ )
        {
            MstaPinRef Ref;
            char *pPinName;
            int rc;
            Ref = ( i < 0 ) ? pNet->Driver : *MstaPinRefArrayAt( &pNet->vLoads, i );
            if ( Ref.InstId == MSTA_NO_ID )
                continue;
            if ( Kind == 'I' )
            {
                rc = Msta_SdcEmitRelated( 'I', Msta_InstName(pDes,Ref.InstId), NULL, pPattern,
                                          pMinus, nMinus, pL );
                nAdded += rc;
                continue;
            }
            if ( i < 0 )
                continue;               /* get_pins -of_objects <net> 只看负载脚 */
            pPinName = Msta_SdcArena( "%s/%s", Msta_InstName(pDes,Ref.InstId),
                                      Msta_NameStr( MstaPinArrayAt(
                                          &MstaInstArrayAt(&pDes->vInsts,Ref.InstId)->pCell->vPins,
                                          Ref.PinId )->Name ) );
            rc = Msta_SdcEmitRelated( 'G', pPinName, NULL, pPattern, pMinus, nMinus,
                                      pL );
            nAdded += rc;
        }
    }
    return nAdded;
}

/* 把父对象解析成 (实例, 引脚, 网络) 三种形式：引脚写成 "实例/引脚"。 */
static void Msta_SdcResolveParent( MstaSdc *pSdc, MstaDesign *pDes, char ParentKind,
                                   const char *pName, int *pnInst, MstaId *pnPin, int *pnNet )
{
    *pnInst = -1;
    *pnPin = MSTA_NO_ID;
    *pnNet = -1;
    if ( ParentKind == 'I' )
    {
        *pnInst = Msta_DesignFindInstByName( pDes, pName );
        return;
    }
    if ( ParentKind == 'C' )
    {
        MstaClock *pClock = Msta_SdcFindClock( pSdc, pName );
        if ( pClock != NULL ) *pnNet = pClock->SourceNet;
        return;
    }
    if ( ParentKind == 'G' )
    {
        const char *pSlash = strrchr( pName, '/' );
        if ( pSlash != NULL && pSlash > pName )
        {
            char *pInst = Msta_SdcArena( "%.*s", (int)( pSlash - pName ), pName );
            *pnInst = Msta_DesignFindInstByName( pDes, pInst );
            *pnPin  = Msta_NameId( pSlash + 1 );
            return;
        }
    }
    {
        int *pNets, nFound = Msta_SdcResolveNets( pDes, pName, &pNets );
        if ( nFound == 1 ) *pnNet = pNets[0];
    }
}

/* -of_objects 的总入口：把每个父对象按关系展开，目标类型由 Kind 指定。 */
static int Msta_SdcExpandOfObjects( MstaSdc *pSdc, MstaDesign *pDes, char *pOfMarker,
                                    const char *pBody, char Kind, const char *pMinus[],
                                    int nMinus, MstaSdcArgList *pL )
{
    MstaSdcArgList Parents, Patterns;
    int nParents, i, j, nBeg = pL->nSize;
    char *pCopy;

    memset( &Parents, 0, sizeof(Parents) );
    memset( &Patterns, 0, sizeof(Patterns) );
    nParents = Msta_SdcExpandParents( pSdc, pDes, pOfMarker, &Parents );
    if ( nParents <= 0 )
    {
        Msta_WarnOnce( "-of_objects could not be resolved; command skipped" );
        Msta_SdcArgListFree( &Parents );
        return -1;
    }
    pCopy = Msta_SdcArena( "%s", pBody );
    for ( pCopy = strtok(pCopy,"\037"); pCopy != NULL; pCopy = strtok(NULL,"\037") )
        Msta_SdcArgListPush( &Patterns, pCopy, 0, 0 );
    if ( Patterns.nSize == 0 )
    {
        Msta_WarnOnce( "-of_objects needs a pattern" );
        Msta_SdcArgListFree( &Parents );
        return -1;
    }
    for ( i = 0; i < nParents; i++ )
    {
        int nInst = -1, nNet = -1;
        MstaId nPin = MSTA_NO_ID;
        Msta_SdcResolveParent( pSdc, pDes, Parents.pKinds[i], Parents.ppText[i],
                               &nInst, &nPin, &nNet );
        for ( j = 0; j < Patterns.nSize; j++ )
        {
            if ( nInst >= 0 )
                Msta_SdcRelatedOfInst( pSdc, pDes, nInst, nPin, Kind, Patterns.ppText[j],
                                       pMinus, nMinus, pL );
            else if ( nNet >= 0 )
                Msta_SdcRelatedOfNet( pSdc, pDes, nNet, Kind, Patterns.ppText[j],
                                      pMinus, nMinus, pL );
        }
    }
    Msta_SdcArgListFree( &Patterns );
    Msta_SdcArgListFree( &Parents );
    return pL->nSize - nBeg;          /* 一个都没有时由 Msta_SdcExpandRecord 作废整条命令 */
}

/* 集合标记对应的 SDC 命令名，告警用。 */
static const char *Msta_SdcCollectionName( char Kind )
{
    switch ( Kind )
    {
        case 'P': return "get_ports";
        case 'G': return "get_pins";
        case 'N': return "get_nets";
        case 'C': return "get_clocks";
        case 'I': return "get_cells";
        case 'L': return "get_libs";
        case 'B': return "get_lib_cells";
        case 'Y': return "get_lib_pins";
    }
    return "collection";
}

/* 给了对象集合、但集合是空的：整条命令作废。不能把它当成"没写对象"，
   否则 set_timing_derate 这类"不写对象就是全局"的命令会把约束加到整个设计上。 */
static void Msta_SdcEmptyCollection( MJsonValue *pRecord, const char *pWhat, const char *pForm )
{
    MJsonValue *pName = Msta_JsonAt( pRecord, 0 );
    Msta_WarnOnce( "%s: object collection [%s%s] is empty; constraint rejected",
                   pName ? pName->pStr : "sdc", pWhat, pForm );
}

static int Msta_SdcExpandRecord( MJsonValue *pRecord, MstaDesign *pDes, MstaSdc *pSdc )
{
    int i, k, nBefore;
    s_Args.nSize = 0;
    free( s_pArgHash );
    s_pArgHash = NULL;
    Msta_SdcArenaReset();
    if ( pRecord == NULL || pRecord->Kind != MJSON_ARRAY )
        return -1;
    for ( i = 0; i < pRecord->nItems; i++ )
    {
        MJsonValue *pWord = Msta_JsonAt( pRecord, i );
        char *pText;
        if ( pWord == NULL || pWord->Kind != MJSON_STRING )
            return -1;
        pText = pWord->pStr;
        nBefore = s_Args.nSize;
        if ( pText[0] == '\036' && pText[1] == 'A' )
        {
            if ( Msta_SdcExpandAll( pSdc, pDes, pText + 2, &s_Args ) < 0 )
            {
                Msta_WarnOnce( "sdc collection \"%s\" is not modeled; command skipped", pText + 2 );
                return 0;
            }
            if ( s_Args.nSize == nBefore )
            {
                Msta_SdcEmptyCollection( pRecord, strncmp( pText + 2, "all_", 4 ) ? "all_" : "", pText + 2 );
                return 0;
            }
        }
        else if ( pText[0] == '\036' && pText[1] == 'Z' )
        {
            /* 桥接脚本已经说明原因（选项不在 SDC 1.8 里，或者 msta 没建模）。 */
            return 0;
        }
        else if ( pText[0] == '\036' )
        {
            char Kind = (char)toupper( (unsigned char)pText[1] );
            int  fQuiet = islower( (unsigned char)pText[1] ) ? 1 : 0;
            char *pPart = pText + 2;
            const char *pMinus[MSTA_SDC_MAX_MINUS];
            int nMinus = 0;
            char *pOf = NULL;
            /* P/N/C/I/G 是网表对象（G 是引脚，与网络分开以便 -of_objects 推关系），
               L/B/Y 是库对象（get_libs/get_lib_cells/get_lib_pins）。 */
            if ( Kind != 'P' && Kind != 'N' && Kind != 'C' && Kind != 'I' && Kind != 'G' &&
                 Kind != 'L' && Kind != 'B' && Kind != 'Y' )
                return -1;
            {
                /* 选项区在 \x1d 之后（minus= 与 of= 都在这里）。 */
                char *pOptions = strchr( pPart, '\035' );
                if ( pOptions != NULL )
                {
                    char *pOpt;
                    *pOptions++ = 0;
                    nMinus = Msta_SdcCollectMinus( pOptions, pMinus );
                    for ( pOpt = strtok(pOptions,"\037"); pOpt != NULL; pOpt = strtok(NULL,"\037") )
                        if ( !strncmp(pOpt,"of=",3) )
                            pOf = pOpt + 3;
                }
            }
            /* -of_objects：按对象关系现推（父对象本身可能还是集合）。 */
            if ( pOf != NULL )
            {
                Msta_SdcUnescapeMarker( pOf );
                if ( Msta_SdcExpandOfObjects( pSdc, pDes, pOf, pPart, Kind,
                                              pMinus, nMinus, &s_Args ) < 0 )
                    return 0;               /* 父对象解析不了：丢这条命令（上面已告警） */
                if ( s_Args.nSize == nBefore )
                {
                    Msta_SdcEmptyCollection( pRecord, Msta_SdcCollectionName( Kind ), " -of_objects" );
                    return 0;
                }
                for ( k = nBefore; k < s_Args.nSize; k++ )
                    s_Args.pArg[k] = i;
                continue;
            }
            while ( *pPart )
            {
                char *pNext = strchr( pPart, '\037' );
                if ( pNext ) *pNext++ = 0;
                if ( Kind == 'C' && strpbrk(pPart,"*?") )
                {
                    int j, nMatched = 0;
                    for ( j = 0; j < pSdc->vClocks.nSize; j++ )
                    {
                        char *pName = (char *)Msta_NameStr(Msta_SdcClockByIndex(pSdc,j)->Name);
                        if ( !Msta_SdcGlobMatch(pPart,pName) ) continue;
                        if ( Msta_SdcIsMinus(pName,pMinus,nMinus) ) continue;
                        Msta_SdcArgListPush( &s_Args, pName, Kind, (char)fQuiet );
                        nMatched++;
                    }
                    if ( nMatched == 0 && !fQuiet )
                        Msta_WarnOnce("get_clocks pattern \"%s\" matched no clocks",pPart);
                }
                else
                {
                    if ( Msta_SdcIsMinus(pPart,pMinus,nMinus) )
                    {
                        if ( pNext == NULL ) break;
                        pPart = pNext;
                        continue;
                    }
                    Msta_SdcArgListPush( &s_Args, pPart, Kind, (char)fQuiet );
                }
                if ( pNext == NULL ) break;
                pPart = pNext;
            }
            if ( s_Args.nSize == nBefore )
            {
                Msta_SdcEmptyCollection( pRecord, Msta_SdcCollectionName( Kind ), "" );
                return 0;
            }
        }
        else
            Msta_SdcArgListPush( &s_Args, pText, 0, 0 );
        for ( k = nBefore; k < s_Args.nSize; k++ )
            s_Args.pArg[k] = i;
    }
    Msta_SdcBuildArgHash();
    return s_Args.nSize;
}

/* 网表对象索引：get_* -filter 的属性要用（Tcl 侧拿 expr 求值）。
   每行一条，制表符分隔：cell <实例> <库单元> / pin <实例/引脚> <方向> <是否时钟脚> /
   net <网络> <扇出> / port <端口> <方向>。只列网表本身的信息——时钟是 SDC 建的，
   这类属性（is_clock/is_virtual 等）只能靠 SDC，过滤不了。 */
static void Msta_SdcWriteDesignIndex( FILE *pFile, MstaDesign *pDes )
{
    int i, j;
    for ( i = 0; i < pDes->vNets.nSize; i++ )
    {
        MstaNet *pNet = MstaNetArrayAt( &pDes->vNets, i );
        if ( pNet->fTopPort )
            fprintf( pFile, "port\t%s\t%s\n", Msta_NetName(pDes,i),
                     pNet->Dir == MSTA_PORT_IN ? "in" : "out" );
        fprintf( pFile, "net\t%s\t%d\n", Msta_NetName(pDes,i), pNet->vLoads.nSize );
    }
    for ( i = 0; i < pDes->vInsts.nSize; i++ )
    {
        MstaInst *pInst = MstaInstArrayAt( &pDes->vInsts, i );
        MstaCell *pCell = pInst->pCell;
        fprintf( pFile, "cell\t%s\t%s\n", Msta_InstName(pDes,i),
                 Msta_NameStr(pCell->Name) );
        for ( j = 0; j < pInst->nPins && j < pCell->vPins.nSize; j++ )
        {
            MstaPin *pPin = MstaPinArrayAt( &pCell->vPins, j );
            const char *pDir = ( pPin->Dir == MSTA_DIR_INPUT )  ? "in"
                             : ( pPin->Dir == MSTA_DIR_OUTPUT ) ? "out"
                             : ( pPin->Dir == MSTA_DIR_INOUT )  ? "inout" : "internal";
            if ( pInst->pNets[j] < 0 )
                continue;
            fprintf( pFile, "pin\t%s/%s\t%s\t%d\n", Msta_InstName(pDes,i),
                     Msta_NameStr(pPin->Name), pDir, pPin->fClock ? 1 : 0 );
        }
    }
}

/* 把库里的对象索引写成文本给 Tcl 前端，供 get_libs / get_lib_cells / get_lib_pins
   查询。每行一条，制表符分隔：lib <名> / cell <库> <单元> / pin <库> <单元> <脚>。
   用纯文本而不是 Tcl 脚本，名字里出现任何字符都不用转义。 */
static void Msta_SdcWriteLibIndex( FILE *pFile, MstaLib *pLib )
{
    int i, j;
    if ( pLib == NULL )
        return;
    for ( i = 0; i < pLib->vLibs.nSize; i++ )
        fprintf( pFile, "lib\t%s\n",
                 Msta_NameStr( MstaLibInfoArrayAt( &pLib->vLibs, i )->Name ) );
    for ( i = 0; i < pLib->vCells.nSize + pLib->vCornerCells.nSize; i++ )
    {
        MstaCell *pCell = i < pLib->vCells.nSize
                        ? MstaCellArrayAt(&pLib->vCells,i)
                        : MstaCellArrayAt(&pLib->vCornerCells,i-pLib->vCells.nSize);
        fprintf( pFile, "cell\t%s\t%s\n", Msta_NameStr(pCell->LibName),
                 Msta_NameStr(pCell->Name) );
        for ( j = 0; j < pCell->vPins.nSize; j++ )
            fprintf( pFile, "pin\t%s\t%s\t%s\n", Msta_NameStr(pCell->LibName),
                     Msta_NameStr(pCell->Name),
                     Msta_NameStr( MstaPinArrayAt( &pCell->vPins, j )->Name ) );
    }
}

int Msta_SdcReadFile( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib,
                      const char *pFileName, int fVerbose )
{
    char sJsonPath[] = "/tmp/msta-sdc-XXXXXX";
    char sIndexPath[] = "/tmp/msta-lib-XXXXXX";
    char sDesignPath[] = "/tmp/msta-design-XXXXXX";
    char sError[256];
    MJsonValue *pCommands;
    pid_t Pid;
    int fd, fdIndex, fdDesign, fHaveIndex = 0, fHaveDesign = 0, Status, i;

    /* SDC 里不写 set_units 时，默认单位取库的单位。 */
    p->TimeScalePs = pLib ? pLib->TimeScale : 1000.0;

    fd = mkstemp( sJsonPath );
    if ( fd < 0 )
    {
        Msta_Error( "read_sdc: cannot create temporary file\n" );
        return 0;
    }
    close( fd );
    /* -filter 要在 Tcl 侧按属性筛对象，所以只有用到它时才导出网表索引
       （大设计上这份文件不小，平时不写）。 */
    {
        size_t nSdcSize = 0;
        char *pSdcText = Msta_FileReadAll( pFileName, &nSdcSize );
        if ( pSdcText != NULL )
        {
            if ( strstr( pSdcText, "-filter" ) != NULL )
            {
                fdDesign = mkstemp( sDesignPath );
                if ( fdDesign >= 0 )
                {
                    FILE *pDesign = fdopen( fdDesign, "w" );
                    if ( pDesign != NULL )
                    { Msta_SdcWriteDesignIndex( pDesign, pDes ); fclose( pDesign ); fHaveDesign = 1; }
                    else close( fdDesign );
                }
            }
            free( pSdcText );
        }
    }
    /* 库索引：Tcl 前端拿它回答 get_libs / get_lib_cells / get_lib_pins。 */
    fdIndex = mkstemp( sIndexPath );
    if ( fdIndex >= 0 )
    {
        FILE *pIndex = fdopen( fdIndex, "w" );
        if ( pIndex != NULL )
        { Msta_SdcWriteLibIndex( pIndex, pLib ); fclose( pIndex ); fHaveIndex = 1; }
        else close( fdIndex );
    }
    Pid = fork();
    if ( Pid == 0 )
    {
        if ( fHaveIndex && fHaveDesign )
            execlp( "tclsh", "tclsh", s_pBridgePath, pFileName, sJsonPath, sIndexPath,
                    sDesignPath, (char *)NULL );
        else if ( fHaveIndex )
            execlp( "tclsh", "tclsh", s_pBridgePath, pFileName, sJsonPath, sIndexPath,
                    (char *)NULL );
        else
            execlp( "tclsh", "tclsh", s_pBridgePath, pFileName, sJsonPath, (char *)NULL );
        perror( "read_sdc: cannot run tclsh" );
        _exit( 127 );
    }
    if ( Pid < 0 || waitpid(Pid, &Status, 0) < 0 || !WIFEXITED(Status) || WEXITSTATUS(Status) != 0 )
    {
        unlink( sJsonPath );
        unlink( sIndexPath );
        if ( fHaveDesign ) unlink( sDesignPath );
        Msta_Error( "read_sdc: Tcl parsing failed for \"%s\"\n", pFileName );
        return 0;
    }
    unlink( sIndexPath );
    if ( fHaveDesign ) unlink( sDesignPath );
    pCommands = Msta_JsonParseFile( sJsonPath, sError, sizeof(sError) );
    unlink( sJsonPath );
    if ( pCommands == NULL || pCommands->Kind != MJSON_ARRAY )
    {
        Msta_Error( "read_sdc: invalid parser output: %s\n", sError );
        Msta_JsonFree( pCommands );
        return 0;
    }
    for ( i = 0; i < pCommands->nItems; i++ )
    {
        int argc = Msta_SdcExpandRecord( Msta_JsonAt(pCommands,i), pDes, p );
        if ( argc <= 0 )
        {
            /* 单条命令不能展开就丢掉它；前面的约束仍然有效。 */
            if ( argc < 0 )
                Msta_WarnOnce( "read_sdc: malformed command %d; skipped", i + 1 );
            p->nCommandsIgnored++;
            continue;
        }
        Msta_SdcRunOne( p, pDes, pLib, argc, s_Args.ppText, s_Args.pArg );
    }
    Msta_SdcScratchFree();
    Msta_JsonFree( pCommands );
    if ( p->nCommandsIgnored > 0 )
        Msta_Warn( "sdc \"%s\": %d command(s) were not modeled and were ignored\n",
                   pFileName, p->nCommandsIgnored );
    if ( fVerbose )
        Msta_Info( "sdc \"%s\": %d commands read, %d ignored, %d clocks, %d exceptions\n",
                   pFileName, p->nCommandsRead, p->nCommandsIgnored,
                   p->vClocks.nSize, p->vExceptions.nSize );
    return 1;
}

/* SDC 1.8 手册里有、但 msta 不建模的命令：认下来、告警说明原因，只丢这一条。
   这样别家工具导出的约束文件能整份读完，缺的部分在日志里有明确交代。
   前八条是明确不支持的过时命令：它们描述老式线负载与驱动电阻模型，已由驱动
   单元和真实负载代替；有对应写法的（set_driving_cell / set_load）告警里写出。 */
static const struct { const char *pName; const char *pNote; } s_vIgnoredCommands[] = {
    { "set_drive", "obsolete command (input drive resistance); use set_driving_cell instead" },
    { "set_resistance", "obsolete command (net resistance); not modeled" },
    { "set_fanout_load", "obsolete command (fanout load units); use set_load instead" },
    { "set_port_fanout_number", "obsolete command (fanout load units); use set_load instead" },
    { "set_wire_load_min_block_size", "obsolete command (wire load models); not modeled" },
    { "set_wire_load_mode", "obsolete command (wire load models); not modeled" },
    { "set_wire_load_model", "obsolete command (wire load models); not modeled" },
    { "set_wire_load_selection_group", "obsolete command (wire load models); not modeled" },
    { "create_voltage_area", "not an STA constraint (multi-voltage design)" },
    { "set_level_shifter_strategy", "not an STA constraint (multi-voltage design)" },
    { "set_level_shifter_threshold", "not an STA constraint (multi-voltage design)" },
    { "set_max_dynamic_power", "not an STA constraint (power)" },
    { "set_max_leakage_power", "not an STA constraint (power)" },
    { NULL, NULL } };

/* 分发表：msta 建模的每条 SDC 命令一行，就是这条命令的语法：
   { 命令名, 选项表, 位置参数开头的值, 对象至少几个, 对象最多几个（-1 = 不限）, 处理函数 }。
   选项表与处理函数在 msta_sdc_clock.c / msta_sdc_io.c / msta_sdc_except.c / msta_sdc_env.c。 */
static const MstaSdcCmdDef s_vSdcCommands[] = {
    { "create_clock",             Msta_SdcCreateClockOpts,         MSTA_SDC_NO_VALUE, 0, -1, Msta_SdcCreateClock            },
    { "create_generated_clock",   Msta_SdcGeneratedClockOpts,      MSTA_SDC_NO_VALUE, 1,  1, Msta_SdcCreateGeneratedClock   },
    { "set_clock_uncertainty",    Msta_SdcClockUncertaintyOpts,    MSTA_SDC_NUMBER,   0, -1, Msta_SdcSetClockUncertainty    },
    { "set_clock_latency",        Msta_SdcClockLatencyOpts,        MSTA_SDC_NUMBER,   0, -1, Msta_SdcSetClockLatency        },
    { "set_propagated_clock",     Msta_SdcNoOpts,                  MSTA_SDC_NO_VALUE, 1, -1, Msta_SdcSetPropagatedClock     },
    { "set_clock_transition",     Msta_SdcMinMaxRiseFallOpts,      MSTA_SDC_NUMBER,   1, -1, Msta_SdcSetClockTransition     },
    { "set_clock_sense",          Msta_SdcClockSenseOpts,          MSTA_SDC_NO_VALUE, 1, -1, Msta_SdcSetClockSense          },
    { "set_clock_groups",         Msta_SdcClockGroupsOpts,         MSTA_SDC_NO_VALUE, 0,  0, Msta_SdcSetClockGroups         },
    { "set_input_delay",          Msta_SdcPortDelayOpts,           MSTA_SDC_NUMBER,   1, -1, Msta_SdcSetInputDelay          },
    { "set_output_delay",         Msta_SdcPortDelayOpts,           MSTA_SDC_NUMBER,   1, -1, Msta_SdcSetOutputDelay         },
    { "set_load",                 Msta_SdcLoadOpts,                MSTA_SDC_NUMBER,   1, -1, Msta_SdcSetLoad                },
    { "set_input_transition",     Msta_SdcInputTransitionOpts,     MSTA_SDC_NUMBER,   1, -1, Msta_SdcSetInputTransition     },
    { "set_driving_cell",         Msta_SdcDrivingCellOpts,         MSTA_SDC_NO_VALUE, 1, -1, Msta_SdcSetDrivingCell         },
    { "set_ideal_network",        Msta_SdcIdealNetworkOpts,        MSTA_SDC_NO_VALUE, 1, -1, Msta_SdcSetIdealNetwork        },
    { "set_ideal_latency",        Msta_SdcMinMaxRiseFallOpts,      MSTA_SDC_NUMBER,   1, -1, Msta_SdcSetIdealLatency        },
    { "set_ideal_transition",     Msta_SdcMinMaxRiseFallOpts,      MSTA_SDC_NUMBER,   1, -1, Msta_SdcSetIdealTransition     },
    { "set_case_analysis",        Msta_SdcNoOpts,                  MSTA_SDC_WORD,     1, -1, Msta_SdcSetCaseAnalysis        },
    { "set_logic_zero",           Msta_SdcNoOpts,                  MSTA_SDC_NO_VALUE, 1, -1, Msta_SdcSetLogicZero           },
    { "set_logic_one",            Msta_SdcNoOpts,                  MSTA_SDC_NO_VALUE, 1, -1, Msta_SdcSetLogicOne            },
    { "set_logic_dc",             Msta_SdcNoOpts,                  MSTA_SDC_NO_VALUE, 1, -1, Msta_SdcSetLogicDc             },
    { "set_disable_timing",       Msta_SdcDisableTimingOpts,       MSTA_SDC_NO_VALUE, 1, -1, Msta_SdcSetDisableTiming       },
    { "set_false_path",           Msta_SdcPathExceptionOpts,       MSTA_SDC_NO_VALUE, 0,  0, Msta_SdcSetFalsePath           },
    { "set_multicycle_path",      Msta_SdcPathExceptionOpts,       MSTA_SDC_NUMBER,   0,  0, Msta_SdcSetMulticyclePath      },
    { "set_max_delay",            Msta_SdcPathDelayOpts,           MSTA_SDC_NUMBER,   0,  0, Msta_SdcSetMaxDelay            },
    { "set_min_delay",            Msta_SdcPathDelayOpts,           MSTA_SDC_NUMBER,   0,  0, Msta_SdcSetMinDelay            },
    { "group_path",               Msta_SdcGroupPathOpts,           MSTA_SDC_NO_VALUE, 0,  0, Msta_SdcSetGroupPath           },
    { "set_data_check",           Msta_SdcDataCheckOpts,           MSTA_SDC_NUMBER,   0,  0, Msta_SdcSetDataCheck           },
    { "set_clock_gating_check",   Msta_SdcClockGatingCheckOpts,    MSTA_SDC_NO_VALUE, 0, -1, Msta_SdcSetClockGatingCheck    },
    { "set_max_time_borrow",      Msta_SdcNoOpts,                  MSTA_SDC_NUMBER,   0, -1, Msta_SdcSetMaxTimeBorrow       },
    { "set_timing_derate",        Msta_SdcTimingDerateOpts,        MSTA_SDC_NUMBER,   0, -1, Msta_SdcSetTimingDerate        },
    { "set_max_transition",       Msta_SdcDrcLimitOpts,            MSTA_SDC_NUMBER,   1, -1, Msta_SdcSetMaxTransition       },
    { "set_max_fanout",           Msta_SdcDrcLimitOpts,            MSTA_SDC_NUMBER,   1, -1, Msta_SdcSetMaxFanout           },
    { "set_max_capacitance",      Msta_SdcDrcLimitOpts,            MSTA_SDC_NUMBER,   1, -1, Msta_SdcSetMaxCapacitance      },
    { "set_min_capacitance",      Msta_SdcDrcLimitOpts,            MSTA_SDC_NUMBER,   1, -1, Msta_SdcSetMinCapacitance      },
    { "set_max_area",             Msta_SdcNoOpts,                  MSTA_SDC_NUMBER,   0,  0, Msta_SdcSetMaxArea             },
    { "set_operating_conditions", Msta_SdcOperatingConditionsOpts, MSTA_SDC_NO_VALUE, 0,  1, Msta_SdcSetOperatingConditions },
    { "set_voltage",              Msta_SdcVoltageOpts,             MSTA_SDC_NUMBER,   0, -1, Msta_SdcSetVoltage             },
    { "set_units",                Msta_SdcUnitsOpts,               MSTA_SDC_NO_VALUE, 0,  0, Msta_SdcSetUnits               },
    { NULL,                       NULL,                            MSTA_SDC_NO_VALUE, 0,  0, NULL                           } };

/* 执行一条已经展开好的命令（argv[0] 是命令名，pArg[i] 是 argv[i] 来自第几个
   Tcl 参数）。返回 1 表示交给了处理函数，0 表示这条命令被忽略（已告警并计数）。 */
static int Msta_SdcRunOne( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, int argc, char **argv,
                           const int *pArg )
{
    const char *pCmd = argv[0];
    int i;

    /* 分发表里的命令：先按选项表解析，再交给处理函数。 */
    for ( i = 0; s_vSdcCommands[i].pName; i++ )
        if ( !strcmp( pCmd, s_vSdcCommands[i].pName ) )
        {
            MstaSdcCmd Cmd;
            p->nCommandsRead++;
            if ( !Msta_SdcParseCmd( p, &s_vSdcCommands[i], argc, argv, pArg, &Cmd ) )
                return 0;                       /* 语法有错：已告警并计入忽略数 */
            s_vSdcCommands[i].pfHandler( p, pDes, pLib, &Cmd );
            return 1;
        }
    /* 认得但不建模的命令：说明原因后丢掉这一条。 */
    for ( i = 0; s_vIgnoredCommands[i].pName; i++ )
        if ( !strcmp( pCmd, s_vIgnoredCommands[i].pName ) )
        {
            Msta_WarnOnce( "sdc command \"%s\" is not modeled by msta: %s (ignored)",
                           pCmd, s_vIgnoredCommands[i].pNote );
            p->nCommandsIgnored++;
            return 0;
        }
    Msta_WarnOnce( "unknown sdc command \"%s\" (ignored)", pCmd );
    p->nCommandsIgnored++;
    return 0;
}

/* =====================================================================
   生命周期
   ===================================================================== */

MstaSdc *Msta_SdcStart( void )
{
    MstaSdc *p = (MstaSdc *)calloc( 1, sizeof(MstaSdc) );
    assert( p );
    MstaClockArrayInit( &p->vClocks );
    Msta_IntMapInit( &p->clockMap, 256 );
    MstaNetConsArrayInit( &p->vNets );
    MstaIoDelayArrayInit( &p->vInputDelays );
    MstaIoDelayArrayInit( &p->vOutputDelays );
    MstaClockSenseArrayInit( &p->vClockSense );
    MstaDataCheckArrayInit( &p->vDataChecks );
    MstaDisabledArcArrayInit( &p->vDisabledArcs );
    Msta_IntMapInit( &p->netConsMap, 4096 );
    MstaExceptionArrayInit( &p->vExceptions );
    MstaInterClockUncArrayInit( &p->vInterClockUnc );
    p->MaxTransition = p->MaxCapacitance = p->MaxFanout = MSTA_UNSET;
    p->MinCapacitance = p->MaxArea = MSTA_UNSET;
    p->OpCondMax = p->OpCondMin = MSTA_NO_ID;
    p->OpCondLibraryMax = p->OpCondLibraryMin = MSTA_NO_ID;
    p->VoltageMax = p->VoltageMin = MSTA_UNSET;
    p->TempMax = p->TempMin = MSTA_UNSET;
    p->KFactorDerateLate = p->KFactorDerateEarly = 1.0;
    p->DerateEarly = p->DerateLate = 1.0;
    p->DerateCheckEarly = p->DerateCheckLate = 1.0;
    /* 0 = 还没定：真正读 SDC 时按库的时间单位兜底。 */
    p->TimeScalePs = 0.0;
    return p;
}

void Msta_SdcFree( MstaSdc *p )
{
    if ( p == NULL )
        return;
    MstaClockArrayFree( &p->vClocks );
    Msta_IntMapFree( &p->clockMap );
    MstaNetConsArrayFree( &p->vNets );
    MstaIoDelayArrayFree( &p->vInputDelays );
    MstaIoDelayArrayFree( &p->vOutputDelays );
    MstaClockSenseArrayFree( &p->vClockSense );
    MstaDataCheckArrayFree( &p->vDataChecks );
    MstaDisabledArcArrayFree( &p->vDisabledArcs );
    Msta_IntMapFree( &p->netConsMap );
    MstaExceptionArrayFree( &p->vExceptions );
    Msta_SdcExIndexRelease( p );
    MstaPathGroupArrayFree( &p->vPathGroups );
    MstaClockGatingSdcArrayFree( &p->vClockGating );
    MstaBorrowSdcArrayFree( &p->vBorrow );
    MstaObjDerateArrayFree( &p->vObjDerate );
    MstaInterClockUncArrayFree( &p->vInterClockUnc );
    free( p );
}
