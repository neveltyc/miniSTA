/**CFile***************************************************************

  FileName    [msta_sdc.c]

  Synopsis    [SDC 子集的读入与查询。]

  Tcl 前端负责 SDC 语法、变量、集合和 source；这里接收类型化的命令参数，
  解析成约束对象，再由时序引擎应用。
  未建模的命令记录告警并计入忽略计数。

***********************************************************************/

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>
#include <sys/wait.h>
#include "msta_sdc.h"
#include "msta_lib.h"
#include "msta_util.h"
#include "msta_json.h"

/* 与 scripts/sdc_bridge.tcl 的集合类型标记保持一致。
   小写类型 = 该集合用了 -quiet（方言写法，即其他工具支持、SDC 1.8 里没有的非标准写法），
   压掉"没匹配到对象"的提示。 */
typedef struct {
    char **ppText;
    char  *pKinds;
    char  *pQuiet;
    int    nSize;
    int    nCap;
} MstaSdcArgList;

MstaArrayDefine( int, MstaSdcIntArray )

static MstaSdcArgList s_Args;
static int  *s_pArgHash;
static int   s_nArgHashCap;
static void **s_ppArena;
static int    s_nArenaUsed, s_nArenaCap;
static char s_pBridgePath[4096] = "scripts/sdc_bridge.tcl";
static int Msta_SdcIsNumber( const char *pText );
static int Msta_SdcIsQuiet( const char *pText );
static void Msta_SdcExIndexRelease( MstaSdc *p );

static void *Msta_SdcArenaKeep( void *pMem )
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

static char *Msta_SdcArena( const char *pFormat, ... )
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

static char **Msta_SdcArgBuffer( int argc )
{
    char **ppBuf = (char **)calloc( (size_t)argc + 1, sizeof(char *) );
    assert( ppBuf );
    return (char **)Msta_SdcArenaKeep( ppBuf );
}

static void Msta_SdcArgListPush( MstaSdcArgList *pL, char *pText, char Kind, char fQuiet )
{
    if ( pL->nSize == pL->nCap )
    {
        pL->nCap = pL->nCap ? 2 * pL->nCap : 64;
        pL->ppText = (char **)realloc( pL->ppText, (size_t)pL->nCap * sizeof(char *) );
        pL->pKinds = (char *)realloc( pL->pKinds, (size_t)pL->nCap );
        pL->pQuiet = (char *)realloc( pL->pQuiet, (size_t)pL->nCap );
        assert( pL->ppText && pL->pKinds && pL->pQuiet );
    }
    pL->ppText[pL->nSize] = pText;
    pL->pKinds[pL->nSize] = Kind;
    pL->pQuiet[pL->nSize] = fQuiet;
    pL->nSize++;
}

static void Msta_SdcArgListFree( MstaSdcArgList *pL )
{
    free( pL->ppText );
    free( pL->pKinds );
    free( pL->pQuiet );
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

/* Tcl 的 * / ? 通配；总线名里的方括号按普通字符处理。 */
static int Msta_SdcGlobMatch( const char *pPattern, const char *pText )
{
    const char *pStar = NULL, *pRetry = NULL;
    while ( *pText )
    {
        if ( *pPattern == '*' )
        { pStar = ++pPattern; pRetry = pText; }
        else if ( *pPattern == '?' || *pPattern == *pText )
        { pPattern++; pText++; }
        else if ( pStar )
        { pPattern = pStar; pText = ++pRetry; }
        else return 0;
    }
    while ( *pPattern == '*' ) pPattern++;
    return *pPattern == 0;
}

/* 取 Tcl 集合参数记录的对象类型。 */
static char Msta_SdcKindOf( const char *pText )
{
    int i = Msta_SdcArgIndex( pText );
    return i >= 0 ? s_Args.pKinds[i] : 0;
}

/* =====================================================================
   参数小工具
   ===================================================================== */

/* argv 里位置 i 是否被某个 flag 用掉了。used[0] 不用（那是命令名）。 */
typedef struct { int *Used; } MstaSdcArgs;

static void Msta_SdcArgsStart( MstaSdcArgs *pA, int argc )
{
    pA->Used = (int *)calloc( (size_t)argc + 1, sizeof(int) );
    assert( pA->Used );
    Msta_SdcArenaKeep( pA->Used );
}

/* 取并消费某个选项后面的值。 */
static const char *Msta_SdcValueOf( int argc, char **argv, MstaSdcArgs *pA, const char *pFlag )
{
    int i;
    for ( i = 1; i + 1 < argc; i++ )
        if ( !strcmp( argv[i], pFlag ) &&
             ( argv[i+1][0] != '-' || Msta_SdcIsNumber(argv[i+1]) ) )
        {
            pA->Used[i] = 1;
            pA->Used[i+1] = 1;
            return argv[i+1];
        }
    return NULL;
}

/* 解析 SDC 的列表写法：-edges {1 3 5} 传到 C 侧就是一段 "1 3 5" 文本。
   花括号、逗号、空格都当分隔符；碰到不是数字的地方停下。返回取到的个数。 */
static int Msta_SdcParseNumberList( const char *pText, double *pOut, int nCap )
{
    const char *p = pText;
    int n = 0;
    while ( p != NULL && *p != 0 && n < nCap )
    {
        char *pEnd;
        double Value;
        while ( *p == ' ' || *p == '\t' || *p == '{' || *p == '}' || *p == ',' ) p++;
        if ( *p == 0 ) break;
        Value = strtod( p, &pEnd );
        if ( pEnd == p ) break;
        pOut[n++] = Value;
        p = pEnd;
    }
    return n;
}

/* 时钟的第 n 个边沿（n 从 1 开始数）：1 = 首个上升沿，2 = 首个下降沿，
   3 = 第二个上升沿…… 返回值是相对时钟源的时间，ps。 */
static double Msta_SdcClockEdgeTime( MstaClock *pClock, int nEdge )
{
    int n = nEdge - 1;
    double Phase = ( n % 2 == 0 ) ? pClock->RiseEdge : pClock->FallEdge;
    return Phase + (double)( n / 2 ) * pClock->Period;
}

/* 标记某个开关选项的所有出现，返回是否出现过。 */
static int Msta_SdcTakeFlag( int argc, char **argv, MstaSdcArgs *pA, const char *pFlag )
{
    int i, fFound = 0;
    for ( i = 1; i < argc; i++ )
        if ( !strcmp( argv[i], pFlag ) )
        {
            pA->Used[i] = 1;
            fFound = 1;
        }
    return fFound;
}

/* 没被 flag 吃掉的参数。负数也是合法的 SDC 时间值。 */
static int Msta_SdcRest( int argc, char **argv, MstaSdcArgs *pA, char **ppOut, int nMax )
{
    int i, n = 0;
    for ( i = 1; i < argc; i++ )
        if ( !pA->Used[i] && ( argv[i][0] != '-' || Msta_SdcIsNumber(argv[i]) ) &&
             argv[i][0] != 0 && n < nMax )
            ppOut[n++] = argv[i];
    return n;
}

/* SDC 时间默认取库单位（set_units 可覆盖），内部一律 ps。 */
static double Msta_SdcToPs( MstaSdc *p, const char *pText )
{
    return atof( pText ) * p->TimeScalePs;
}

/* 判断一个词是不是完整的浮点数。 */
static int Msta_SdcIsNumber( const char *pText )
{
    char *pEnd;
    if ( pText == NULL || pText[0] == 0 )
        return 0;
    (void)strtod( pText, &pEnd );
    return pEnd != pText && *pEnd == 0;
}

/* 把时间/电容单位文本换算成内部比例。 */
static double Msta_SdcUnitScale( const char *pText, int fTime )
{
    char *pEnd;
    double Value = strtod(pText,&pEnd);
    if ( pEnd == pText ) { Value = 1.0; pEnd = (char *)pText; }
    if ( Value <= 0.0 ) return 0.0;
    if ( fTime )
    {
        if ( !strcasecmp(pEnd,"fs") ) return Value * 0.001;
        if ( !strcasecmp(pEnd,"ps") ) return Value;
        if ( !strcasecmp(pEnd,"ns") ) return Value * 1000.0;
        if ( !strcasecmp(pEnd,"us") ) return Value * 1000000.0;
    }
    else
    {
        if ( !strcasecmp(pEnd,"af") ) return Value * 0.001;
        if ( !strcasecmp(pEnd,"ff") ) return Value;
        if ( !strcasecmp(pEnd,"pf") ) return Value * 1000.0;
        if ( !strcasecmp(pEnd,"nf") ) return Value * 1000000.0;
    }
    return 0.0;
}

/* =====================================================================
   选项表与通用解析器

   每条命令的语法写成一张选项表（MstaSdcOpt 数组）：选项名 + 种类。
   通用解析器 Msta_SdcParseCmd 按表把 argv 拆成一个 MstaSdcCmd：
   哪些开关出现了、各选项取到了什么值、剩下哪些位置参数（数值、对象名）。
   处理函数只通过 Msta_SdcHasFlag / Msta_SdcOptValue 这类访问函数和
   ppPos 位置参数数组取结果，不再自己遍历 argv。
   ===================================================================== */

/* 选项的种类：决定解析器怎样认出这个选项、怎样取它的值。 */
typedef enum {
    MSTA_SDC_FLAG,         /* 开关，如 -add：出现就算数 */
    MSTA_SDC_VALUE,        /* 带一个值，如 -period 5。值不能长得像选项（以 - 开头的
                              非数字），否则这次出现不算；写了多次时只认第一次取到值的 */
    MSTA_SDC_VALUE_RAW,    /* 带一个值，值可以以 - 开头，如 -edge_shift {-0.1 0 0}；
                              只认第一次出现 */
    MSTA_SDC_FLAG_NUMBER,  /* 开关，后面紧跟数值时数值归它，如 -setup 0.1（方言写法）；
                              带值写了多次时取最后一个 */
    MSTA_SDC_RF_VALUE,     /* 带一个值，并且认 -rise_/-fall_ 前缀的变体：表里写 -from，
                              -rise_from / -fall_from 也算它，同时记下边沿 'r' / 'f' */
    MSTA_SDC_GROUP         /* 可以重复的对象组，如 -group：每出现一次开一个新组，
                              后面的对象名都归入这个组（只用于顺序模式） */
} MstaSdcOptKind;

/* 选项表的一行。表以 pName == NULL 的一行结尾。 */
typedef struct {
    const char     *pName;
    MstaSdcOptKind  Kind;
} MstaSdcOpt;

/* 解析模式：同一种选项表，有三种读 argv 的方式。 */
typedef enum {
    MSTA_SDC_SCAN,         /* 扫描：每个选项各自在整条命令里找自己，剩下的数值和
                              对象名就是位置参数，所以数值可以写在选项之间任意位置 */
    MSTA_SDC_ORDERED,      /* 顺序：从左到右读，以 - 开头的词都当选项，选项的值就是
                              紧跟的下一个词；遇到第一个认不出的选项就停下 */
    MSTA_SDC_WORDS         /* 没有选项：每个词（包括以 - 开头的词和空串）都是位置参数 */
} MstaSdcParseMode;

/* 解析结果。下标为 k 的数组对应选项表的第 k 行；数组都挂在本条命令的
   临时内存上（读下一条命令时一起释放）。 */
typedef struct {
    const MstaSdcOpt *pOpts;   /* 这条命令的选项表 */
    int           argc;
    char        **argv;        /* argv[0] 是命令名 */
    int          *pUsed;       /* 扫描模式：argv[i] 已被某个选项认领 */
    int          *pAt;         /* 选项第一次生效的位置（argv 下标），0 = 没出现 */
    const char  **ppValue;     /* 选项取到的值，NULL = 没取到 */
    char         *pEdge;       /* MSTA_SDC_RF_VALUE 选项的边沿：'r' / 'f' / 0 */
    char        **ppPos;       /* 位置参数，按出现顺序 */
    int          *pPosAt;      /* 位置参数在 argv 里的下标 */
    int           nPos;
    char        **ppMember;    /* MSTA_SDC_GROUP 组里的对象名，按出现顺序 */
    int          *pMemberAt;   /* 它们在 argv 里的下标 */
    int          *pMemberGroup;/* 它们属于第几组（从 1 数） */
    int           nMembers;
    int           nGroups;     /* 一共开了几个组 */
    int           iBad;        /* 第一个"长得像选项却没被认领"的词的下标：不认识的
                                  选项，或没取到值的选项。0 = 没有。要不要为此告警
                                  由各条命令自己决定 */
} MstaSdcCmd;

/* 命令的处理函数：拿解析结果做语义——查对象、换算单位、写模型。 */
typedef void (*MstaSdcHandler)( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd );

/* 分发表的一行：命令名、解析模式、选项表、处理函数。 */
typedef struct {
    const char       *pName;
    MstaSdcParseMode  Mode;
    const MstaSdcOpt *pOpts;
    MstaSdcHandler    pfHandler;
} MstaSdcCmdDef;

/* 在本条命令的临时内存上分配 n 个清零的元素（多给一个，n 为 0 也能用）。 */
static void *Msta_SdcArenaZeros( int n, size_t nSize )
{
    void *pMem = calloc( (size_t)n + 1, nSize );
    assert( pMem );
    return Msta_SdcArenaKeep( pMem );
}

/* 以 - 开头又不是数值的词，才算"长得像选项"；-0.5 这样的负数是普通的值。 */
static int Msta_SdcLooksLikeOption( const char *pWord )
{
    return pWord[0] == '-' && !Msta_SdcIsNumber( pWord );
}

/* pWord 是不是选项 pOpt：RF_VALUE 选项还认 -rise_/-fall_ 前缀的变体，
   边沿写进 *pEdge（不带前缀写 0）。 */
static int Msta_SdcWordIsOpt( const char *pWord, const MstaSdcOpt *pOpt, char *pEdge )
{
    *pEdge = 0;
    if ( !strcmp( pWord, pOpt->pName ) )
        return 1;
    if ( pOpt->Kind != MSTA_SDC_RF_VALUE )
        return 0;
    /* 选项名去掉开头的 '-' 再接到前缀后面：-from -> -rise_from */
    if ( !strncmp( pWord, "-rise_", 6 ) && !strcmp( pWord + 6, pOpt->pName + 1 ) )
    { *pEdge = 'r'; return 1; }
    if ( !strncmp( pWord, "-fall_", 6 ) && !strcmp( pWord + 6, pOpt->pName + 1 ) )
    { *pEdge = 'f'; return 1; }
    return 0;
}

/* 记下选项 k 在下标 i 处生效（只记第一次的位置）。 */
static void Msta_SdcCmdSeen( MstaSdcCmd *pCmd, int k, int i )
{
    if ( pCmd->pAt[k] == 0 )
        pCmd->pAt[k] = i;
}

/* 扫描模式。每个选项各自在整条 argv 里找自己并把用到的词标为"已认领"；
   最后既没被认领、又不像选项的词就是位置参数。 */
static void Msta_SdcParseScan( MstaSdcCmd *pCmd )
{
    int argc = pCmd->argc, i, k;
    char **argv = pCmd->argv;
    char Edge;
    for ( k = 0; pCmd->pOpts[k].pName; k++ )
    {
        const char *pName = pCmd->pOpts[k].pName;
        MstaSdcOptKind Kind = pCmd->pOpts[k].Kind;
        if ( Kind == MSTA_SDC_FLAG || Kind == MSTA_SDC_FLAG_NUMBER )
        {
            for ( i = 1; i < argc; i++ )
                if ( !strcmp( argv[i], pName ) )
                {
                    pCmd->pUsed[i] = 1;
                    Msta_SdcCmdSeen( pCmd, k, i );
                }
            /* 带数值的开关：每个后面紧跟数值的出现都把数值记下，最后一个留下。 */
            for ( i = 1; Kind == MSTA_SDC_FLAG_NUMBER && i + 1 < argc; i++ )
                if ( !strcmp( argv[i], pName ) && Msta_SdcIsNumber( argv[i+1] ) )
                {
                    pCmd->ppValue[k] = argv[i+1];
                    pCmd->pUsed[i+1] = 1;
                }
        }
        else if ( Kind == MSTA_SDC_VALUE || Kind == MSTA_SDC_VALUE_RAW )
        {
            for ( i = 1; i + 1 < argc; i++ )
            {
                if ( strcmp( argv[i], pName ) )
                    continue;
                if ( Kind == MSTA_SDC_VALUE && Msta_SdcLooksLikeOption( argv[i+1] ) )
                    continue;
                pCmd->ppValue[k] = argv[i+1];
                pCmd->pUsed[i] = pCmd->pUsed[i+1] = 1;
                Msta_SdcCmdSeen( pCmd, k, i );
                break;
            }
        }
        else
            assert( Kind == MSTA_SDC_RF_VALUE );    /* 下面一起处理；扫描模式没有组 */
    }
    /* RF_VALUE 选项（-from/-to 及其 -rise_/-fall_ 变体）一起从左往右读：紧跟的下一个词
       不管长什么样都是值，读过的值不再当选项看；同一选项写多次时最后一次算数。 */
    for ( i = 1; i + 1 < argc; i++ )
        for ( k = 0; pCmd->pOpts[k].pName; k++ )
        {
            if ( pCmd->pOpts[k].Kind != MSTA_SDC_RF_VALUE ||
                 !Msta_SdcWordIsOpt( argv[i], &pCmd->pOpts[k], &Edge ) )
                continue;
            pCmd->ppValue[k] = argv[i+1];
            pCmd->pEdge[k] = Edge;
            pCmd->pUsed[i] = pCmd->pUsed[i+1] = 1;
            Msta_SdcCmdSeen( pCmd, k, i );
            i++;
            break;
        }
    /* 剩下的词：像选项的记为"没认领"，空串丢掉，其余是位置参数。 */
    for ( i = 1; i < argc; i++ )
    {
        if ( pCmd->pUsed[i] )
            continue;
        if ( Msta_SdcLooksLikeOption( argv[i] ) )
        {
            if ( pCmd->iBad == 0 )
                pCmd->iBad = i;
            continue;
        }
        if ( argv[i][0] == 0 )
            continue;
        pCmd->pPosAt[pCmd->nPos] = i;
        pCmd->ppPos[pCmd->nPos++] = argv[i];
    }
}

/* 顺序模式。从左到右读：选项按种类吃掉自己的值；不认识的、以 - 开头的词
   让解析停下（记在 iBad）；其余的词在出现过组选项之后归入最近的组，
   之前的是位置参数。 */
static void Msta_SdcParseOrdered( MstaSdcCmd *pCmd )
{
    int argc = pCmd->argc, i, k;
    char **argv = pCmd->argv;
    char Edge = 0;
    for ( i = 1; i < argc; i++ )
    {
        for ( k = 0; pCmd->pOpts[k].pName; k++ )
            if ( Msta_SdcWordIsOpt( argv[i], &pCmd->pOpts[k], &Edge ) )
                break;
        if ( pCmd->pOpts[k].pName != NULL )
        {
            MstaSdcOptKind Kind = pCmd->pOpts[k].Kind;
            int fTakesValue = Kind == MSTA_SDC_VALUE || Kind == MSTA_SDC_VALUE_RAW ||
                              Kind == MSTA_SDC_RF_VALUE;
            if ( fTakesValue && i + 1 >= argc )     /* 选项在末尾、没有值 */
            {
                pCmd->iBad = i;
                return;
            }
            Msta_SdcCmdSeen( pCmd, k, i );
            if ( Kind == MSTA_SDC_GROUP )
                pCmd->nGroups++;
            else if ( fTakesValue )
            {
                pCmd->ppValue[k] = argv[++i];        /* 紧跟的下一个词就是值，跳过它 */
                pCmd->pEdge[k] = Edge;
            }
            else if ( Kind == MSTA_SDC_FLAG_NUMBER && i + 1 < argc && Msta_SdcIsNumber( argv[i+1] ) )
                pCmd->ppValue[k] = argv[++i];
            continue;
        }
        if ( argv[i][0] == '-' )
        {
            pCmd->iBad = i;
            return;
        }
        if ( pCmd->nGroups > 0 )
        {
            pCmd->pMemberAt[pCmd->nMembers] = i;
            pCmd->pMemberGroup[pCmd->nMembers] = pCmd->nGroups;
            pCmd->ppMember[pCmd->nMembers++] = argv[i];
        }
        else
        {
            pCmd->pPosAt[pCmd->nPos] = i;
            pCmd->ppPos[pCmd->nPos++] = argv[i];
        }
    }
}

/* 通用解析器：按 pDef 的选项表和模式把 argv 拆进 *pCmd。
   解析本身不告警，所有告警都由处理函数按命令语义给出。 */
static void Msta_SdcParseCmd( const MstaSdcCmdDef *pDef, int argc, char **argv, MstaSdcCmd *pCmd )
{
    int nOpts = 0, i;
    while ( pDef->pOpts[nOpts].pName )
        nOpts++;
    memset( pCmd, 0, sizeof(MstaSdcCmd) );
    pCmd->pOpts        = pDef->pOpts;
    pCmd->argc         = argc;
    pCmd->argv         = argv;
    pCmd->pUsed        = (int *)Msta_SdcArenaZeros( argc, sizeof(int) );
    pCmd->pAt          = (int *)Msta_SdcArenaZeros( nOpts, sizeof(int) );
    pCmd->ppValue      = (const char **)Msta_SdcArenaZeros( nOpts, sizeof(char *) );
    pCmd->pEdge        = (char *)Msta_SdcArenaZeros( nOpts, sizeof(char) );
    pCmd->ppPos        = (char **)Msta_SdcArenaZeros( argc, sizeof(char *) );
    pCmd->pPosAt       = (int *)Msta_SdcArenaZeros( argc, sizeof(int) );
    pCmd->ppMember     = (char **)Msta_SdcArenaZeros( argc, sizeof(char *) );
    pCmd->pMemberAt    = (int *)Msta_SdcArenaZeros( argc, sizeof(int) );
    pCmd->pMemberGroup = (int *)Msta_SdcArenaZeros( argc, sizeof(int) );
    if ( pDef->Mode == MSTA_SDC_SCAN )
        Msta_SdcParseScan( pCmd );
    else if ( pDef->Mode == MSTA_SDC_ORDERED )
        Msta_SdcParseOrdered( pCmd );
    else
        for ( i = 1; i < argc; i++ )
        {
            pCmd->pPosAt[pCmd->nPos] = i;
            pCmd->ppPos[pCmd->nPos++] = argv[i];
        }
}

/* 选项名在表里的下标。名字不在表里说明处理函数和选项表对不上，是代码错误。 */
static int Msta_SdcOptIndex( const MstaSdcCmd *pCmd, const char *pName )
{
    int k;
    for ( k = 0; pCmd->pOpts[k].pName; k++ )
        if ( !strcmp( pCmd->pOpts[k].pName, pName ) )
            return k;
    assert( 0 );
    return 0;
}

/* 选项是否生效过：开关出现过，或带值的选项取到了值。 */
static int Msta_SdcHasFlag( const MstaSdcCmd *pCmd, const char *pName )
{
    return pCmd->pAt[ Msta_SdcOptIndex( pCmd, pName ) ] > 0;
}

/* 选项第一次生效的位置（argv 下标），0 = 没出现。需要按出现顺序处理时用。 */
static int Msta_SdcOptAt( const MstaSdcCmd *pCmd, const char *pName )
{
    return pCmd->pAt[ Msta_SdcOptIndex( pCmd, pName ) ];
}

/* 选项的值，没取到返回 NULL。 */
static const char *Msta_SdcOptValue( const MstaSdcCmd *pCmd, const char *pName )
{
    return pCmd->ppValue[ Msta_SdcOptIndex( pCmd, pName ) ];
}

/* RF_VALUE 选项是用哪种写法给的：-rise_xxx 返回 'r'，-fall_xxx 返回 'f'，否则 0。 */
static char Msta_SdcOptEdge( const MstaSdcCmd *pCmd, const char *pName )
{
    return pCmd->pEdge[ Msta_SdcOptIndex( pCmd, pName ) ];
}

/* 公共子语法：-min/-max 与 -rise/-fall 决定一个值落在哪几个 [角][边沿] 格子里。
   角 0 = min、1 = max；边沿 0 = fall、1 = rise（与 MstaClock::Slew 等数组的下标一致）。
   一对限定都没写等于两个都选：只写 -max 选中 max 的 rise 和 fall 两格，
   什么都不写四格全选。 */
static void Msta_SdcMinMaxRiseFall( int fMin, int fMax, int fRise, int fFall, int Sel[2][2] )
{
    int m, e;
    for ( m = 0; m < 2; m++ )
        for ( e = 0; e < 2; e++ )
            Sel[m][e] = ( m ? ( fMax || !fMin ) : ( fMin || !fMax ) ) &&
                        ( e ? ( fRise || !fFall ) : ( fFall || !fRise ) );
}

/* 把 Value 写进 Target 里被 Sel 选中的格子。 */
static void Msta_SdcStoreMinMaxRiseFall( double Target[2][2], int Sel[2][2], double Value )
{
    int m, e;
    for ( m = 0; m < 2; m++ )
        for ( e = 0; e < 2; e++ )
            if ( Sel[m][e] )
                Target[m][e] = Value;
}

/* 把 set_units 的值用到后面命令的数值上。 */
static void Msta_SdcSetUnits( MstaSdc *p, int argc, char **argv )
{
    MstaSdcArgs A;
    const char *pTime, *pCap;
    double Scale;
    int i;
    Msta_SdcArgsStart( &A, argc );
    pTime = Msta_SdcValueOf(argc,argv,&A,"-time");
    pCap = Msta_SdcValueOf(argc,argv,&A,"-capacitance");
    for ( i = 1; i < argc; i++ )
        if ( argv[i][0] == '-' && strcmp(argv[i],"-time") && strcmp(argv[i],"-capacitance") )
            Msta_WarnOnce("set_units option \"%s\" is not modeled; ignored",argv[i]);
    if ( pTime )
    {
        Scale = Msta_SdcUnitScale(pTime,1);
        if ( Scale > 0.0 ) p->TimeScalePs = Scale;
        else Msta_WarnOnce("set_units: unsupported time unit \"%s\"",pTime);
    }
    if ( pCap )
    {
        Scale = Msta_SdcUnitScale(pCap,0);
        if ( Scale > 0.0 ) p->CapScaleFf = Scale;
        else Msta_WarnOnce("set_units: unsupported capacitance unit \"%s\"",pCap);
    }
}

/* 端口名 / "实例路径/引脚名" / 网络名 -> 一组全局网络号。
   总线端口（如 input [8:0] dma_ack_i）在展平后只有 dma_ack_i[0..8] 这些名字，
   所以这里允许只写基名，自动展开成它的所有位。返回找到的个数。 */
static int Msta_SdcResolveNetsInto( MstaDesign *pDes, const char *pTarget, MstaSdcIntArray *vOut )
{
    int nBeg = vOut->nSize, nNet, i;
    char *pBuf;
    char Kind = Msta_SdcKindOf(pTarget);

    if ( strpbrk(pTarget, "*?") != NULL )
    {
        for ( i = 0; i < pDes->vNets.nSize; i++ )
        {
            MstaNet *pNet = MstaNetArrayAt(&pDes->vNets,i);
            if ( Kind == 'P' && !pNet->fTopPort ) continue;
            if ( !Msta_SdcGlobMatch(pTarget,Msta_NetName(pDes,i)) ) continue;
            *MstaSdcIntArrayAppend( vOut ) = i;
        }
        if ( vOut->nSize == nBeg && !Msta_SdcIsQuiet(pTarget) )
            Msta_WarnOnce("sdc pattern \"%s\" matched no nets",pTarget);
        return vOut->nSize - nBeg;
    }

    nNet = Msta_DesignNetByName( pDes, pTarget );
    if ( nNet >= 0 && ( Kind != 'P' || MstaNetArrayAt(&pDes->vNets,nNet)->fTopPort ) )
    {
        *MstaSdcIntArrayAppend( vOut ) = nNet;
        return 1;
    }
    /* 总线基名：按实际存在的位扫描，允许 [15:8] 等非零起始编号。 */
    for ( i = 0; i < pDes->vNets.nSize; i++ )
    {
        const char *pName = Msta_NetName(pDes,i);
        size_t nBase = strlen(pTarget);
        char *pEnd;
        if ( strncmp(pName,pTarget,nBase) || pName[nBase] != '[' ) continue;
        (void)strtol(pName+nBase+1,&pEnd,10);
        if ( pEnd == pName+nBase+1 || *pEnd != ']' || pEnd[1] != 0 ) continue;
        if ( Kind == 'P' && !MstaNetArrayAt(&pDes->vNets,i)->fTopPort ) continue;
        *MstaSdcIntArrayAppend( vOut ) = i;
    }
    if ( vOut->nSize > nBeg )
        return vOut->nSize - nBeg;
    /* "u_core/u_alu/U7/D"：前缀是实例路径，最后一截是引脚名 */
    pBuf = Msta_SdcArena( "%s", pTarget );
    {
        char *pSlash = strrchr( pBuf, '/' );
        int nInst;
        if ( pSlash == NULL )
        {
            if ( !Msta_SdcIsQuiet(pTarget) )
                Msta_WarnOnce( "sdc target \"%s\" is not a net of this design", pTarget );
            return 0;
        }
        *pSlash = 0;
        nInst = Msta_DesignFindInstByName( pDes, pBuf );
        if ( nInst < 0 )
        {
            if ( !Msta_SdcIsQuiet(pTarget) )
                Msta_WarnOnce( "sdc target \"%s\" is neither a net nor an instance pin", pTarget );
            return 0;
        }
        nNet = Msta_DesignInstPinNet( pDes, nInst, pSlash + 1 );
        if ( nNet < 0 )
        {
            Msta_WarnOnce( "instance \"%s\" has no pin \"%s\"", pBuf, pSlash + 1 );
            return 0;
        }
        *MstaSdcIntArrayAppend( vOut ) = nNet;
    }
    return 1;
}

static int Msta_SdcResolveNets( MstaDesign *pDes, const char *pTarget, int **ppNets )
{
    MstaSdcIntArray vNets;
    MstaSdcIntArrayInit( &vNets );
    Msta_SdcResolveNetsInto( pDes, pTarget, &vNets );
    *ppNets = (int *)Msta_SdcArenaKeep( vNets.pData );
    return vNets.nSize;
}


/* 分配并初始化一条时钟记录。 */
static MstaClock *Msta_SdcNewClock( MstaSdc *p, const char *pName )
{
    MstaClock *pClock = MstaClockArrayAppend( &p->vClocks );
    pClock->Name = Msta_NameId( pName );
    pClock->Period = 0.0;
    pClock->UncertaintySetup = 0.0;
    pClock->UncertaintyHold  = 0.0;
    pClock->RiseEdge = 0.0;
    pClock->FallEdge = 0.0;
    pClock->fPropagated = 1; /* 默认沿 Liberty 时序弧传播时钟。 */
    pClock->MasterClock = MSTA_NO_ID;
    pClock->SourceNet  = MSTA_NO_ID;
    pClock->SourceText = MSTA_NO_ID;
    pClock->Slew[0][0] = pClock->Slew[0][1] = MSTA_UNSET;
    pClock->Slew[1][0] = pClock->Slew[1][1] = MSTA_UNSET;
    Msta_IntMapSet( &p->clockMap, pClock->Name, p->vClocks.nSize - 1 );
    return pClock;
}

/* =====================================================================
   各条命令
   ===================================================================== */

/* create_clock [-name 名字] -period 周期 [-waveform {上升 下降}] [-add] [源端口]
   -period 的 "多周期波形"（{4 8}）写法不支持；只认标量。 */
static const MstaSdcOpt s_vCreateClockOpts[] = {
    { "-name",     MSTA_SDC_VALUE },
    { "-period",   MSTA_SDC_VALUE },
    { "-waveform", MSTA_SDC_VALUE },
    { "-add",      MSTA_SDC_FLAG  },
    { NULL,        MSTA_SDC_FLAG  } };

static void Msta_SdcCreateClock( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    const char *pPeriod = Msta_SdcOptValue( pCmd, "-period" );
    const char *pName   = Msta_SdcOptValue( pCmd, "-name" );
    const char *pWave   = Msta_SdcOptValue( pCmd, "-waveform" );
    int fAdd = Msta_SdcHasFlag( pCmd, "-add" );
    char **pTargets = pCmd->ppPos;          /* 位置参数就是源端口/引脚 */
    int nTargets = pCmd->nPos;
    double Rise = 0.0, Fall = -1.0;
    int nSource = MSTA_NO_ID, i;

    if ( pPeriod == NULL )
    {
        Msta_WarnOnce( "create_clock without -period is ignored" );
        return;
    }
    if ( !Msta_SdcIsNumber(pPeriod) || Msta_SdcToPs(p,pPeriod) <= 0.0 )
    {
        Msta_WarnOnce( "create_clock needs a positive numeric -period" );
        return;
    }
    if ( pWave )
    {
        char Extra;
        if ( sscanf(pWave,"%lf %lf %c",&Rise,&Fall,&Extra) != 2 )
        { Msta_WarnOnce("create_clock: -waveform needs exactly two edge times"); return; }
    }
    if ( pWave && (Rise < 0.0 || Fall <= Rise || Fall >= atof(pPeriod)) )
    {
        Msta_WarnOnce("create_clock: waveform must satisfy 0 <= rise < fall < period");
        return;
    }
    if ( nTargets > 1 )
    { Msta_WarnOnce("create_clock with multiple source pins is not modeled; clock rejected"); return; }
    if ( pName == NULL && nTargets == 0 )
    {
        Msta_WarnOnce( "create_clock needs either -name <c> or a port" );
        return;
    }
    if ( Msta_SdcFindClock(p,pName ? pName : pTargets[0]) )
    {
        Msta_WarnOnce("create_clock: duplicate clock name \"%s\" is ignored",
                      pName ? pName : pTargets[0]);
        return;
    }
    if ( nTargets == 1 )
    {
        int *pNets;
        int nCount = Msta_SdcResolveNets( pDes, pTargets[0], &pNets );
        if ( nCount != 1 )
        { Msta_WarnOnce("create_clock source must resolve to one net"); return; }
        nSource = pNets[0];
        for ( i = 0; i < p->vClocks.nSize; i++ )
            if ( Msta_SdcClockByIndex(p,i)->SourceNet == nSource )
            {
                /* -add 之外的写法是"换掉这个源脚上的时钟"，msta 不做替换。 */
                if ( !fAdd )
                {
                    Msta_WarnOnce("create_clock on a source that already has a clock "
                                  "needs -add; clock rejected");
                    return;
                }
                break;
            }
    }
    /* 不写 -name 时，端口名就是时钟名（SDC 惯例）。 */
    {
        MstaClock *pClock = Msta_SdcNewClock( p, pName ? pName : pTargets[0] );
        pClock->Period = Msta_SdcToPs( p,pPeriod );
        pClock->RiseEdge = Rise * p->TimeScalePs;
        pClock->FallEdge = pWave ? Fall * p->TimeScalePs : 0.5 * pClock->Period;
        if ( nTargets > 0 )
        {
            pClock->SourceNet  = nSource;
            pClock->SourceText = Msta_NameId( pTargets[0] );
        }
    }
}

/* create_generated_clock [-name 名字] -source 主时钟源 [-master_clock 主时钟]
       [-divide_by n | -multiply_by n] [-duty_cycle d] [-invert]
       [-edges {e1 e2 e3} [-edge_shift {s1 s2 s3}]] 目标引脚
   创建支持分频、倍频和反相的生成时钟。-add / -combinational 认得但不建模。 */
static const MstaSdcOpt s_vGeneratedClockOpts[] = {
    { "-name",          MSTA_SDC_VALUE     },
    { "-source",        MSTA_SDC_VALUE     },
    { "-master_clock",  MSTA_SDC_VALUE     },
    { "-divide_by",     MSTA_SDC_VALUE     },
    { "-multiply_by",   MSTA_SDC_VALUE     },
    { "-duty_cycle",    MSTA_SDC_VALUE     },
    { "-edges",         MSTA_SDC_VALUE_RAW },
    { "-edge_shift",    MSTA_SDC_VALUE_RAW },   /* 值常以负数开头，如 {-0.1 0 0} */
    { "-invert",        MSTA_SDC_FLAG      },
    { "-add",           MSTA_SDC_FLAG      },
    { "-combinational", MSTA_SDC_FLAG      },
    { NULL,             MSTA_SDC_FLAG      } };

static void Msta_SdcCreateGeneratedClock( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    const char *pName       = Msta_SdcOptValue( pCmd, "-name" );
    const char *pSource     = Msta_SdcOptValue( pCmd, "-source" );
    const char *pMasterName = Msta_SdcOptValue( pCmd, "-master_clock" );
    const char *pDivide     = Msta_SdcOptValue( pCmd, "-divide_by" );
    const char *pMultiply   = Msta_SdcOptValue( pCmd, "-multiply_by" );
    const char *pDuty       = Msta_SdcOptValue( pCmd, "-duty_cycle" );
    const char *pEdges      = Msta_SdcOptValue( pCmd, "-edges" );
    const char *pShifts     = Msta_SdcOptValue( pCmd, "-edge_shift" );
    int fInvert = Msta_SdcHasFlag( pCmd, "-invert" );
    char **pTargets = pCmd->ppPos;          /* 位置参数就是目标引脚/端口 */
    int nTargets = pCmd->nPos;
    MstaClock *pMaster, *pClock;
    int *pNets, nCount, i, fEdges = 0;
    double Div = 1.0, Mult = 1.0, Duty = 50.0;
    double vEdges[3], vShifts[3];
    if ( Msta_SdcHasFlag( pCmd, "-add" ) )
    { Msta_WarnOnce("create_generated_clock: -add is not modeled; clock rejected"); return; }
    if ( Msta_SdcHasFlag( pCmd, "-combinational" ) )
    { Msta_WarnOnce("create_generated_clock: -combinational is not modeled; clock rejected"); return; }
    if ( pSource == NULL || nTargets != 1 )
    { Msta_WarnOnce("create_generated_clock needs -source and one target pin/port"); return; }
    if ( pDivide && pMultiply )
    { Msta_WarnOnce("create_generated_clock cannot combine -divide_by and -multiply_by"); return; }
    if ( pEdges != NULL && ( pDivide || pMultiply || pDuty != NULL ) )
    {
        Msta_WarnOnce("create_generated_clock: -edges cannot combine with -divide_by/-multiply_by/-duty_cycle");
        return;
    }
    if ( pDivide ) Div = atof(pDivide);
    if ( pMultiply ) Mult = atof(pMultiply);
    if ( pDuty ) Duty = atof(pDuty);
    if ( Div <= 0.0 || Mult <= 0.0 || Div > 1000000.0 || Mult > 1000000.0 ||
         Div != (double)(int)Div ||
         Mult != (double)(int)Mult || Duty <= 0.0 || Duty >= 100.0 )
    { Msta_WarnOnce("create_generated_clock needs positive ratio and 0 < duty_cycle < 100"); return; }
    /* -edges {e1 e2 e3}：用主时钟的第 e1/e2/e3 个边沿定义新时钟的
       上升沿、下降沿和下一个上升沿。边号从 1 开始数：1 = 首个上升沿，
       2 = 首个下降沿，3 = 第二个上升沿……。 */
    if ( pEdges != NULL )
    {
        int nS;
        if ( Msta_SdcParseNumberList( pEdges, vEdges, 3 ) != 3 )
        { Msta_WarnOnce("create_generated_clock -edges needs exactly three edge numbers"); return; }
        nS = pShifts ? Msta_SdcParseNumberList( pShifts, vShifts, 3 ) : 0;
        if ( pShifts != NULL && nS != 3 )
        { Msta_WarnOnce("create_generated_clock: -edge_shift needs as many values as -edges"); return; }
        for ( i = 0; i < 3; i++ )
        {
            if ( vEdges[i] < 1.0 || vEdges[i] != (double)(int)vEdges[i] )
            { Msta_WarnOnce("create_generated_clock: -edges values must be positive integers"); return; }
            if ( i > 0 && vEdges[i] <= vEdges[i-1] )
            { Msta_WarnOnce("create_generated_clock: -edges values must increase"); return; }
        }
        fEdges = 1;
    }
    pMaster = pMasterName ? Msta_SdcFindClock(p,pMasterName) : NULL;
    if ( pMasterName && pMaster == NULL )
    { Msta_WarnOnce("create_generated_clock: unknown master clock \"%s\"",pMasterName); return; }
    if ( pMaster == NULL )
    {
        nCount = Msta_SdcResolveNets( pDes, pSource, &pNets );
        for ( i = 0; i < p->vClocks.nSize && pMaster == NULL; i++ )
            if ( nCount > 0 && Msta_SdcClockByIndex(p,i)->SourceNet == pNets[0] )
                pMaster = Msta_SdcClockByIndex(p,i);
    }
    if ( pMaster == NULL )
    { Msta_WarnOnce("create_generated_clock: no master clock for \"%s\"",pSource); return; }
    nCount = Msta_SdcResolveNets( pDes, pTargets[0], &pNets );
    if ( nCount != 1 )
    { Msta_WarnOnce("create_generated_clock target must resolve to one net"); return; }
    for ( i = 0; i < p->vClocks.nSize; i++ )
        if ( Msta_SdcClockByIndex(p,i)->SourceNet == pNets[0] )
        { Msta_WarnOnce("create_generated_clock target already has a clock; clock rejected"); return; }
    if ( pName == NULL ) pName = pTargets[0];
    if ( Msta_SdcFindClock(p,pName) )
    { Msta_WarnOnce("create_generated_clock: duplicate clock \"%s\"",pName); return; }
    /* Append 可能重新分配 vClocks，先把主时钟的各项值取出来。 */
    {
        double Period = pMaster->Period * Div / Mult;
        double Rise = pMaster->RiseEdge;
        double Fall = Rise + Period * Duty / 100.0;
        MstaId MasterName = pMaster->Name;
        if ( fEdges )
        {
            double Shift[3] = { 0.0, 0.0, 0.0 };
            for ( i = 0; pShifts != NULL && i < 3; i++ )
                Shift[i] = vShifts[i] * p->TimeScalePs;
            Rise = Msta_SdcClockEdgeTime( pMaster, (int)vEdges[0] ) + Shift[0];
            Fall = Msta_SdcClockEdgeTime( pMaster, (int)vEdges[1] ) + Shift[1];
            Period = Msta_SdcClockEdgeTime( pMaster, (int)vEdges[2] ) + Shift[2] - Rise;
            if ( Period <= 0.0 || Fall <= Rise )
            {
                Msta_WarnOnce("create_generated_clock: -edges gives a non-positive period; clock rejected");
                return;
            }
        }
        pClock = Msta_SdcNewClock(p,pName);
        pClock->MasterClock = MasterName;
        pClock->Period = Period;
        pClock->RiseEdge = Rise;
        pClock->FallEdge = Fall;
        if ( fInvert )
        {
            pClock->RiseEdge = pClock->FallEdge;
            pClock->FallEdge = Rise;
        }
        pClock->SourceNet = pNets[0];
        pClock->SourceText = Msta_NameId(pTargets[0]);
    }
}

/* set_clock_uncertainty [-setup] [-hold] [-rise|-fall] 值 时钟列表
   set_clock_uncertainty [-setup] [-hold] -from|-rise_from|-fall_from 时钟
                         -to|-rise_to|-fall_to 时钟 值
   给一个或多个时钟、或者一对时钟之间设置 setup/hold 不确定度。
   -setup 0.1 -hold 0.05 这种把值直接跟在 -setup/-hold 后面的写法是方言，也接受。 */
static const MstaSdcOpt s_vClockUncertaintyOpts[] = {
    { "-setup", MSTA_SDC_FLAG_NUMBER },
    { "-hold",  MSTA_SDC_FLAG_NUMBER },
    { "-rise",  MSTA_SDC_FLAG        },
    { "-fall",  MSTA_SDC_FLAG        },
    { "-from",  MSTA_SDC_RF_VALUE    },
    { "-to",    MSTA_SDC_RF_VALUE    },
    { NULL,     MSTA_SDC_FLAG        } };

static void Msta_SdcSetUncertainty( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    const char *pSetup = Msta_SdcOptValue( pCmd, "-setup" );
    const char *pHold  = Msta_SdcOptValue( pCmd, "-hold" );
    const char *pFrom  = Msta_SdcOptValue( pCmd, "-from" );
    const char *pTo    = Msta_SdcOptValue( pCmd, "-to" );
    char FromRF = Msta_SdcOptEdge( pCmd, "-from" );
    char ToRF   = Msta_SdcOptEdge( pCmd, "-to" );
    int fSetup = Msta_SdcHasFlag( pCmd, "-setup" );
    int fHold  = Msta_SdcHasFlag( pCmd, "-hold" );
    int fRise  = Msta_SdcHasFlag( pCmd, "-rise" );
    int fFall  = Msta_SdcHasFlag( pCmd, "-fall" );
    char **pRest = pCmd->ppPos;
    int nRest = pCmd->nPos, i;

    /* 裸 -rise/-fall 限定的是 -to 一侧（捕获沿）的边沿。 */
    if ( fRise && !fFall && ToRF == 0 ) ToRF = 'r';
    if ( fFall && !fRise && ToRF == 0 ) ToRF = 'f';
    /* 第一个位置参数是数值时，它就是不确定度：没写 -setup/-hold 时两者都用它，
       否则补给还没取到值的那个。剩下的位置参数是时钟。 */
    if ( nRest > 0 && Msta_SdcIsNumber(pRest[0]) )
    {
        if ( !fSetup && !fHold ) pSetup = pHold = pRest[0];
        else
        {
            if ( fSetup && pSetup == NULL ) pSetup = pRest[0];
            if ( fHold && pHold == NULL ) pHold = pRest[0];
        }
        pRest++;
        nRest--;
    }
    if ( fSetup && fHold )
    {
        if ( pSetup == NULL ) pSetup = pHold;
        if ( pHold == NULL ) pHold = pSetup;
    }
    if ( pFrom != NULL || pTo != NULL )
    {
        MstaClock *pFromClock = pFrom ? Msta_SdcFindClock(p,pFrom) : NULL;
        MstaClock *pToClock   = pTo   ? Msta_SdcFindClock(p,pTo)   : NULL;
        if ( pFrom && pFromClock == NULL )
        { Msta_WarnOnce("set_clock_uncertainty: no clock named \"%s\"",pFrom); return; }
        if ( pTo && pToClock == NULL )
        { Msta_WarnOnce("set_clock_uncertainty: no clock named \"%s\"",pTo); return; }
        if ( pSetup == NULL && pHold == NULL )
        { Msta_WarnOnce("set_clock_uncertainty needs an uncertainty value"); return; }
        if ( nRest > 0 )
            Msta_WarnOnce("set_clock_uncertainty: object list is ignored for -from/-to form");
        for ( i = 0; i < 2; i++ )
        {
            const char *pValue = i ? pHold : pSetup;
            MstaInterClockUnc *pUnc;
            if ( pValue == NULL )
                continue;
            pUnc = MstaInterClockUncArrayAppend( &p->vInterClockUnc );
            pUnc->FromClock = pFromClock ? pFromClock->Name : MSTA_NO_ID;
            pUnc->ToClock   = pToClock   ? pToClock->Name   : MSTA_NO_ID;
            pUnc->FromRF = FromRF;
            pUnc->ToRF = ToRF;
            pUnc->fSetup = i ? 0 : 1;
            pUnc->fHold  = i ? 1 : 0;
            pUnc->Value  = Msta_SdcToPs( p, pValue );
        }
        return;
    }
    if ( nRest == 0 || (fSetup && pSetup == NULL) || (fHold && pHold == NULL) ||
         (pSetup == NULL && pHold == NULL) )
    {
        Msta_WarnOnce( "set_clock_uncertainty needs a value and clock objects" );
        return;
    }
    for ( i = 0; i < nRest; i++ )
    {
        MstaClock *pClock = Msta_SdcFindClock( p, pRest[i] );
        if ( pClock == NULL )
        {
            Msta_WarnOnce( "set_clock_uncertainty: no clock named \"%s\"", pRest[i] );
            continue;
        }
        if ( pSetup )
            pClock->UncertaintySetup = Msta_SdcToPs( p,pSetup );
        if ( pHold )
            pClock->UncertaintyHold = Msta_SdcToPs( p,pHold );
    }
}

/* set_clock_latency [-source] [-min|-max|-early|-late] [-rise|-fall] [-clock 时钟] 延迟 时钟列表
   设置理想时钟分析的源延迟 / 网络延迟。-early/-late 与 -min/-max 同义；
   手册允许用 -clock clock_list 代替对象列表。 */
static const MstaSdcOpt s_vClockLatencyOpts[] = {
    { "-clock",  MSTA_SDC_VALUE },
    { "-source", MSTA_SDC_FLAG  },
    { "-min",    MSTA_SDC_FLAG  },
    { "-max",    MSTA_SDC_FLAG  },
    { "-early",  MSTA_SDC_FLAG  },
    { "-late",   MSTA_SDC_FLAG  },
    { "-rise",   MSTA_SDC_FLAG  },
    { "-fall",   MSTA_SDC_FLAG  },
    { NULL,      MSTA_SDC_FLAG  } };

static void Msta_SdcSetClockLatency( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    const char *pClockOpt = Msta_SdcOptValue( pCmd, "-clock" );
    int fSource = Msta_SdcHasFlag( pCmd, "-source" );
    int fMax = Msta_SdcHasFlag( pCmd, "-max" ) || Msta_SdcHasFlag( pCmd, "-late" );
    int fMin = Msta_SdcHasFlag( pCmd, "-min" ) || Msta_SdcHasFlag( pCmd, "-early" );
    int Sel[2][2], i;
    double Delay;
    if ( pCmd->nPos < 1 || !Msta_SdcIsNumber(pCmd->ppPos[0]) )
    { Msta_WarnOnce("set_clock_latency needs a delay and clock objects"); return; }
    Delay = Msta_SdcToPs(p,pCmd->ppPos[0]);
    Msta_SdcMinMaxRiseFall( fMin, fMax, Msta_SdcHasFlag( pCmd, "-rise" ),
                            Msta_SdcHasFlag( pCmd, "-fall" ), Sel );
    /* 要设的时钟：-clock 的值（写了的话）排第一，然后是延迟值后面的位置参数。
       下标 0 留给 -clock，位置参数从 1 开始正好跳过延迟值。 */
    for ( i = pClockOpt ? 0 : 1; i < pCmd->nPos; i++ )
    {
        const char *pName = i == 0 ? pClockOpt : pCmd->ppPos[i];
        MstaClock *pClock = Msta_SdcFindClock(p,pName);
        if ( pClock == NULL )
        { Msta_WarnOnce("set_clock_latency: unknown clock \"%s\"",pName); continue; }
        if ( fSource )
        {
            Msta_SdcStoreMinMaxRiseFall( pClock->SourceLatency, Sel, Delay );
            pClock->SourceLatencyMax = fmax(pClock->SourceLatency[1][0],pClock->SourceLatency[1][1]);
            pClock->SourceLatencyMin = fmin(pClock->SourceLatency[0][0],pClock->SourceLatency[0][1]);
        }
        else
        {
            Msta_SdcStoreMinMaxRiseFall( pClock->NetworkLatency, Sel, Delay );
            pClock->NetworkLatencyMax = fmax(pClock->NetworkLatency[1][0],pClock->NetworkLatency[1][1]);
            pClock->NetworkLatencyMin = fmin(pClock->NetworkLatency[0][0],pClock->NetworkLatency[0][1]);
            /* 网络延迟是对时钟树延迟的估计，只对理想时钟有意义：给了它就是要用
               估计值代替沿时钟树算出的真实延迟，所以这个时钟按理想时钟处理。 */
            pClock->fPropagated = 0;
        }
    }
}

/* set_max_transition / set_max_fanout / set_max_capacitance：
   [current_design] 是全局限制；给端口/网络/引脚/单元时记在对应的网络上，
   检查在时序分析之后统一做（按对象优先，没写对象的用全局值）。 */
static void Msta_SdcSetDrcLimit( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib,
                                 int nWhich, int argc, char **argv )
{
    MstaSdcArgs A;
    char **pRest = Msta_SdcArgBuffer( argc );
    int nRest, i, fDesignObject = 0, fPerObject = 0;
    double Value;
    const char *pName = nWhich == 0 ? "set_max_transition"
                     : nWhich == 1 ? "set_max_fanout"
                     : nWhich == 2 ? "set_max_capacitance" : "set_min_capacitance";
    Msta_SdcArgsStart( &A, argc );

    /* 这些选项只改变检查范围，msta 一律按最坏角检查。 */
    if ( Msta_SdcTakeFlag(argc,argv,&A,"-clock_path") ||
         Msta_SdcTakeFlag(argc,argv,&A,"-data_path") ||
         Msta_SdcTakeFlag(argc,argv,&A,"-rise") ||
         Msta_SdcTakeFlag(argc,argv,&A,"-fall") )
        Msta_WarnOnce("%s: -clock_path/-data_path/-rise/-fall are not modeled; "
                      "the limit is checked on both", pName);
    nRest = Msta_SdcRest(argc,argv,&A,pRest, argc );
    if ( nRest < 1 || !Msta_SdcIsNumber(pRest[0]) )
    { Msta_WarnOnce("%s needs a numeric limit", pName); return; }
    for ( i = 1; i < nRest; i++ )
    {
        if ( Msta_SdcKindOf(pRest[i]) == 'D' ) fDesignObject = 1;
        else if ( Msta_SdcKindOf(pRest[i]) == 'C' ) fPerObject = 1;   /* 时钟域限制 */
        else fPerObject = 1;
    }
    if ( !fDesignObject && !fPerObject )
    { Msta_WarnOnce("%s needs a limit value and objects", pName); return; }
    if ( nWhich == 0 )
        Value = Msta_SdcToPs(p,pRest[0]);
    else if ( nWhich >= 2 )
        Value = atof(pRest[0]) * ( p->CapScaleFf > 0.0 ? p->CapScaleFf : pLib->CapScale );
    else
        Value = atof(pRest[0]);
    if ( Value <= 0.0 )
    { Msta_WarnOnce("%s needs a positive limit", pName); return; }
    if ( fDesignObject )
    {
        if      ( nWhich == 0 ) p->MaxTransition  = Value;
        else if ( nWhich == 1 ) p->MaxFanout      = Value;
        else if ( nWhich == 2 ) p->MaxCapacitance = Value;
        else                    p->MinCapacitance = Value;
    }
    for ( i = 1; i < nRest; i++ )
    {
        int j, *pNets, nFound;
        if ( Msta_SdcKindOf(pRest[i]) == 'D' )
            continue;
        if ( Msta_SdcKindOf(pRest[i]) == 'C' )
        {
            /* 时钟域限制：时钟树要等传播完才知道，先告警跳过这一项。 */
            Msta_WarnOnce("%s on a clock (clock-domain limit) is not modeled; "
                          "use ports or cells instead", pName);
            continue;
        }
        if ( Msta_SdcKindOf(pRest[i]) == 'I' )
        {
            /* 单元形式的限制：记到它驱动的那些网络上。 */
            int nInst = Msta_DesignFindInstByName( pDes, pRest[i] );
            MstaInst *pInst;
            if ( nInst < 0 )
            { Msta_WarnOnce("%s: unknown instance \"%s\"", pName, pRest[i]); continue; }
            pInst = MstaInstArrayAt( &pDes->vInsts, nInst );
            for ( j = 0; j < pInst->nPins && j < pInst->pCell->vPins.nSize; j++ )
            {
                MstaNetCons *pCons;
                if ( pInst->pNets[j] < 0 ||
                     pInst->pCell->vPins.pData[j].Dir != MSTA_DIR_OUTPUT )
                    continue;
                pCons = Msta_SdcNetConsOrCreate( p, pInst->pNets[j] );
                if      ( nWhich == 0 ) pCons->DrcMaxTransition   = Value;
                else if ( nWhich == 1 ) pCons->DrcMaxFanout       = Value;
                else if ( nWhich == 2 ) pCons->DrcMaxCapacitance  = Value;
                else                    pCons->DrcMinCapacitance  = Value;
            }
            continue;
        }
        nFound = Msta_SdcResolveNets( pDes, pRest[i], &pNets );
        for ( j = 0; j < nFound; j++ )
        {
            MstaNetCons *pCons = Msta_SdcNetConsOrCreate( p, pNets[j] );
            if      ( nWhich == 0 ) pCons->DrcMaxTransition   = Value;
            else if ( nWhich == 1 ) pCons->DrcMaxFanout       = Value;
            else if ( nWhich == 2 ) pCons->DrcMaxCapacitance  = Value;
            else                    pCons->DrcMinCapacitance  = Value;
        }
    }
}

/* set_propagated_clock 时钟列表
   把选中的时钟改成按时钟树传播。这条命令没有选项，每个词都当时钟名查。 */
static const MstaSdcOpt s_vPropagatedClockOpts[] = {
    { NULL, MSTA_SDC_FLAG } };

static void Msta_SdcSetPropagatedClock( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    int i;
    if ( pCmd->nPos == 0 )
    { Msta_WarnOnce("set_propagated_clock needs clock objects"); return; }
    for ( i = 0; i < pCmd->nPos; i++ )
    {
        MstaClock *pClock = Msta_SdcFindClock(p,pCmd->ppPos[i]);
        if ( pClock ) pClock->fPropagated = pClock->fPropagatedSet = 1;
        else Msta_WarnOnce("set_propagated_clock: unknown clock \"%s\"",pCmd->ppPos[i]);
    }
}

/* set_clock_transition [-rise|-fall] [-min|-max] 摆率 时钟列表
   给时钟源指定摆率；不写这里就用时钟网络上的输入摆率或默认值。 */
static const MstaSdcOpt s_vClockTransitionOpts[] = {
    { "-min",  MSTA_SDC_FLAG },
    { "-max",  MSTA_SDC_FLAG },
    { "-rise", MSTA_SDC_FLAG },
    { "-fall", MSTA_SDC_FLAG },
    { NULL,    MSTA_SDC_FLAG } };

static void Msta_SdcSetClockTransition( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    int Sel[2][2], i;
    double Slew;
    if ( pCmd->nPos < 2 || !Msta_SdcIsNumber(pCmd->ppPos[0]) )
    { Msta_WarnOnce("set_clock_transition needs a value and clock objects"); return; }
    Slew = Msta_SdcToPs(p,pCmd->ppPos[0]);
    Msta_SdcMinMaxRiseFall( Msta_SdcHasFlag( pCmd, "-min" ), Msta_SdcHasFlag( pCmd, "-max" ),
                            Msta_SdcHasFlag( pCmd, "-rise" ), Msta_SdcHasFlag( pCmd, "-fall" ), Sel );
    for ( i = 1; i < pCmd->nPos; i++ )
    {
        const char *pName = pCmd->ppPos[i];
        MstaClock *pClock = Msta_SdcFindClock(p,pName);
        if ( pClock == NULL )
        { Msta_WarnOnce("set_clock_transition: unknown clock \"%s\"",pName); continue; }
        if ( pClock->SourceNet < 0 )
        { Msta_WarnOnce("set_clock_transition on virtual clock \"%s\" is ignored",pName); continue; }
        Msta_SdcStoreMinMaxRiseFall( pClock->Slew, Sel, Slew );
    }
}

/* set_ideal_latency / set_ideal_transition：拿掉数值和对象，剩下的选项都记下来。
   对象都解析成网络；时钟对象没建模，单独告警。返回网络个数；缺值或对象时
   返回 0，-1 表示整条命令作废（调用方计入 ignored）。 */
static int Msta_SdcIdealTargets( MstaSdc *p, MstaDesign *pDes, const char *pCmd,
                                 int argc, char **argv, int *pfRise, int *pfFall,
                                 int *pfMax, int *pfMin, double *pdValue, int **ppNets )
{
    MstaSdcArgs A;
    char **pRest = Msta_SdcArgBuffer( argc );
    MstaSdcIntArray vNets;
    int nRest, i;
    Msta_SdcArgsStart( &A, argc );
    *ppNets = NULL;

    *pfMax = Msta_SdcTakeFlag( argc, argv, &A, "-max" );
    *pfMin = Msta_SdcTakeFlag( argc, argv, &A, "-min" );
    *pfRise = Msta_SdcTakeFlag( argc, argv, &A, "-rise" );
    *pfFall = Msta_SdcTakeFlag( argc, argv, &A, "-fall" );
    nRest = Msta_SdcRest( argc, argv, &A, pRest, argc );
    if ( nRest < 2 || !Msta_SdcIsNumber(pRest[0]) )
    {
        Msta_WarnOnce( "%s needs a value and a target", pCmd );
        return 0;
    }
    *pdValue = Msta_SdcToPs( p, pRest[0] );
    MstaSdcIntArrayInit( &vNets );
    for ( i = 1; i < nRest; i++ )
    {
        /* 手册里 -min/-max 是开关，值只有一个；"-max 0.5 -min 0.2" 这种
           连着写两个数的方言没建模，整条命令作废，免得按错的含义约束。 */
        if ( Msta_SdcIsNumber( pRest[i] ) )
        {
            Msta_WarnOnce( "%s: \"-min/-max value\" is not SDC 1.8 syntax; "
                           "constraint rejected", pCmd );
            MstaSdcIntArrayFree( &vNets );
            return -1;
        }
        if ( Msta_SdcKindOf( pRest[i] ) == 'C' )
        {
            Msta_WarnOnce( "%s: clock objects are not modeled; constraint rejected", pCmd );
            MstaSdcIntArrayFree( &vNets );
            return -1;
        }
        Msta_SdcResolveNetsInto( pDes, pRest[i], &vNets );
    }
    *ppNets = (int *)Msta_SdcArenaKeep( vNets.pData );
    return vNets.nSize;
}

/* 理想网络的属性存一份；-min/-max 与 -rise/-fall 分别记住，用的时候再挑。 */
static void Msta_SdcStoreIdeal( double *pMaxRise, double *pMaxFall,
                                double *pMinRise, double *pMinFall,
                                int fRise, int fFall, int fMax, int fMin, double Value )
{
    if ( fMax || !fMin )
    {
        if ( fRise || !fFall ) *pMaxRise = Value;
        if ( fFall || !fRise ) *pMaxFall = Value;
    }
    if ( fMin || !fMax )
    {
        if ( fRise || !fFall ) *pMinRise = Value;
        if ( fFall || !fRise ) *pMinFall = Value;
    }
}

/* set_ideal_network [-no_propagate] object_list
   把对象标成理想网络；不带 -no_propagate 时理想属性沿组合扇出继续往下传。 */
static void Msta_SdcSetIdealNetwork( MstaSdc *p, MstaDesign *pDes, int argc, char **argv )
{
    MstaSdcArgs A;
    char **pRest = Msta_SdcArgBuffer( argc );
    int nRest, i, j, fNoProp;
    Msta_SdcArgsStart( &A, argc );

    fNoProp = Msta_SdcTakeFlag( argc, argv, &A, "-no_propagate" );
    if ( Msta_SdcTakeFlag( argc, argv, &A, "-no_propagation" ) )
    {
        Msta_WarnOnce( "set_ideal_network -no_propagation is not SDC 1.8 syntax; "
                       "honored as -no_propagate" );
        fNoProp = 1;
    }
    if ( Msta_SdcTakeFlag( argc, argv, &A, "-force" ) )
        Msta_WarnOnce( "set_ideal_network -force is not modeled; ignored" );
    nRest = Msta_SdcRest( argc, argv, &A, pRest, argc );
    if ( nRest < 1 )
    {
        Msta_WarnOnce( "set_ideal_network needs a target" );
        return;
    }
    for ( i = 0; i < nRest; i++ )
    {
        int *pNets, nFound;
        if ( Msta_SdcKindOf( pRest[i] ) == 'C' )
        {
            Msta_WarnOnce( "set_ideal_network: clock objects are not modeled; "
                           "constraint rejected" );
            p->nCommandsIgnored++;
            return;
        }
        nFound = Msta_SdcResolveNets( pDes, pRest[i], &pNets );
        for ( j = 0; j < nFound; j++ )
        {
            MstaNetCons *pCons = Msta_SdcNetConsOrCreate( p, pNets[j] );
            pCons->fIdeal = 1;
            if ( fNoProp ) pCons->fIdealNoPropagate = 1;
        }
    }
}

/* set_ideal_latency [-rise|-fall] [-min|-max] delay object_list */
static void Msta_SdcSetIdealLatency( MstaSdc *p, MstaDesign *pDes, int argc, char **argv )
{
    int *pNets, fRise, fFall, fMax, fMin, nFound, i;
    double Delay;

    nFound = Msta_SdcIdealTargets( p, pDes, "set_ideal_latency", argc, argv,
                                   &fRise, &fFall, &fMax, &fMin, &Delay, &pNets );
    if ( nFound < 0 )
    { p->nCommandsIgnored++; return; }
    if ( fRise && fFall )
        Msta_WarnOnce( "set_ideal_latency: rise/fall values are merged" );
    for ( i = 0; i < nFound; i++ )
    {
        MstaNetCons *pCons = Msta_SdcNetConsOrCreate( p, pNets[i] );
        Msta_SdcStoreIdeal( &pCons->IdealLatencyMaxRise, &pCons->IdealLatencyMaxFall,
                            &pCons->IdealLatencyMinRise, &pCons->IdealLatencyMinFall,
                            fRise, fFall, fMax, fMin, Delay );
    }
}

/* set_ideal_transition [-rise|-fall] [-min|-max] transition_time object_list */
static void Msta_SdcSetIdealTransition( MstaSdc *p, MstaDesign *pDes, int argc, char **argv )
{
    int *pNets, fRise, fFall, fMax, fMin, nFound, i;
    double Slew;

    nFound = Msta_SdcIdealTargets( p, pDes, "set_ideal_transition", argc, argv,
                                   &fRise, &fFall, &fMax, &fMin, &Slew, &pNets );
    if ( nFound < 0 )
    { p->nCommandsIgnored++; return; }
    if ( fRise && fFall )
        Msta_WarnOnce( "set_ideal_transition: rise/fall values are merged" );
    for ( i = 0; i < nFound; i++ )
    {
        MstaNetCons *pCons = Msta_SdcNetConsOrCreate( p, pNets[i] );
        Msta_SdcStoreIdeal( &pCons->IdealTranMaxRise, &pCons->IdealTranMaxFall,
                            &pCons->IdealTranMinRise, &pCons->IdealTranMinFall,
                            fRise, fFall, fMax, fMin, Slew );
    }
}

/* -clock 的取值可能是裸名字、Tcl 列表（空格分隔）或 get_clocks 的集合标记
   （\x1e<kind> 打头、名字之间用 \x1f 分隔），这里逐个取出来。 */
static const char *Msta_SdcNextClockName( const char **ppList, char *pName, size_t nCap )
{
    const char *p = *ppList;
    size_t n = 0;
    if ( p == NULL )
        return NULL;
    if ( p[0] == '\x1e' )               /* 集合标记：跳过 \x1e 和类型字符 */
        p += 2;
    while ( *p == ' ' || *p == '\t' || *p == '\x1f' )
        p++;
    if ( *p == 0 )
    {
        *ppList = NULL;
        return NULL;
    }
    while ( *p && *p != ' ' && *p != '\t' && *p != '\x1f' && n + 1 < nCap )
        pName[n++] = *p++;
    pName[n] = 0;
    *ppList = p;
    return pName;
}

/* set_clock_sense [-positive|-negative] [-stop_propagation] [-clock clock_list] pin_list
   记下某个脚/网络上的时钟极性（-negative）和"这个时钟到这里不再往下传"。
   -pulse 认得但不建模。 */
static const MstaSdcOpt s_vClockSenseOpts[] = {
    { "-positive",         MSTA_SDC_FLAG  },
    { "-negative",         MSTA_SDC_FLAG  },
    { "-stop_propagation", MSTA_SDC_FLAG  },
    { "-clock",            MSTA_SDC_VALUE },
    { "-pulse",            MSTA_SDC_VALUE },
    { NULL,                MSTA_SDC_FLAG  } };

static void Msta_SdcSetClockSense( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    const char *pClockList = Msta_SdcOptValue( pCmd, "-clock" );
    int fPos  = Msta_SdcHasFlag( pCmd, "-positive" );
    int fNeg  = Msta_SdcHasFlag( pCmd, "-negative" );
    int fStop = Msta_SdcHasFlag( pCmd, "-stop_propagation" );
    char **pRest = pCmd->ppPos;             /* 位置参数是引脚/网络 */
    int nRest = pCmd->nPos, i, j;

    if ( Msta_SdcOptValue( pCmd, "-pulse" ) )
    {
        Msta_WarnOnce( "set_clock_sense -pulse is not modeled; constraint rejected" );
        p->nCommandsIgnored++;
        return;
    }
    if ( fPos && fNeg )
    {
        Msta_WarnOnce( "set_clock_sense cannot combine -positive and -negative; "
                       "constraint rejected" );
        p->nCommandsIgnored++;
        return;
    }
    if ( !fPos && !fNeg && !fStop )
    {
        Msta_WarnOnce( "set_clock_sense needs -positive, -negative or -stop_propagation" );
        return;
    }
    if ( nRest < 1 )
    {
        Msta_WarnOnce( "set_clock_sense needs a pin" );
        return;
    }
    for ( i = 0; i < nRest; i++ )
    {
        int *pNets, nFound;
        if ( Msta_SdcKindOf( pRest[i] ) == 'C' )
        {
            Msta_WarnOnce( "set_clock_sense: clock objects are not modeled, "
                           "use -clock; constraint rejected" );
            p->nCommandsIgnored++;
            return;
        }
        nFound = Msta_SdcResolveNets( pDes, pRest[i], &pNets );
        for ( j = 0; j < nFound; j++ )
        {
            const char *pList = pClockList;
            char sClock[256];
            int fAnyClock = 0;
            while ( Msta_SdcNextClockName(&pList, sClock, sizeof(sClock)) != NULL )
            {
                MstaClock *pClock = Msta_SdcFindClock( p, sClock );
                MstaClockSense *pSense;
                if ( pClock == NULL )
                {
                    Msta_WarnOnce( "set_clock_sense: unknown clock \"%s\"", sClock );
                    continue;
                }
                pSense = MstaClockSenseArrayAppend( &p->vClockSense );
                pSense->Net = pNets[j];
                pSense->Clock = pClock->Name;
                pSense->Polarity = fNeg ? -1 : ( fPos ? 1 : 0 );
                pSense->fStop = fStop;
                fAnyClock = 1;
            }
            if ( !fAnyClock )               /* 没写 -clock：对所有时钟生效 */
            {
                MstaClockSense *pSense = MstaClockSenseArrayAppend( &p->vClockSense );
                pSense->Net = pNets[j];
                pSense->Clock = MSTA_NO_ID;
                pSense->Polarity = fNeg ? -1 : ( fPos ? 1 : 0 );
                pSense->fStop = fStop;
            }
        }
    }
}

static MstaLibInfo *Msta_SdcDefaultOpCond( MstaLib *pLib, MstaId nLibrary, MstaOpCond **ppCond )
{
    int i, j;
    for ( i = 0; i < pLib->vLibs.nSize; i++ )
    {
        MstaLibInfo *pOne = MstaLibInfoArrayAt( &pLib->vLibs, i );
        if ( nLibrary != MSTA_NO_ID && pOne->Name != nLibrary )
            continue;
        for ( j = 0; j < pOne->vOpConds.nSize; j++ )
            if ( MstaOpCondArrayAt(&pOne->vOpConds,j)->Name == pOne->OpCondName )
            {
                *ppCond = MstaOpCondArrayAt( &pOne->vOpConds, j );
                return pOne;
            }
        if ( pOne->vOpConds.nSize > 0 )
        {
            *ppCond = MstaOpCondArrayAt( &pOne->vOpConds, 0 );
            return pOne;
        }
    }
    return NULL;
}

/* 按库里的 K 因子算延迟缩放系数：
     derate = 1 + k_volt*(V - Vnom) + k_temp*(T - Tnom)   （process 项不建模）
   库没声明 K 因子（或没给 V/T）时系数保持 1.0，只把选的角记录/报出来。 */
static void Msta_SdcUpdateKFactor( MstaSdc *p, MstaLib *pLib )
{
    int nCorners = 2, c;
    if ( pLib == NULL )
        return;
    for ( c = 0; c < nCorners; c++ )
    {
        int fMax = ( c == 0 );
        double v = fMax ? p->VoltageMax : p->VoltageMin;
        double t = fMax ? p->TempMax : p->TempMin;
        MstaId nName = fMax ? p->OpCondMax : p->OpCondMin;
        MstaId nLibrary = fMax ? p->OpCondLibraryMax : p->OpCondLibraryMin;
        MstaOpCond *pCond = NULL;
        MstaLibInfo *pInfo = ( nName != MSTA_NO_ID )
                           ? Msta_LibFindOpCond(pLib,Msta_NameStr(nName),nLibrary,&pCond)
                           : NULL;
        double Derate = 1.0, dV, dT;
        /* 没点名就用库里默认的角（K 因子/标称值从它身上取）。 */
        if ( pInfo == NULL )
            pInfo = Msta_SdcDefaultOpCond( pLib, nLibrary, &pCond );
        if ( pInfo == NULL || pCond == NULL )
            continue;
        if ( !Msta_IsSet(v) ) v = pCond->Voltage;
        if ( !Msta_IsSet(t) ) t = pCond->Temperature;
        if ( pCond->KVolt < 0.0 && pCond->KTemp < 0.0 && pCond->KProcess < 0.0 )
        {
            /* 库里没有 K 因子：表值按读进来的原样用，只把请求的电压/温度报出来。 */
            if ( ( Msta_IsSet(v) && pInfo->NomVoltage >= 0.0 && v != pInfo->NomVoltage ) ||
                 ( Msta_IsSet(t) && pInfo->NomTemperature >= 0.0 && t != pInfo->NomTemperature ) )
                Msta_WarnOnce( "set_operating_conditions: the library has no k_volt/k_temp "
                               "factor; delay tables are used as read (the requested "
                               "voltage/temperature is recorded and reported only)" );
            continue;
        }
        dV = ( Msta_IsSet(v) && pInfo->NomVoltage >= 0.0 ) ? v - pInfo->NomVoltage : 0.0;
        dT = ( Msta_IsSet(t) && pInfo->NomTemperature >= 0.0 ) ? t - pInfo->NomTemperature : 0.0;
        if ( pCond->KVolt >= 0.0 )    Derate += pCond->KVolt * dV;
        if ( pCond->KTemp >= 0.0 )    Derate += pCond->KTemp * dT;
        if ( Derate <= 0.0 ) Derate = 1.0;
        if ( fMax ) p->KFactorDerateLate = Derate;
        else        p->KFactorDerateEarly = Derate;
        if ( Derate != 1.0 )
            Msta_WarnOnce( "set_operating_conditions: K-factor derate %.4f applied "
                           "(%.3f V / %.1f C); verify it against your reference tool",
                           Derate, Msta_IsSet(v) ? v : 0.0, Msta_IsSet(t) ? t : 0.0 );
    }
}

/* 统计声明某个 operating condition 的库，发现同名角的歧义。 */
static int Msta_SdcOpCondLibraryCount( MstaLib *pLib, const char *pName )
{
    int i, n = 0;
    for ( i = 0; i < pLib->vLibs.nSize; i++ )
        if ( Msta_LibFindOpCond(pLib,pName,MstaLibInfoArrayAt(&pLib->vLibs,i)->Name,NULL) )
            n++;
    return n;
}

/* 选择 max/min 两个角的工作条件与对应的 Liberty 库。 */
static void Msta_SdcSetOperatingConditions( MstaSdc *p, MstaLib *pLib, int argc, char **argv )
{
    MstaSdcArgs A;
    char **pRest = Msta_SdcArgBuffer( argc );
    const char *pLibName, *pMaxLibName, *pMinLibName, *pMax, *pMin, *pAnalysis;
    const char *pVolt, *pTemp;
    int nRest, i;
    MstaId nLibraryMax, nLibraryMin;
    MstaLibInfo *pMaxInfo = NULL, *pMinInfo = NULL;
    int fMaxLibrarySpecified, fMinLibrarySpecified;
    Msta_SdcArgsStart( &A, argc );

    pLibName  = Msta_SdcValueOf( argc, argv, &A, "-library" );
    pMaxLibName = Msta_SdcValueOf( argc, argv, &A, "-max_library" );
    pMinLibName = Msta_SdcValueOf( argc, argv, &A, "-min_library" );
    pMax      = Msta_SdcValueOf( argc, argv, &A, "-max" );
    pMin      = Msta_SdcValueOf( argc, argv, &A, "-min" );
    pAnalysis = Msta_SdcValueOf( argc, argv, &A, "-analysis_type" );
    pVolt     = Msta_SdcValueOf( argc, argv, &A, "-voltage" );
    pTemp     = Msta_SdcValueOf( argc, argv, &A, "-temperature" );
    if ( Msta_SdcValueOf(argc,argv,&A,"-object_list") )
        Msta_WarnOnce( "set_operating_conditions: -object_list is not modeled" );
    if ( pAnalysis && strcasecmp(pAnalysis,"on_chip_variation") == 0 )
    {
        Msta_WarnOnce( "set_operating_conditions: -analysis_type on_chip_variation is "
                       "not modeled; command rejected" );
        p->nCommandsIgnored++;
        return;
    }
    if ( pAnalysis && strcasecmp(pAnalysis,"single") != 0 &&
         strcasecmp(pAnalysis,"bc_wc") != 0 )
    {
        Msta_WarnOnce( "set_operating_conditions: unknown -analysis_type \"%s\"; command rejected",
                       pAnalysis );
        p->nCommandsIgnored++;
        return;
    }
    if ( pAnalysis && strcasecmp(pAnalysis,"single") == 0 &&
         (pMax != NULL || pMin != NULL || pMaxLibName != NULL || pMinLibName != NULL) )
    {
        Msta_WarnOnce( "set_operating_conditions: -analysis_type single cannot use "
                       "-max/-min or -max_library/-min_library; command rejected" );
        p->nCommandsIgnored++;
        return;
    }
    nRest = Msta_SdcRest( argc, argv, &A, pRest, argc );
    nLibraryMax = pLibName ? Msta_NameId(pLibName) : MSTA_NO_ID;
    nLibraryMin = nLibraryMax;
    if ( pMaxLibName ) nLibraryMax = Msta_NameId(pMaxLibName);
    if ( pMinLibName ) nLibraryMin = Msta_NameId(pMinLibName);
    fMaxLibrarySpecified = pLibName != NULL || pMaxLibName != NULL;
    fMinLibrarySpecified = pLibName != NULL || pMinLibName != NULL;
    for ( i = 0; i < 2; i++ )
    {
        MstaId nLibrary = i ? nLibraryMin : nLibraryMax;
        int fSpecified = i ? fMinLibrarySpecified : fMaxLibrarySpecified;
        if ( fSpecified && nLibrary != MSTA_NO_ID )
        {
            int j, fFound = 0;
            for ( j = 0; j < pLib->vLibs.nSize; j++ )
                if ( MstaLibInfoArrayAt(&pLib->vLibs,j)->Name == nLibrary )
                    { fFound = 1; break; }
            if ( !fFound )
            {
                Msta_WarnOnce("set_operating_conditions: unknown library \"%s\"",
                              Msta_NameStr(nLibrary));
                return;
            }
        }
    }
    if ( pMax == NULL && pMin == NULL )
    {
        if ( nRest > 0 )
            pMax = pMin = pRest[0];
        else if ( pVolt == NULL && pTemp == NULL && !fMaxLibrarySpecified &&
                  !fMinLibrarySpecified )
        {
            Msta_WarnOnce( "set_operating_conditions needs a condition name, library, "
                           "or -voltage/-temperature" );
            return;
        }
    }
    if ( pMax != NULL )
    {
        pMaxInfo = Msta_LibFindOpCond(pLib,pMax,nLibraryMax,NULL);
        if ( pMaxInfo == NULL )
        {
            Msta_WarnOnce( "set_operating_conditions: no library declares operating "
                           "condition \"%s\"; constraint ignored", pMax );
            return;
        }
    }
    if ( pMin != NULL )
    {
        pMinInfo = Msta_LibFindOpCond(pLib,pMin,nLibraryMin,NULL);
        if ( pMinInfo == NULL )
        {
            Msta_WarnOnce( "set_operating_conditions: no library declares operating "
                           "condition \"%s\"; constraint ignored", pMin );
            return;
        }
    }

    /* 给出的名字都有效，库与工艺角的改动一起生效。 */
    if ( fMaxLibrarySpecified ) p->OpCondLibraryMax = nLibraryMax;
    if ( fMinLibrarySpecified ) p->OpCondLibraryMin = nLibraryMin;
    if ( pMaxInfo != NULL )
    {
        p->OpCondMax = Msta_NameId(pMax);
        if ( !fMaxLibrarySpecified )
        {
            if ( Msta_SdcOpCondLibraryCount(pLib,pMax) > 1 )
                Msta_WarnOnce("set_operating_conditions: max corner \"%s\" is in multiple libraries; specify -max_library",
                              pMax);
            p->OpCondLibraryMax = pMaxInfo->Name;
        }
    }
    else if ( fMaxLibrarySpecified && p->OpCondMax != MSTA_NO_ID &&
              Msta_LibFindOpCond(pLib,Msta_NameStr(p->OpCondMax),nLibraryMax,NULL) == NULL )
        p->OpCondMax = MSTA_NO_ID;
    if ( pMinInfo != NULL )
    {
        p->OpCondMin = Msta_NameId(pMin);
        if ( !fMinLibrarySpecified )
        {
            if ( Msta_SdcOpCondLibraryCount(pLib,pMin) > 1 )
                Msta_WarnOnce("set_operating_conditions: min corner \"%s\" is in multiple libraries; specify -min_library",
                              pMin);
            p->OpCondLibraryMin = pMinInfo->Name;
        }
    }
    else if ( fMinLibrarySpecified && p->OpCondMin != MSTA_NO_ID &&
              Msta_LibFindOpCond(pLib,Msta_NameStr(p->OpCondMin),nLibraryMin,NULL) == NULL )
        p->OpCondMin = MSTA_NO_ID;

    /* 只给电压/温度的写法，对带 K 因子的库同样有效。 */
    if ( pVolt != NULL ) p->VoltageMax = p->VoltageMin = atof( pVolt );
    if ( pTemp != NULL ) p->TempMax = p->TempMin = atof( pTemp );
    Msta_SdcUpdateKFactor( p, pLib );
}

/* set_voltage max_case_voltage [-min min_case_value] [-object_list power_nets]
   记录设计的工作电压。分对象的电压要电源域模型，未建模（告警）。 */
static void Msta_SdcSetVoltage( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, int argc, char **argv )
{
    MstaSdcArgs A;
    char **pRest = Msta_SdcArgBuffer( argc );
    const char *pMin;
    int nRest, i;
    Msta_SdcArgsStart( &A, argc );

    pMin = Msta_SdcValueOf( argc, argv, &A, "-min" );
    if ( Msta_SdcValueOf( argc, argv, &A, "-object_list" ) )
        Msta_WarnOnce( "set_voltage: -object_list needs a power domain model; ignored" );
    nRest = Msta_SdcRest( argc, argv, &A, pRest, argc );
    for ( i = 0; i < nRest; i++ )
        if ( !Msta_SdcIsNumber(pRest[i]) )
        {
            Msta_WarnOnce( "set_voltage: per-object voltage is not modeled; ignored" );
            return;
        }
    if ( nRest < 1 || !Msta_SdcIsNumber(pRest[0]) )
    { Msta_WarnOnce("set_voltage needs a voltage value"); return; }
    p->VoltageMax = atof( pRest[0] );
    p->VoltageMin = pMin ? atof( pMin ) : p->VoltageMax;
    Msta_SdcUpdateKFactor( p, pLib );
    /* 延迟表只能靠库里的 k 因子随电压缩放；没有 k 因子时 set_voltage 只被记录、
       不改变延迟，电压偏离标称值时要告警，免得用户以为结果已按新电压算过。 */
    if ( pLib != NULL && pLib->vLibs.nSize > 0 )
    {
        double Nominal = MstaLibInfoArrayAt(&pLib->vLibs,0)->NomVoltage;
        if ( Nominal >= 0.0 && fabs(p->VoltageMax - Nominal) > 1e-6 )
            Msta_WarnOnce( "set_voltage: %.3f V is recorded, but the library has no "
                           "k_volt factor; delay tables are used as read (nominal %.3f V)",
                           p->VoltageMax, Nominal );
    }
    (void)pDes;
}

/* SDC 是否点过工作条件（set_operating_conditions 的角名/库，或 set_voltage）。 */
int Msta_SdcOpCondSelected( MstaSdc *p )
{
    return p->OpCondMax != MSTA_NO_ID || p->OpCondMin != MSTA_NO_ID ||
           p->OpCondLibraryMax != MSTA_NO_ID || p->OpCondLibraryMin != MSTA_NO_ID ||
           Msta_IsSet(p->VoltageMax) || Msta_IsSet(p->VoltageMin);
}

MstaId Msta_SdcOperatingLibrary( MstaSdc *p, int fMax )
{
    return fMax ? p->OpCondLibraryMax : p->OpCondLibraryMin;
}

/* 这个角上生效的工艺角：命令选中的名字，否则库里默认的；再取它的电压/温度。 */
void Msta_SdcOpCondInfo( MstaSdc *p, MstaLib *pLib, int fMax,
                         const char **ppName, double *pVoltage, double *pTemp )
{
    MstaId nName = fMax ? p->OpCondMax : p->OpCondMin;
    MstaId nLibrary = fMax ? p->OpCondLibraryMax : p->OpCondLibraryMin;
    MstaOpCond *pCond = NULL;
    MstaLibInfo *pInfo;
    if ( ppName ) *ppName = NULL;
    if ( pVoltage ) *pVoltage = fMax ? p->VoltageMax : p->VoltageMin;
    if ( pTemp ) *pTemp = fMax ? p->TempMax : p->TempMin;
    if ( pLib == NULL )
        return;
    if ( nName != MSTA_NO_ID )
        pInfo = Msta_LibFindOpCond( pLib, Msta_NameStr(nName), nLibrary, &pCond );
    else
        pInfo = Msta_SdcDefaultOpCond( pLib, nLibrary, &pCond );     /* 没选就报库里默认的那个角。 */
    if ( pInfo == NULL )
        return;
    if ( ppName ) *ppName = Msta_NameStr( pCond ? pCond->Name : pInfo->OpCondName );
    if ( pCond != NULL )
    {
        if ( pVoltage && !Msta_IsSet(*pVoltage) && pCond->Voltage >= 0.0 )
            *pVoltage = pCond->Voltage;
        if ( pTemp && !Msta_IsSet(*pTemp) ) *pTemp = pCond->Temperature;
    }
}

/* set_data_check [-from A] [-to B] [-setup|-hold] [-clock C] margin：
   两条数据路径之间的检查，-from 是参照。两个对象都解析成网络。 */
static void Msta_SdcSetDataCheck( MstaSdc *p, MstaDesign *pDes, int argc, char **argv )
{
    MstaSdcArgs A;
    char **pRest = Msta_SdcArgBuffer( argc );
    const char *pFrom, *pTo;
    char FromRF = 0, ToRF = 0;
    int nRest, *pFromNets, *pToNets, nFromCount, nToCount;
    MstaDataCheck *pCheck;
    Msta_SdcArgsStart( &A, argc );

    pFrom = Msta_SdcValueOf( argc, argv, &A, "-from" );
    pTo   = Msta_SdcValueOf( argc, argv, &A, "-to" );
    if ( pFrom == NULL && ( pFrom = Msta_SdcValueOf(argc,argv,&A,"-rise_from") ) != NULL )
        FromRF = 'r';
    if ( pFrom == NULL && ( pFrom = Msta_SdcValueOf(argc,argv,&A,"-fall_from") ) != NULL )
    {   FromRF = 'f';  }
    if ( pTo == NULL && ( pTo = Msta_SdcValueOf(argc,argv,&A,"-rise_to") ) != NULL )
        ToRF = 'r';
    if ( pTo == NULL && ( pTo = Msta_SdcValueOf(argc,argv,&A,"-fall_to") ) != NULL )
        ToRF = 'f';
    Msta_SdcValueOf( argc, argv, &A, "-clock" );   /* 检查时钟读入但不使用 */
    if ( pFrom == NULL || pTo == NULL )
    { Msta_WarnOnce("set_data_check needs -from and -to"); return; }
    nRest = Msta_SdcRest( argc, argv, &A, pRest, argc );
    if ( nRest < 1 || !Msta_SdcIsNumber(pRest[0]) )
    { Msta_WarnOnce("set_data_check needs a numeric margin"); return; }
    nFromCount = Msta_SdcResolveNets( pDes, pFrom, &pFromNets );
    nToCount   = Msta_SdcResolveNets( pDes, pTo,   &pToNets );
    if ( nFromCount != 1 || nToCount != 1 )
    { Msta_WarnOnce("set_data_check: -from/-to must each resolve to one net"); return; }
    pCheck = MstaDataCheckArrayAppend( &p->vDataChecks );
    pCheck->FromNet = pFromNets[0];
    pCheck->ToNet   = pToNets[0];
    pCheck->FromText = Msta_NameId( pFrom );
    pCheck->ToText   = Msta_NameId( pTo );
    pCheck->FromRF = FromRF;
    pCheck->ToRF   = ToRF;
    pCheck->fSetup = Msta_SdcTakeFlag( argc, argv, &A, "-setup" );
    pCheck->fHold  = Msta_SdcTakeFlag( argc, argv, &A, "-hold" );
    if ( Msta_SdcTakeFlag(argc,argv,&A,"-rise") || Msta_SdcTakeFlag(argc,argv,&A,"-fall") )
        Msta_WarnOnce("set_data_check: -rise/-fall are not modeled" );
    if ( !pCheck->fSetup && !pCheck->fHold )
        pCheck->fSetup = pCheck->fHold = 1;         /* 不写 -setup/-hold 时两个角都查 */
    pCheck->Value = Msta_SdcToPs( p, pRest[0] );
}

int Msta_SdcDataCheckCount( MstaSdc *p )
{
    return p->vDataChecks.nSize;
}

MstaDataCheck *Msta_SdcDataCheckByIndex( MstaSdc *p, int i )
{
    return ( i < 0 || i >= p->vDataChecks.nSize ) ? NULL
                                                  : MstaDataCheckArrayAt(&p->vDataChecks,i);
}

/* set_max_area area_value：整个设计的面积目标，单位跟随库。 */
static void Msta_SdcSetMaxArea( MstaSdc *p, int argc, char **argv )
{
    MstaSdcArgs A;
    int i;
    Msta_SdcArgsStart( &A, argc );
    for ( i = 1; i < argc; i++ )
        if ( argv[i][0] == '-' )
            Msta_WarnOnce( "set_max_area option \"%s\" is not modeled; ignored", argv[i] );
    if ( argc < 2 || !Msta_SdcIsNumber(argv[argc-1]) )
    { Msta_WarnOnce("set_max_area needs a numeric area"); return; }
    p->MaxArea = atof( argv[argc-1] );
}

/* set_timing_derate [-early|-late] [-cell_delay|-net_delay|-cell_check]
                     [-clock|-data] [-rise|-fall] factor [objects]
   不带对象时是全局系数；带对象时只对那个实例/时钟生效（分对象的值覆盖全局值，
   不是相乘）。-clock 管时钟树上的弧，-data 管数据路径上的单元延迟
   （含 FF 的 clk-to-Q），不写这两个开关时两者都算。 */
static void Msta_SdcSetTimingDerate( MstaSdc *p, MstaDesign *pDes, int argc, char **argv )
{
    MstaSdcArgs A;
    char **pRest = Msta_SdcArgBuffer( argc );
    const char *pFactor;
    int nRest, fEarly, fLate, fCellDelay, fCellCheck = 0, fNetDelay = 0, fClock = 0, fData = 0;
    int fRise, fFall, i;
    double Factor;
    Msta_SdcArgsStart( &A, argc );

    fEarly = Msta_SdcTakeFlag(argc,argv,&A,"-early");
    fLate  = Msta_SdcTakeFlag(argc,argv,&A,"-late");
    fCellCheck = Msta_SdcTakeFlag(argc,argv,&A,"-cell_check");
    fNetDelay  = Msta_SdcTakeFlag(argc,argv,&A,"-net_delay");
    fClock     = Msta_SdcTakeFlag(argc,argv,&A,"-clock");
    fData      = Msta_SdcTakeFlag(argc,argv,&A,"-data");
    fRise = Msta_SdcTakeFlag(argc,argv,&A,"-rise");
    fFall = Msta_SdcTakeFlag(argc,argv,&A,"-fall");
    if ( fRise && fFall )
    { Msta_WarnOnce("set_timing_derate cannot combine -rise and -fall"); return; }
    fCellDelay = Msta_SdcTakeFlag(argc,argv,&A,"-cell_delay");
    if ( !fCellDelay && !fCellCheck && !fNetDelay )
        fCellDelay = 1;          /* 不写类型时只 derate 单元延迟 */
    if ( fNetDelay && !fCellDelay && !fCellCheck )
    {
        Msta_WarnOnce("set_timing_derate -net_delay has no effect (msta has no net delay)");
        p->nCommandsIgnored++;
        return;
    }
    nRest = Msta_SdcRest(argc,argv,&A,pRest, argc );
    if ( nRest < 1 || !Msta_SdcIsNumber(pRest[0]) )
    { Msta_WarnOnce("set_timing_derate needs a derate factor"); return; }
    pFactor = pRest[0];
    Factor = atof(pFactor);
    if ( Factor <= 0.0 )
    { Msta_WarnOnce("set_timing_derate: factor must be positive"); return; }
    if ( !fCellDelay && !fCellCheck )
        return;                  /* -net_delay 单用：msta 没有线延迟，已经告警 */
    /* 带对象：逐条记录。分对象只支持实例（get_cells）和时钟（get_clocks）。 */
    if ( nRest > 1 )
    {
        for ( i = 1; i < nRest; i++ )
        {
            char Kind = Msta_SdcKindOf( pRest[i] );
            int nInst = -1;
            MstaId Name = MSTA_NO_ID;
            MstaObjDerate *pRec;
            if ( Kind == 'C' )
            {
                MstaClock *pClock = Msta_SdcFindClock( p, pRest[i] );
                if ( pClock == NULL )
                {
                    Msta_WarnOnce( "set_timing_derate: unknown clock \"%s\"; command rejected",
                                   pRest[i] );
                    return;
                }
                Name = pClock->Name;
                if ( fRise || fFall )
                    Msta_WarnOnce( "set_timing_derate -rise/-fall on a clock object is not modeled; "
                                   "the clock tree keeps rise/fall merged" );
            }
            else
            {
                nInst = Msta_DesignFindInstByName( pDes, pRest[i] );
                if ( nInst < 0 )
                {
                    /* 端口/网络/库单元上的 derate 在 msta 的模型里没有对应量。 */
                    Msta_WarnOnce( "set_timing_derate object \"%s\" is not modeled "
                                   "(only instances and clocks); command rejected", pRest[i] );
                    return;
                }
            }
            pRec = MstaObjDerateArrayAppend( &p->vObjDerate );
            pRec->Kind = ( Kind == 'C' ) ? 'C' : 'I';
            pRec->Inst = nInst;
            pRec->Name = Name;
            pRec->fRise = fRise;
            pRec->fFall = fFall;
            pRec->fCellCheck = fCellCheck;
            pRec->Early = ( fEarly || !fLate ) ? Factor : 1.0;
            pRec->Late  = ( fLate  || !fEarly ) ? Factor : 1.0;
        }
        return;
    }
    if ( fClock && !fData )      { p->fDerateClock = 1; p->fDerateData  = 0; }
    else if ( fData && !fClock ) { p->fDerateClock = 0; p->fDerateData  = 1; }
    else                         { p->fDerateClock = 1; p->fDerateData  = 1; }
    if ( !fCellDelay ) { p->fDerateClock = 0; p->fDerateData = 0; }
    if ( fEarly || !fLate ) p->DerateEarly = Factor;
    if ( fLate  || !fEarly ) p->DerateLate  = Factor;
    if ( fCellCheck )
    {
        if ( fEarly || !fLate ) p->DerateCheckEarly = Factor;
        if ( fLate  || !fEarly ) p->DerateCheckLate  = Factor;
    }
}

/* 存一条 I/O 延迟；-add_delay 时保留同一网络上的其他时钟。 */
static void Msta_SdcStorePortDelay( MstaSdc *p, int nNet, MstaId Clock,
                                     int fOutput, const char *pMax,
                                     const char *pMin, int fAdd,
                                     int fClockFall, int DataRise,
                                     int RefNet, int fSourceLatencyIncluded,
                                     int fNetworkLatencyIncluded )
{
    MstaIoDelayArray *pArr = fOutput ? &p->vOutputDelays : &p->vInputDelays;
    MstaIoDelay *pEntry = NULL;
    int i;
    for ( i = 0; i < pArr->nSize; i++ )
    {
        MstaIoDelay *pOld = MstaIoDelayArrayAt(pArr,i);
        if ( pOld->Net != nNet ) continue;
        if ( pOld->Clock == Clock && pOld->ClockFall == fClockFall && pOld->DataRise == DataRise ) pEntry = pOld;
        else if ( !fAdd )
        {
            if ( pMax ) pOld->Max = MSTA_UNSET;
            if ( pMin ) pOld->Min = MSTA_UNSET;
        }
    }
    if ( pEntry == NULL )
    {
        pEntry = MstaIoDelayArrayAppend(pArr);
        pEntry->Net = nNet;
        pEntry->Clock = Clock;
        pEntry->Max = pEntry->Min = MSTA_UNSET;
        pEntry->ClockFall = fClockFall;
        pEntry->DataRise = DataRise;
        pEntry->RefNetMax = pEntry->RefNetMin = -1;
    }
    if ( pMax )
    {
        double Value = Msta_SdcToPs(p,pMax);
        if ( !fAdd || !Msta_IsSet(pEntry->Max) || Value > pEntry->Max )
            pEntry->Max = Value;
        pEntry->RefNetMax = RefNet;
        pEntry->SourceLatencyIncludedMax = fSourceLatencyIncluded;
        pEntry->NetworkLatencyIncludedMax = fNetworkLatencyIncluded;
    }
    if ( pMin )
    {
        double Value = Msta_SdcToPs(p,pMin);
        if ( !fAdd || !Msta_IsSet(pEntry->Min) || Value < pEntry->Min )
            pEntry->Min = Value;
        pEntry->RefNetMin = RefNet;
        pEntry->SourceLatencyIncludedMin = fSourceLatencyIncluded;
        pEntry->NetworkLatencyIncludedMin = fNetworkLatencyIncluded;
    }
}

/* fOutput=0 是 set_input_delay，=1 是 set_output_delay。
   不写 -max/-min 时一个值同时约束两个角；写了选项就只改对应角。 */
static void Msta_SdcSetPortDelay( MstaSdc *p, MstaDesign *pDes, int fOutput, int argc, char **argv )
{
    MstaSdcArgs A;
    char **pTargets = Msta_SdcArgBuffer( argc );
    const char *pClockText, *pMax, *pMin, *pReferencePin;
    int nTargets, i, fMax, fMin, fAdd, fClockFall, DataRise = -1;
    int fSourceLatencyIncluded, fNetworkLatencyIncluded, RefNet = -1;
    Msta_SdcArgsStart( &A, argc );

    pClockText = Msta_SdcValueOf( argc, argv, &A, "-clock" );
    pReferencePin = Msta_SdcValueOf( argc, argv, &A, "-reference_pin" );
    fSourceLatencyIncluded = Msta_SdcTakeFlag(argc,argv,&A,"-source_latency_included");
    fNetworkLatencyIncluded = Msta_SdcTakeFlag(argc,argv,&A,"-network_latency_included");
    fClockFall = Msta_SdcTakeFlag( argc, argv, &A, "-clock_fall" );
    fMax = Msta_SdcTakeFlag( argc, argv, &A, "-max" );
    fMin = Msta_SdcTakeFlag( argc, argv, &A, "-min" );
    Msta_SdcTakeFlag(argc,argv,&A,"-pin_load");
    Msta_SdcTakeFlag(argc,argv,&A,"-wire_load");
    if ( Msta_SdcTakeFlag(argc,argv,&A,"-level_sensitive") )
    { Msta_WarnOnce("I/O delay -level_sensitive is not modeled; constraint rejected"); return; }
    if ( Msta_SdcTakeFlag(argc,argv,&A,"-subtract_pin_load") )
    { Msta_WarnOnce("set_load -subtract_pin_load is not modeled; constraint rejected"); return; }
    pMax = pMin = NULL;
    /* 标准 SDC 里 -max/-min 是开关，延迟值是唯一的位置参数，可以出现在选项之间。
       另外接受 '-max 2 -min 1' 这种方言写法：-max/-min 后紧跟的数值只给那一角。 */
    for ( i = 1; i + 1 < argc; i++ )
    {
        if ( !strcmp(argv[i], "-max") && Msta_SdcIsNumber(argv[i+1]) )
        { pMax = argv[i+1]; A.Used[i+1] = 1; }
        if ( !strcmp(argv[i], "-min") && Msta_SdcIsNumber(argv[i+1]) )
        { pMin = argv[i+1]; A.Used[i+1] = 1; }
    }
    fAdd = Msta_SdcTakeFlag( argc, argv, &A, "-add_delay" );
    if ( Msta_SdcTakeFlag( argc, argv, &A, "-rise" ) ) DataRise = 1;
    if ( Msta_SdcTakeFlag( argc, argv, &A, "-fall" ) )
    {
        if ( DataRise == 1 )
        { Msta_WarnOnce("I/O delay cannot combine -rise and -fall"); return; }
        DataRise = 0;
    }
    if ( pReferencePin != NULL )
    {
        int *pRefNets;
        char RefKind = Msta_SdcKindOf(pReferencePin);
        int nRefNets = Msta_SdcResolveNets(pDes,pReferencePin,&pRefNets);
        if ( (RefKind != 0 && RefKind != 'G' && RefKind != 'P') || nRefNets != 1 )
        {
            Msta_WarnOnce("%s -reference_pin must resolve to exactly one pin or port",
                          fOutput ? "set_output_delay" : "set_input_delay");
            p->nCommandsIgnored++;
            return;
        }
        RefNet = pRefNets[0];
        if ( fSourceLatencyIncluded || fNetworkLatencyIncluded )
            Msta_WarnOnce("%s: latency-included flags are ignored with -reference_pin",
                          fOutput ? "set_output_delay" : "set_input_delay");
        fSourceLatencyIncluded = fNetworkLatencyIncluded = 0;
    }
    nTargets = Msta_SdcRest( argc, argv, &A, pTargets, argc );
    if ( nTargets > 0 && Msta_SdcIsNumber(pTargets[0]) )
    {
        if ( !fMax && !fMin )
            pMax = pMin = pTargets[0];
        else
        {
            if ( fMax && pMax == NULL ) pMax = pTargets[0];
            if ( fMin && pMin == NULL ) pMin = pTargets[0];
        }
        for ( i = 1; i < nTargets; i++ )
            pTargets[i-1] = pTargets[i];
        nTargets--;
    }
    if ( ( fMax && pMax == NULL ) || ( fMin && pMin == NULL ) ||
         ( pMax == NULL && pMin == NULL ) || nTargets == 0 )
    {
        Msta_WarnOnce( "%s needs a numeric delay and a port",
                       fOutput ? "set_output_delay" : "set_input_delay" );
        return;
    }
    if ( ( pMax && !Msta_SdcIsNumber(pMax) ) || ( pMin && !Msta_SdcIsNumber(pMin) ) )
    {
        Msta_WarnOnce( "%s: delay must be numeric", fOutput ? "set_output_delay" : "set_input_delay" );
        return;
    }
    if ( pClockText && Msta_SdcFindClock(p,pClockText) == NULL )
    { Msta_WarnOnce("I/O delay: unknown clock \"%s\"; constraint rejected",pClockText); return; }
    /* 不写 -clock 时先记成"未指定"：SDC 允许 create_clock 写在 I/O 约束之后，
       所以真正用哪个时钟留到查询时再按当时只有一个时钟来判定。 */
    if ( pClockText == NULL && pReferencePin == NULL && Msta_SdcClockCount(p) > 1 )
        Msta_WarnOnce("I/O delay without -clock is ambiguous with multiple clocks; "
                      "it applies only while the design has a single clock");
    for ( i = 0; i < nTargets; i++ )
    {
        int *pNets, nNetsFound, j;
        nNetsFound = Msta_SdcResolveNets( pDes, pTargets[i], &pNets );
        for ( j = 0; j < nNetsFound; j++ )
        {
            Msta_SdcNetConsOrCreate( p, pNets[j] );
            Msta_SdcStorePortDelay(p,pNets[j],
                                   pClockText ? Msta_NameId(pClockText) : MSTA_NO_ID,
                                   fOutput,pMax,pMin,fAdd,fClockFall,DataRise,
                                   RefNet,fSourceLatencyIncluded,fNetworkLatencyIncluded);
        }
    }
}

/* set_load <值> [get_ports p ...] —— 值默认按库的电容单位，set_units 可覆盖。 */
static void Msta_SdcSetLoad( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, int argc, char **argv )
{
    MstaSdcArgs A;
    char **pRest = Msta_SdcArgBuffer( argc );
    int nRest, i, fMax, fMin;
    int fSubtract, fPinLoad, fWireLoad;
    Msta_SdcArgsStart( &A, argc );

    fMax = Msta_SdcTakeFlag( argc, argv, &A, "-max" );
    fMin = Msta_SdcTakeFlag( argc, argv, &A, "-min" );
    fSubtract = Msta_SdcTakeFlag( argc, argv, &A, "-subtract_pin_load" );
    /* -pin_load / -wire_load 是"这到底是脚负载还是线负载"的标注；msta 把它们
       都当外加负载加在网络上，与 -subtract_pin_load 的语义不冲突。 */
    fPinLoad  = Msta_SdcTakeFlag( argc, argv, &A, "-pin_load" );
    fWireLoad = Msta_SdcTakeFlag( argc, argv, &A, "-wire_load" );
    if ( fPinLoad && fWireLoad )
    { Msta_WarnOnce("set_load cannot combine -pin_load and -wire_load"); return; }
    if ( Msta_SdcTakeFlag(argc,argv,&A,"-rise") ||
         Msta_SdcTakeFlag(argc,argv,&A,"-fall") )
    { Msta_WarnOnce("set_load -rise/-fall is not SDC 1.8 syntax; constraint rejected"); return; }
    nRest = Msta_SdcRest( argc, argv, &A, pRest, argc );
    if ( nRest < 2 || !Msta_SdcIsNumber(pRest[0]) )
    {
        Msta_WarnOnce( "set_load needs a value and a target" );
        return;
    }
    for ( i = 1; i < nRest; i++ )
    {
        int *pNets, nFound, j;
        double Load = atof( pRest[0] ) * ( p->CapScaleFf > 0.0 ? p->CapScaleFf : pLib->CapScale );
        /* -subtract_pin_load 只对网络有意义（减的是网络上的脚电容），端口不接受。 */
        if ( fSubtract && Msta_SdcKindOf(pRest[i]) == 'P' )
        {
            Msta_WarnOnce( "set_load -subtract_pin_load is not allowed for port objects; "
                           "\"%s\" skipped", pRest[i] );
            continue;
        }
        nFound = Msta_SdcResolveNets( pDes, pRest[i], &pNets );
        for ( j = 0; j < nFound; j++ )
        {
            MstaNetCons *pCons = Msta_SdcNetConsOrCreate(p,pNets[j]);
            if ( fMax || !fMin ) pCons->LoadMax = Load;
            if ( fMin || !fMax ) pCons->LoadMin = Load;
            if ( fSubtract ) pCons->fSubtractPinLoad = 1;
        }
    }
}

/* set_input_transition：可以按 -rise/-fall、-max/-min 分别给值。
   时序引擎用逐边沿字段起步。 */
static void Msta_SdcSetInputSlew( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, int argc, char **argv )
{
    MstaSdcArgs A;
    char **pRest = Msta_SdcArgBuffer( argc );
    int nRest, i, fMax, fMin, fRise, fFall;
    Msta_SdcArgsStart( &A, argc );
    (void)pLib;

    fMax = Msta_SdcTakeFlag( argc, argv, &A, "-max" );
    fMin = Msta_SdcTakeFlag( argc, argv, &A, "-min" );
    fRise = Msta_SdcTakeFlag( argc, argv, &A, "-rise" );
    fFall = Msta_SdcTakeFlag( argc, argv, &A, "-fall" );
    Msta_SdcTakeFlag( argc, argv, &A, "-clock_fall" );
    Msta_SdcValueOf( argc, argv, &A, "-clock" );   /* -clock 只是标注，值也要吃掉 */
    if ( fRise && fFall )
    { Msta_WarnOnce("set_input_transition cannot combine -rise and -fall"); return; }
    nRest = Msta_SdcRest( argc, argv, &A, pRest, argc );
    if ( nRest < 2 || !Msta_SdcIsNumber(pRest[0]) )
    {
        Msta_WarnOnce( "set_input_transition needs a value and a target" );
        return;
    }
    for ( i = 1; i < nRest; i++ )
    {
        int *pNets, nFound, j;
        double Slew = Msta_SdcToPs( p,pRest[0] );
        nFound = Msta_SdcResolveNets( pDes, pRest[i], &pNets );
        for ( j = 0; j < nFound; j++ )
        {
            MstaNetCons *pCons = Msta_SdcNetConsOrCreate(p,pNets[j]);
            if ( fMax || !fMin )
            {
                if ( fRise || !fFall ) pCons->InputSlewMaxRise = Slew;
                if ( fFall || !fRise ) pCons->InputSlewMaxFall = Slew;
            }
            if ( fMin || !fMax )
            {
                if ( fRise || !fFall ) pCons->InputSlewMinRise = Slew;
                if ( fFall || !fRise ) pCons->InputSlewMinFall = Slew;
            }
        }
    }
}

/* 根据驱动单元的输出转换表设置输入端口摆率，并记下驱动单元供分析时算延迟。 */
static void Msta_SdcSetDrivingCell( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, int argc, char **argv )
{
    MstaSdcArgs A;
    char **pTargets = Msta_SdcArgBuffer( argc );
    const char *pCellName, *pPinName, *pFromPin = NULL, *pMult = NULL;
    const char *pInRise = NULL, *pInFall = NULL;
    char sCellQual[512];
    MstaCell *pCell;
    int nTargets, i, k;
    double Slew = 0.0;
    Msta_SdcArgsStart( &A, argc );

    /* 下列选项 msta 没建模：吃掉并告警忽略。 */
    {
        static const char *pIgnoreValue[] = { "-clock", NULL };
        static const char *pIgnoreFlag[] = { "-rise", "-fall", "-dont_scale",
                                             "-no_design_rule", "-clock_fall", NULL };
        int n;
        for ( n = 0; pIgnoreValue[n]; n++ )
            if ( Msta_SdcValueOf( argc, argv, &A, pIgnoreValue[n] ) != NULL )
                Msta_WarnOnce( "set_driving_cell option %s is not modeled; ignored",
                               pIgnoreValue[n] );
        for ( n = 0; pIgnoreFlag[n]; n++ )
            if ( Msta_SdcTakeFlag( argc, argv, &A, pIgnoreFlag[n] ) )
                Msta_WarnOnce( "set_driving_cell option %s is not modeled; ignored",
                               pIgnoreFlag[n] );
        /* -min/-max 只影响哪一角的摆率；msta 两个角都记同一份驱动单元。 */
        Msta_SdcTakeFlag( argc, argv, &A, "-min" );
        Msta_SdcTakeFlag( argc, argv, &A, "-max" );
    }
    pCellName = Msta_SdcValueOf( argc, argv, &A, "-lib_cell" );
    pPinName  = Msta_SdcValueOf( argc, argv, &A, "-pin" );
    pFromPin  = Msta_SdcValueOf( argc, argv, &A, "-from_pin" );
    pMult     = Msta_SdcValueOf( argc, argv, &A, "-multiply_by" );
    pInRise   = Msta_SdcValueOf( argc, argv, &A, "-input_transition_rise" );
    pInFall   = Msta_SdcValueOf( argc, argv, &A, "-input_transition_fall" );
    {
        /* 多库时用 "库名/cell 名" 限定（SDC 手册的 -library + -lib_cell 组合）。 */
        const char *pLibName = Msta_SdcValueOf( argc, argv, &A, "-library" );
        if ( pLibName != NULL && pCellName != NULL &&
             strchr(pCellName,'/') == NULL )
        {
            snprintf( sCellQual, sizeof(sCellQual), "%s/%s", pLibName, pCellName );
            pCellName = sCellQual;
        }
    }
    nTargets = Msta_SdcRest( argc, argv, &A, pTargets, argc );
    if ( pCellName == NULL || nTargets == 0 )
    {
        Msta_WarnOnce( "set_driving_cell needs -lib_cell and a target" );
        return;
    }
    pCell = Msta_LibFindCell( pLib, pCellName );
    if ( pCell == NULL )
    {
        Msta_WarnOnce( "set_driving_cell: cell \"%s\" is not in the requested library", pCellName );
        p->nCommandsIgnored++;
        return;
    }
    for ( k = 0; k < pCell->vArcs.nSize && Slew <= 0.0; k++ )
    {
        MstaArc *pArc = MstaArcArrayAt( &pCell->vArcs, k );
        if ( pPinName && pArc->OutPin != Msta_NameId(pPinName) ) continue;
        if ( Msta_TableExists( &pArc->TransRise ) )
            Slew = pArc->TransRise.pValues[0] * pLib->TimeScale;
        else if ( Msta_TableExists( &pArc->TransFall ) )
            Slew = pArc->TransFall.pValues[0] * pLib->TimeScale;
    }
    for ( i = 0; i < nTargets; i++ )
    {
        int *pNets, nFound, j;
        nFound = Msta_SdcResolveNets( pDes, pTargets[i], &pNets );
        for ( j = 0; j < nFound; j++ )
        {
            MstaNetCons *pCons = Msta_SdcNetConsOrCreate( p, pNets[j] );
            pCons->DrivingCell   = Msta_NameId( pCellName );
            pCons->DrivingPin    = pPinName ? Msta_NameId( pPinName ) : MSTA_NO_ID;
            pCons->DrivingFromPin= pFromPin ? Msta_NameId( pFromPin ) : MSTA_NO_ID;
            pCons->DriveMultiply = pMult ? atof(pMult) : 1.0;
            if ( pCons->DriveMultiply <= 0.0 ) pCons->DriveMultiply = 1.0;
            if ( pInRise ) pCons->DriveInSlewRise = Msta_SdcToPs( p, pInRise );
            if ( pInFall ) pCons->DriveInSlewFall = Msta_SdcToPs( p, pInFall );
            if ( !Msta_IsSet( pCons->InputSlewMaxRise ) ) pCons->InputSlewMaxRise = Slew;
            if ( !Msta_IsSet( pCons->InputSlewMaxFall ) ) pCons->InputSlewMaxFall = Slew;
            if ( !Msta_IsSet( pCons->InputSlewMinRise ) ) pCons->InputSlewMinRise = Slew;
            if ( !Msta_IsSet( pCons->InputSlewMinFall ) ) pCons->InputSlewMinFall = Slew;
        }
    }
}

/* 路径例外共用的选项解析结果：-from/-to 集合、-through 分组和边沿限定。 */
typedef struct {
    int  iFromBeg, iFromEnd, iToBeg, iToEnd;
    char FromRF, ToRF;
    MstaThruObject Thru[MSTA_SDC_MAX_THRU];
    int  nThru;
} MstaSdcPathArgs;

/* 解析 -from/-rise_from/-fall_from/-to/-rise_to/-fall_to/-through 这类集合选项。
   返回 1 成功；0 表示选项本身不支持，-1 表示集合为空或 -through 超限（已告警），
   两者调用方都丢弃这条约束。 */
static int Msta_SdcPathCollection( int argc, char **argv, int *pi, MstaSdcPathArgs *pA )
{
    const char *pKey = argv[*pi];
    int fFrom = 0, fTo = 0, fThrough = 0, Beg, k;
    char RF = 0;

    if      ( !strcmp(pKey,"-from") )         fFrom = 1;
    else if ( !strcmp(pKey,"-rise_from") )  { fFrom = 1; RF = 'r'; }
    else if ( !strcmp(pKey,"-fall_from") )  { fFrom = 1; RF = 'f'; }
    else if ( !strcmp(pKey,"-to") )           fTo = 1;
    else if ( !strcmp(pKey,"-rise_to") )    { fTo = 1; RF = 'r'; }
    else if ( !strcmp(pKey,"-fall_to") )    { fTo = 1; RF = 'f'; }
    else if ( !strcmp(pKey,"-through") )      fThrough = 1;
    else if ( !strcmp(pKey,"-rise_through") ) { fThrough = 1; RF = 'r'; }
    else if ( !strcmp(pKey,"-fall_through") ) { fThrough = 1; RF = 'f'; }
    else return 0;

    Beg = *pi + 1;
    while ( *pi + 1 < argc && argv[*pi+1][0] != '-' ) (*pi)++;
    if ( Beg == *pi + 1 )
    {
        Msta_WarnOnce("path exception option \"%s\" has an empty collection; constraint rejected",pKey);
        return -1;
    }
    if ( fFrom ) { pA->iFromBeg = Beg; pA->iFromEnd = *pi + 1; pA->FromRF = RF; }
    else if ( fTo ) { pA->iToBeg = Beg; pA->iToEnd = *pi + 1; pA->ToRF = RF; }
    else if ( fThrough )
    {
        /* 一个 -through 是一组可以互相替代的对象；多个 -through 之间按路径顺序匹配。 */
        if ( pA->nThru + 1 + (*pi + 1 - Beg) > MSTA_SDC_MAX_THRU )
        {
            Msta_WarnOnce("path exception has more than %d -through objects; constraint rejected",
                          MSTA_SDC_MAX_THRU);
            return -1;
        }
        pA->Thru[pA->nThru].Text = MSTA_NO_ID;
        pA->Thru[pA->nThru].Kind = MSTA_SDC_THRU_SEP;
        pA->Thru[pA->nThru].RF   = 0;
        pA->nThru++;
        for ( k = Beg; k <= *pi; k++ )
        {
            pA->Thru[pA->nThru].Text = Msta_NameId(argv[k]);
            pA->Thru[pA->nThru].Kind = Msta_SdcKindOf(argv[k]);
            pA->Thru[pA->nThru].RF   = RF;
            pA->nThru++;
        }
    }
    return 1;
}

/* 把解析出的 -from/-to/-through 组合写进例外表：from 与 to 取笛卡尔积。 */
static void Msta_SdcAddPathExceptions( MstaSdc *p, MstaSdcPathArgs *pA,
                                       char **pArgv, int fFalse,
                                       int fSetup, int fHold, int nCycles,
                                       int fMaxDelay, int fMinDelay, double Delay )
{
    int j, k;
    int iFromBeg = pA->iFromBeg, iToBeg = pA->iToBeg;
    if ( iFromBeg < 0 ) iFromBeg = pA->iFromEnd = 0;
    if ( iToBeg < 0 ) iToBeg = pA->iToEnd = 0;
    for ( j = iFromBeg; j < (pA->iFromEnd > iFromBeg ? pA->iFromEnd : iFromBeg + 1); j++ )
        for ( k = iToBeg; k < (pA->iToEnd > iToBeg ? pA->iToEnd : iToBeg + 1); k++ )
        {
            MstaException *pEx = MstaExceptionArrayAppend(&p->vExceptions);
            const char *pFrom = j ? pArgv[j] : NULL;
            const char *pTo = k ? pArgv[k] : NULL;
            pEx->FromText = pFrom ? Msta_NameId(pFrom) : MSTA_NO_ID;
            pEx->ToText = pTo ? Msta_NameId(pTo) : MSTA_NO_ID;
            pEx->FromKind = pFrom ? Msta_SdcKindOf(pFrom) : 0;
            pEx->ToKind = pTo ? Msta_SdcKindOf(pTo) : 0;
            pEx->FromRF = pA->FromRF;
            pEx->ToRF = pA->ToRF;
            memcpy( pEx->Thru, pA->Thru, sizeof(pA->Thru) );
            pEx->nThru = pA->nThru;
            pEx->nSetupCycles = nCycles;
            /* 只在写了 -hold 时用（fApplyHold）：这时命令里的数字 M 不是周期数，
               而是 hold 沿从（由 setup 推出的）默认位置往回拉的拍数。 */
            pEx->nHoldShift = nCycles;
            pEx->fApplySetup = !fFalse && ( !fHold || fSetup );
            pEx->fApplyHold = !fFalse && fHold;
            pEx->fFalseSetup = fFalse && ( fSetup || !fHold );
            pEx->fFalseHold = fFalse && ( fHold || !fSetup );
            if ( fMaxDelay ) { pEx->fMaxDelay = 1; pEx->MaxDelay = Delay; }
            if ( fMinDelay ) { pEx->fMinDelay = 1; pEx->MinDelay = Delay; }
        }
}

/* set_false_path / set_multicycle_path：-from/-to/-through 集合 + 周期数。 */
static void Msta_SdcSetException( MstaSdc *p, int fFalse, int argc, char **argv )
{
    MstaSdcPathArgs A;
    int i, fSetup = 0, fHold = 0, fRise = 0, fFall = 0, nCycles = 1, fHaveCycles = 0;
    memset( &A, 0, sizeof(A) );
    A.iFromBeg = A.iToBeg = -1;
    for ( i = 1; i < argc; i++ )
    {
        int n = 0;
        if ( !strcmp(argv[i],"-setup") ) { fSetup = 1; continue; }
        if ( !strcmp(argv[i],"-hold") )  { fHold  = 1; continue; }
        if ( !strcmp(argv[i],"-rise") )  { fRise  = 1; continue; }
        if ( !strcmp(argv[i],"-fall") )  { fFall  = 1; continue; }
        if ( argv[i][0] != '-' )
        {
            if ( !fFalse && !fHaveCycles && Msta_SdcIsNumber(argv[i]) )
            { nCycles = atoi(argv[i]); fHaveCycles = 1; }
            continue;
        }
        n = Msta_SdcPathCollection( argc, argv, &i, &A );
        if ( n < 0 ) return;
        if ( n == 0 )
        {
            Msta_WarnOnce("path exception option \"%s\" is not modeled; constraint rejected",argv[i]);
            p->nCommandsIgnored++;
            return;
        }
    }
    if ( fRise && fFall )
    { Msta_WarnOnce("path exception cannot combine -rise and -fall; constraint rejected"); return; }
    /* 裸 -rise/-fall 是 -to 点的边沿限定。 */
    if ( ( fRise || fFall ) && A.ToRF == 0 ) A.ToRF = fRise ? 'r' : 'f';
    if ( !fFalse && nCycles < 1 )
    { Msta_WarnOnce("set_multicycle_path needs a positive cycle count"); return; }
    Msta_SdcAddPathExceptions( p, &A, argv, fFalse, fSetup, fHold, nCycles, 0, 0, 0.0 );
}

/* set_max_delay / set_min_delay：-from/-to/-through 集合 + 预算值。 */
static void Msta_SdcSetPathDelay( MstaSdc *p, int fMax, int argc, char **argv )
{
    MstaSdcPathArgs A;
    int i, n;
    double Delay = MSTA_UNSET;
    memset( &A, 0, sizeof(A) );
    A.iFromBeg = A.iToBeg = -1;
    for ( i = 1; i < argc; i++ )
    {
        if ( argv[i][0] != '-' )
        {
            if ( Msta_SdcIsNumber(argv[i]) ) Delay = Msta_SdcToPs(p,argv[i]);
            continue;
        }
        n = Msta_SdcPathCollection( argc, argv, &i, &A );
        if ( n < 0 ) return;
        if ( n == 0 )
        {
            Msta_WarnOnce("set_%s_delay option \"%s\" is not modeled; constraint rejected",
                          fMax ? "max" : "min", argv[i]);
            p->nCommandsIgnored++;
            return;
        }
    }
    if ( !Msta_IsSet(Delay) )
    { Msta_WarnOnce("set_%s_delay needs a numeric delay",fMax ? "max" : "min"); return; }
    Msta_SdcAddPathExceptions( p, &A, argv, 0, 0, 0, 1, fMax, !fMax, Delay );
}

/* group_path：把命中的路径归到一个分组里，报告按组统计 WNS/TNS。
   分组不影响 slack；-weight 只记录并在报告里显示，不参与 WNS/TNS 等数字的计算。
   -name 与 -default 互斥；两个都没写、或 -from/-to/-through 全空的分组没有意义。 */
static void Msta_SdcGroupPath( MstaSdc *p, int argc, char **argv )
{
    MstaSdcPathArgs A;
    MstaPathGroup *pGroup;
    const char *pName = NULL, *pWeight = NULL;
    int i, n, fDefault = 0;
    memset( &A, 0, sizeof(A) );
    A.iFromBeg = A.iToBeg = -1;
    for ( i = 1; i < argc; i++ )
    {
        if ( !strcmp(argv[i],"-default") ) { fDefault = 1; continue; }
        if ( !strcmp(argv[i],"-name") )
        {
            if ( i + 1 >= argc ) { Msta_WarnOnce("group_path -name needs a group name"); return; }
            pName = argv[++i];
            continue;
        }
        if ( !strcmp(argv[i],"-weight") )
        {
            if ( i + 1 >= argc ) { Msta_WarnOnce("group_path -weight needs a number"); return; }
            pWeight = argv[++i];
            continue;
        }
        if ( !strcmp(argv[i],"-critical_range") )
        {
            if ( i + 1 < argc ) i++;
            Msta_WarnOnce("group_path -critical_range is not modeled; the group takes every matching path");
            continue;
        }
        if ( argv[i][0] != '-' ) continue;
        n = Msta_SdcPathCollection( argc, argv, &i, &A );
        if ( n < 0 ) return;
        if ( n == 0 )
        {
            Msta_WarnOnce("group_path option \"%s\" is not modeled; group rejected", argv[i]);
            p->nCommandsIgnored++;
            return;
        }
    }
    if ( fDefault && pName != NULL )
    {
        Msta_WarnOnce("group_path -name and -default are mutually exclusive; group rejected");
        return;
    }
    if ( !fDefault && pName == NULL )
    {
        Msta_WarnOnce("group_path needs -name or -default; group rejected");
        return;
    }
    /* 不写 -from/-to/-through 就是"所有路径"（-default 常这么用）。 */
    /* -from/-to 的多个对象按笛卡尔积摊成多条记录，名字与权重相同（与例外一致）。 */
    {
        int iFromBeg = A.iFromBeg, iToBeg = A.iToBeg, j, k;
        int iFromEnd = ( A.iFromBeg >= 0 ) ? A.iFromEnd : 0;
        int iToEnd   = ( A.iToBeg   >= 0 ) ? A.iToEnd   : 0;
        if ( A.iFromBeg < 0 ) iFromBeg = iFromEnd = 0;
        if ( A.iToBeg   < 0 ) iToBeg   = iToEnd   = 0;
        for ( j = iFromBeg; j < ( iFromEnd > iFromBeg ? iFromEnd : iFromBeg + 1 ); j++ )
            for ( k = iToBeg; k < ( iToEnd > iToBeg ? iToEnd : iToBeg + 1 ); k++ )
            {
                const char *pFrom = j ? argv[j] : NULL;
                const char *pTo   = k ? argv[k] : NULL;
                if ( pWeight != NULL && !Msta_SdcIsNumber(pWeight) )
                {
                    Msta_WarnOnce( "group_path -weight \"%s\" is not a number; using 1.0", pWeight );
                    pWeight = NULL;
                }
                pGroup = MstaPathGroupArrayAppend( &p->vPathGroups );
                pGroup->Name    = fDefault ? MSTA_NO_ID : Msta_NameId( pName );
                pGroup->fDefault= fDefault;
                pGroup->Weight  = ( pWeight != NULL && atof(pWeight) > 0.0 ) ? atof(pWeight) : 1.0;
                pGroup->FromText= pFrom ? Msta_NameId(pFrom) : MSTA_NO_ID;
                pGroup->ToText  = pTo   ? Msta_NameId(pTo)   : MSTA_NO_ID;
                pGroup->FromKind= pFrom ? Msta_SdcKindOf(pFrom) : 0;
                pGroup->ToKind  = pTo   ? Msta_SdcKindOf(pTo)   : 0;
                pGroup->FromRF  = A.FromRF;
                pGroup->ToRF    = A.ToRF;
                memcpy( pGroup->Thru, A.Thru, sizeof(A.Thru) );
                pGroup->nThru   = A.nThru;
            }
    }
}

/* set_max_time_borrow：锁存器 D 脚允许比使能脚关闭沿晚到多久。
   不写对象列表就是所有锁存器的默认值（手册里对象列表是必写的，这里放宽）。 */
static void Msta_SdcSetMaxTimeBorrow( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib,
                                      int argc, char **argv )
{
    MstaSdcArgs A;
    char **pRest = Msta_SdcArgBuffer( argc );
    int nRest, i;
    double Value = MSTA_UNSET;
    Msta_SdcArgsStart( &A, argc );
    nRest = Msta_SdcRest( argc, argv, &A, pRest, argc );
    if ( nRest == 0 || !Msta_SdcIsNumber(pRest[0]) )
    { Msta_WarnOnce("set_max_time_borrow needs a numeric delay and latch objects"); return; }
    Value = Msta_SdcToPs( p, pRest[0] );
    if ( Value < 0.0 )
    { Msta_WarnOnce("set_max_time_borrow needs a non-negative delay"); return; }
    if ( nRest == 1 )
    {
        MstaBorrowSdc *pRec = MstaBorrowSdcArrayAppend( &p->vBorrow );
        pRec->Inst = -1;
        pRec->Value = Value;
        return;
    }
    for ( i = 1; i < nRest; i++ )
    {
        int nInst = Msta_DesignFindInstByName( pDes, pRest[i] );
        MstaBorrowSdc *pRec;
        if ( nInst < 0 )
        {
            MstaCell *pCell = Msta_LibFindCell( pLib, pRest[i] );
            int k;
            if ( pCell == NULL )
            { Msta_WarnOnce("set_max_time_borrow: unknown cell \"%s\"", pRest[i]); continue; }
            for ( k = 0; k < pDes->vInsts.nSize; k++ )
                if ( MstaInstArrayAt(&pDes->vInsts,k)->pCell == pCell )
                {
                    pRec = MstaBorrowSdcArrayAppend( &p->vBorrow );
                    pRec->Inst = k;
                    pRec->Value = Value;
                }
            continue;
        }
        pRec = MstaBorrowSdcArrayAppend( &p->vBorrow );
        pRec->Inst = nInst;
        pRec->Value = Value;
    }
}

/* set_clock_gating_check：门控单元使能脚相对时钟脚的 setup/hold 值。
   没有对象列表时作为全局默认；带对象列表时按实例（或库单元名）覆盖。
   -rise/-fall/-high/-low 认下来，但检查对象仍按库里声明的有效沿（不一致时告警）。 */
static void Msta_SdcSetClockGatingCheck( MstaSdc *p, MstaDesign *pDes,
                                         MstaLib *pLib, int argc, char **argv )
{
    MstaSdcArgs A;
    const char *pSetup, *pHold;
    MstaClockGatingSdc *pRec;
    double Setup = MSTA_UNSET, Hold = MSTA_UNSET;
    int i;
    Msta_SdcArgsStart( &A, argc );
    pSetup = Msta_SdcValueOf(argc,argv,&A,"-setup");
    pHold  = Msta_SdcValueOf(argc,argv,&A,"-hold");
    for ( i = 1; i < argc; i++ )
    {
        if ( A.Used[i] ) continue;
        if ( !strcmp(argv[i],"-rise") || !strcmp(argv[i],"-fall") ||
             !strcmp(argv[i],"-high") || !strcmp(argv[i],"-low") )
        {
            A.Used[i] = 1;
            Msta_WarnOnce( "set_clock_gating_check %s is not modeled; the check "
                           "always uses the edge the library declares", argv[i] );
        }
    }
    if ( pSetup != NULL )
    {
        if ( !Msta_SdcIsNumber(pSetup) )
        { Msta_WarnOnce("set_clock_gating_check -setup needs a number"); return; }
        Setup = Msta_SdcToPs( p, pSetup );
    }
    if ( pHold != NULL )
    {
        if ( !Msta_SdcIsNumber(pHold) )
        { Msta_WarnOnce("set_clock_gating_check -hold needs a number"); return; }
        Hold = Msta_SdcToPs( p, pHold );
    }
    if ( !Msta_IsSet(Setup) && !Msta_IsSet(Hold) )
    { Msta_WarnOnce("set_clock_gating_check needs -setup and/or -hold"); return; }

    /* 没写对象列表 = 全局默认（手册里的 object_list 可以省略）。 */
    {
        char **pRest = Msta_SdcArgBuffer( argc );
        int nRest = Msta_SdcRest( argc, argv, &A, pRest, argc );
        if ( nRest == 0 )
        {
            pRec = MstaClockGatingSdcArrayAppend( &p->vClockGating );
            pRec->Inst  = -1;
            pRec->Setup = Setup;
            pRec->Hold  = Hold;
            pRec->fRise = pRec->fFall = pRec->fHigh = pRec->fLow = 0;
            return;
        }
        for ( i = 0; i < nRest; i++ )
        {
            int nInst = Msta_DesignFindInstByName( pDes, pRest[i] );
            if ( nInst < 0 )
            {
                MstaCell *pCell = Msta_LibFindCell( pLib, pRest[i] );
                int k;
                if ( pCell == NULL )
                {
                    Msta_WarnOnce( "set_clock_gating_check: unknown cell \"%s\"", pRest[i] );
                    continue;
                }
                for ( k = 0; k < pDes->vInsts.nSize; k++ )
                    if ( MstaInstArrayAt(&pDes->vInsts,k)->pCell == pCell )
                    {
                        pRec = MstaClockGatingSdcArrayAppend( &p->vClockGating );
                        pRec->Inst  = k;
                        pRec->Setup = Setup;
                        pRec->Hold  = Hold;
                        pRec->fRise = pRec->fFall = pRec->fHigh = pRec->fLow = 0;
                    }
                continue;
            }
            pRec = MstaClockGatingSdcArrayAppend( &p->vClockGating );
            pRec->Inst  = nInst;
            pRec->Setup = Setup;
            pRec->Hold  = Hold;
            pRec->fRise = pRec->fFall = pRec->fHigh = pRec->fLow = 0;
        }
    }
}

/* 把选中的网络钉成常量。 */
static void Msta_SdcSetCaseAnalysis( MstaDesign *pDes, int argc, char **argv )
{
    MstaSdcArgs A;
    char **pRest = Msta_SdcArgBuffer( argc );
    int nRest, i, Value;
    Msta_SdcArgsStart( &A, argc );
    nRest = Msta_SdcRest(argc,argv,&A,pRest, argc );
    if ( nRest < 2 || !Msta_SdcIsNumber(pRest[0]) )
    {
        /* 手册还允许 rising / falling（只能沿该边沿翻转），msta 不建模。 */
        if ( nRest >= 2 )
            Msta_WarnOnce("set_case_analysis value \"%s\" is not modeled; only 0 and 1 are supported",
                          pRest[0]);
        else
            Msta_WarnOnce("set_case_analysis needs 0 or 1 and objects");
        return;
    }
    Value = atoi(pRest[0]) ? 2 : 1;   /* fCaseValue 编码：1 = 逻辑 0，2 = 逻辑 1（见 msta_net.h） */
    for ( i = 1; i < nRest; i++ )
    {
        int *pNets, nFound, j;
        nFound = Msta_SdcResolveNets( pDes, pRest[i], &pNets );
        for ( j = 0; j < nFound; j++ )
        {
            MstaNet *pNet = MstaNetArrayAt(&pDes->vNets,pNets[j]);
            pNet->fCaseValue = Value;
            pNet->fConst = Value;
        }
    }
}

/* set_logic_zero / set_logic_one / set_logic_dc：把端口固定成常量。
   dc 是 don't care：按"不传播"处理（路径到这里断开）。
   fCaseValue 编码：1 = 逻辑 0，2 = 逻辑 1，3 = dc（见 msta_net.h）。 */
static void Msta_SdcSetLogic( MstaSdc *p, MstaDesign *pDes, int nKind, int argc, char **argv )
{
    int i, j;
    (void)p;
    for ( i = 1; i < argc; i++ )
    {
        int *pNets, nFound;
        if ( argv[i][0] == '-' ) continue;
        nFound = Msta_SdcResolveNets( pDes, argv[i], &pNets );
        for ( j = 0; j < nFound; j++ )
        {
            MstaNet *pNet = MstaNetArrayAt( &pDes->vNets, pNets[j] );
            if ( nKind == 0 )      { pNet->fCaseValue = 1; pNet->fConst = 1; }   /* 逻辑 0 */
            else if ( nKind == 1 ) { pNet->fCaseValue = 2; pNet->fConst = 2; }   /* 逻辑 1 */
            else                   { pNet->fCaseValue = 3; pNet->fConst = 0; }   /* 无关值 */
        }
    }
}

/* set_disable_timing：屏蔽指定实例上从 -from 到 -to 的时序弧。
   两个端点可以只写一个，缺的那个按通配处理（与 SDC 一致）。 */
static void Msta_SdcSetDisableTiming( MstaSdc *p, MstaDesign *pDes, int argc, char **argv )
{
    MstaSdcArgs A;
    const char *pFrom, *pTo;
    char **pRest = Msta_SdcArgBuffer( argc );
    int nRest, i;
    Msta_SdcArgsStart( &A, argc );
    pFrom = Msta_SdcValueOf(argc,argv,&A,"-from");
    pTo = Msta_SdcValueOf(argc,argv,&A,"-to");
    nRest = Msta_SdcRest(argc,argv,&A,pRest, argc );
    if ( ( pFrom == NULL && pTo == NULL ) || nRest == 0 )
    { Msta_WarnOnce("set_disable_timing needs -from and/or -to plus cell objects"); return; }
    for ( i = 0; i < nRest; i++ )
    {
        int nInst = Msta_DesignFindInstByName(pDes,pRest[i]);
        if ( nInst < 0 ) { Msta_WarnOnce("set_disable_timing: unknown instance \"%s\"",pRest[i]); continue; }
        {
            MstaInst *pInst = MstaInstArrayAt(&pDes->vInsts,nInst);
            MstaDisabledArc *pArc = MstaDisabledArcArrayAppend(&p->vDisabledArcs);
            pArc->Inst = nInst;
            pArc->FromPin = pFrom ? Msta_NameId(pFrom) : MSTA_NO_ID;
            pArc->ToPin = pTo ? Msta_NameId(pTo) : MSTA_NO_ID;
            if ( ( pFrom && Msta_CellPinIndexOf(pInst->pCell,pArc->FromPin) < 0 ) ||
                 ( pTo && Msta_CellPinIndexOf(pInst->pCell,pArc->ToPin) < 0 ) )
                Msta_WarnOnce("set_disable_timing: instance \"%s\" has no matching pins",pRest[i]);
        }
    }
}

/* set_clock_groups -asynchronous|-logically_exclusive|-physically_exclusive
                    [-name 名字] -group 时钟列表 -group 时钟列表 ...
   不同时钟组之间没有 setup/hold 关系，两两展开成例外。-allow_paths 认得但不建模。
   这条命令按顺序模式读：-group 之后、下一个 -group 之前的时钟都归这一组。 */
static const MstaSdcOpt s_vClockGroupsOpts[] = {
    { "-asynchronous",         MSTA_SDC_FLAG      },
    { "-logically_exclusive",  MSTA_SDC_FLAG      },
    { "-physically_exclusive", MSTA_SDC_FLAG      },
    { "-name",                 MSTA_SDC_VALUE_RAW },
    { "-allow_paths",          MSTA_SDC_FLAG      },
    { "-group",                MSTA_SDC_GROUP     },
    { NULL,                    MSTA_SDC_FLAG      } };

static void Msta_SdcSetClockGroups( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    int *pGroups = (int *)calloc((size_t)(p->vClocks.nSize > 0 ? p->vClocks.nSize : 1),sizeof(int));
    int i, j, iStop = pCmd->argc, iAllow = Msta_SdcOptAt( pCmd, "-allow_paths" );
    int fMode = Msta_SdcHasFlag( pCmd, "-asynchronous" ) ||
                Msta_SdcHasFlag( pCmd, "-logically_exclusive" ) ||
                Msta_SdcHasFlag( pCmd, "-physically_exclusive" );
    assert(pGroups);
    /* 命令从左到右生效：碰到 -allow_paths、不认识的选项、或第一个 -group 之前的
       对象，整条作废；在那之前组里的时钟已经查过（未知时钟已经告警）。
       所以先求出最早的"停止位置"，只处理它前面的组成员。 */
    if ( iAllow > 0 )
        iStop = iAllow;
    if ( pCmd->iBad > 0 && pCmd->iBad < iStop )
        iStop = pCmd->iBad;
    if ( pCmd->nPos > 0 && pCmd->pPosAt[0] < iStop )
        iStop = pCmd->pPosAt[0];
    for ( i = 0; i < pCmd->nMembers && pCmd->pMemberAt[i] < iStop; i++ )
    {
        j = Msta_SdcClockIndexOf(p,Msta_NameId(pCmd->ppMember[i]));
        if ( j < 0 ) Msta_WarnOnce("set_clock_groups: unknown clock \"%s\"",pCmd->ppMember[i]);
        else pGroups[j] = pCmd->pMemberGroup[i];
    }
    if ( iStop < pCmd->argc )
    {
        if ( iStop == iAllow )
            Msta_WarnOnce("set_clock_groups -allow_paths is not modeled; constraint rejected");
        else
            Msta_WarnOnce("set_clock_groups: unexpected argument \"%s\"",pCmd->argv[iStop]);
        free(pGroups);
        return;
    }
    if ( !fMode || pCmd->nGroups < 2 )
        Msta_WarnOnce("set_clock_groups needs a mode and at least two -group lists");
    else
    {
        for ( i = 0; i < p->vClocks.nSize; i++ )
            for ( j = 0; j < p->vClocks.nSize; j++ )
            {
                MstaException *pEx;
                if ( pGroups[i] == 0 || pGroups[j] == 0 || pGroups[i] == pGroups[j] )
                    continue;
                pEx = MstaExceptionArrayAppend(&p->vExceptions);
                pEx->FromText = Msta_SdcClockByIndex(p,i)->Name;
                pEx->ToText = Msta_SdcClockByIndex(p,j)->Name;
                pEx->FromKind = pEx->ToKind = 'C';
                pEx->nSetupCycles = 1;
                pEx->fFalseSetup = pEx->fFalseHold = 1;
            }
    }
    free(pGroups);
}

/* =====================================================================
   读入与分发
   ===================================================================== */

static int Msta_SdcRunOne( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, int argc, char **argv );

/* 集合是否用了 -quiet：用了就不报 "没匹配到对象"。 */
static int Msta_SdcIsQuiet( const char *pText )
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
    if ( pL->nSize == nBeg )
        Msta_WarnOnce( "-of_objects matched no objects; command skipped" );
    return pL->nSize - nBeg;
}

static int Msta_SdcExpandRecord( MJsonValue *pRecord, MstaDesign *pDes, MstaSdc *pSdc )
{
    int i;
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
        if ( pText[0] == '\036' && pText[1] == 'A' )
        {
            if ( Msta_SdcExpandAll( pSdc, pDes, pText + 2, &s_Args ) < 0 )
            {
                Msta_WarnOnce( "sdc collection \"%s\" is not modeled; command skipped", pText + 2 );
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
        }
        else
            Msta_SdcArgListPush( &s_Args, pText, 0, 0 );
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
        Msta_SdcRunOne( p, pDes, pLib, argc, s_Args.ppText );
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

/* 分发表：已经写成选项表的命令。{ 命令名, 解析模式, 选项表, 处理函数 }。 */
static const MstaSdcCmdDef s_vSdcCommands[] = {
    { "create_clock",           MSTA_SDC_SCAN,    s_vCreateClockOpts,      Msta_SdcCreateClock          },
    { "create_generated_clock", MSTA_SDC_SCAN,    s_vGeneratedClockOpts,   Msta_SdcCreateGeneratedClock },
    { "set_clock_uncertainty",  MSTA_SDC_SCAN,    s_vClockUncertaintyOpts, Msta_SdcSetUncertainty       },
    { "set_clock_latency",      MSTA_SDC_SCAN,    s_vClockLatencyOpts,     Msta_SdcSetClockLatency      },
    { "set_propagated_clock",   MSTA_SDC_WORDS,   s_vPropagatedClockOpts,  Msta_SdcSetPropagatedClock   },
    { "set_clock_transition",   MSTA_SDC_SCAN,    s_vClockTransitionOpts,  Msta_SdcSetClockTransition   },
    { "set_clock_sense",        MSTA_SDC_SCAN,    s_vClockSenseOpts,       Msta_SdcSetClockSense        },
    { "set_clock_groups",       MSTA_SDC_ORDERED, s_vClockGroupsOpts,      Msta_SdcSetClockGroups       },
    { NULL,                     MSTA_SDC_SCAN,    NULL,                    NULL                         } };

static int Msta_SdcRunOne( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, int argc, char **argv )
{
    const char *pCmd = argv[0];
    int i;

    /* 分发表里的命令：先按选项表解析，再交给处理函数。 */
    for ( i = 0; s_vSdcCommands[i].pName; i++ )
        if ( !strcmp( pCmd, s_vSdcCommands[i].pName ) )
        {
            MstaSdcCmd Cmd;
            p->nCommandsRead++;
            Msta_SdcParseCmd( &s_vSdcCommands[i], argc, argv, &Cmd );
            s_vSdcCommands[i].pfHandler( p, pDes, pLib, &Cmd );
            return 1;
        }
    /* 其余命令还没有选项表，由各自的函数直接解析 argv。 */
    if      ( !strcmp(pCmd, "set_input_delay") )       p->nCommandsRead++, Msta_SdcSetPortDelay( p, pDes, 0, argc, argv );
    else if ( !strcmp(pCmd, "set_output_delay") )      p->nCommandsRead++, Msta_SdcSetPortDelay( p, pDes, 1, argc, argv );
    else if ( !strcmp(pCmd, "set_load") )              p->nCommandsRead++, Msta_SdcSetLoad( p, pDes, pLib, argc, argv );
    else if ( !strcmp(pCmd, "set_input_transition") )  p->nCommandsRead++, Msta_SdcSetInputSlew( p, pDes, pLib, argc, argv );
    else if ( !strcmp(pCmd, "set_driving_cell") )      p->nCommandsRead++, Msta_SdcSetDrivingCell( p, pDes, pLib, argc, argv );
    else if ( !strcmp(pCmd, "set_false_path") )        p->nCommandsRead++, Msta_SdcSetException( p, 1, argc, argv );
    else if ( !strcmp(pCmd, "set_multicycle_path") )   p->nCommandsRead++, Msta_SdcSetException( p, 0, argc, argv );
    else if ( !strcmp(pCmd, "set_max_delay") )         p->nCommandsRead++, Msta_SdcSetPathDelay( p, 1, argc, argv );
    else if ( !strcmp(pCmd, "set_min_delay") )         p->nCommandsRead++, Msta_SdcSetPathDelay( p, 0, argc, argv );
    else if ( !strcmp(pCmd, "set_case_analysis") )     p->nCommandsRead++, Msta_SdcSetCaseAnalysis( pDes, argc, argv );
    else if ( !strcmp(pCmd, "set_logic_zero") )        p->nCommandsRead++, Msta_SdcSetLogic( p, pDes, 0, argc, argv );
    else if ( !strcmp(pCmd, "set_logic_one") )         p->nCommandsRead++, Msta_SdcSetLogic( p, pDes, 1, argc, argv );
    else if ( !strcmp(pCmd, "set_logic_dc") )          p->nCommandsRead++, Msta_SdcSetLogic( p, pDes, 2, argc, argv );
    else if ( !strcmp(pCmd, "set_disable_timing") )    p->nCommandsRead++, Msta_SdcSetDisableTiming( p, pDes, argc, argv );
    else if ( !strcmp(pCmd, "set_ideal_network") )     p->nCommandsRead++, Msta_SdcSetIdealNetwork( p, pDes, argc, argv );
    else if ( !strcmp(pCmd, "set_ideal_latency") )     p->nCommandsRead++, Msta_SdcSetIdealLatency( p, pDes, argc, argv );
    else if ( !strcmp(pCmd, "set_ideal_transition") )  p->nCommandsRead++, Msta_SdcSetIdealTransition( p, pDes, argc, argv );
    else if ( !strcmp(pCmd, "set_timing_derate") )     p->nCommandsRead++, Msta_SdcSetTimingDerate( p, pDes, argc, argv );
    else if ( !strcmp(pCmd, "set_max_transition") )    p->nCommandsRead++, Msta_SdcSetDrcLimit( p, pDes, pLib, 0, argc, argv );
    else if ( !strcmp(pCmd, "set_max_fanout") )        p->nCommandsRead++, Msta_SdcSetDrcLimit( p, pDes, pLib, 1, argc, argv );
    else if ( !strcmp(pCmd, "set_max_capacitance") )   p->nCommandsRead++, Msta_SdcSetDrcLimit( p, pDes, pLib, 2, argc, argv );
    else if ( !strcmp(pCmd, "set_min_capacitance") )   p->nCommandsRead++, Msta_SdcSetDrcLimit( p, pDes, pLib, 3, argc, argv );
    else if ( !strcmp(pCmd, "set_max_area") )          p->nCommandsRead++, Msta_SdcSetMaxArea( p, argc, argv );
    else if ( !strcmp(pCmd, "set_data_check") )        p->nCommandsRead++, Msta_SdcSetDataCheck( p, pDes, argc, argv );
    else if ( !strcmp(pCmd, "group_path") )            p->nCommandsRead++, Msta_SdcGroupPath( p, argc, argv );
    else if ( !strcmp(pCmd, "set_clock_gating_check") ) p->nCommandsRead++, Msta_SdcSetClockGatingCheck( p, pDes, pLib, argc, argv );
    else if ( !strcmp(pCmd, "set_max_time_borrow") )   p->nCommandsRead++, Msta_SdcSetMaxTimeBorrow( p, pDes, pLib, argc, argv );
    else if ( !strcmp(pCmd, "set_operating_conditions") ) p->nCommandsRead++, Msta_SdcSetOperatingConditions( p, pLib, argc, argv );
    else if ( !strcmp(pCmd, "set_voltage") )           p->nCommandsRead++, Msta_SdcSetVoltage( p, pDes, pLib, argc, argv );
    else if ( !strcmp(pCmd, "set_units") )             p->nCommandsRead++, Msta_SdcSetUnits( p, argc, argv );
    else
    {
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
    return 1;
}

/* =====================================================================
   查询
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

MstaClock *Msta_SdcFindClock( MstaSdc *p, const char *pName )
{
    int i = Msta_IntMapGet( &p->clockMap, Msta_NameId(pName), -1 );
    return i < 0 ? NULL : MstaClockArrayAt( &p->vClocks, i );
}

MstaClock *Msta_SdcClockByIndex( MstaSdc *p, int i )
{
    return ( i >= 0 && i < p->vClocks.nSize ) ? MstaClockArrayAt( &p->vClocks, i ) : NULL;
}

int Msta_SdcClockCount( MstaSdc *p )
{
    return p->vClocks.nSize;
}

int Msta_SdcClockIndexOf( MstaSdc *p, MstaId NameId )
{
    if ( NameId == MSTA_NO_ID )
        return -1;
    return Msta_IntMapGet( &p->clockMap, NameId, -1 );
}

MstaNetCons *Msta_SdcNetCons( MstaSdc *p, int nNet )
{
    int i = Msta_IntMapGet( &p->netConsMap, nNet, -1 );
    return i < 0 ? NULL : MstaNetConsArrayAt( &p->vNets, i );
}

MstaNetCons *Msta_SdcNetConsOrCreate( MstaSdc *p, int nNet )
{
    MstaNetCons *pCons;
    int i = Msta_IntMapGet( &p->netConsMap, nNet, -1 );
    if ( i >= 0 )
        return MstaNetConsArrayAt( &p->vNets, i );
    pCons = MstaNetConsArrayAppend( &p->vNets );
    pCons->Net     = nNet;
    pCons->LoadMax = pCons->LoadMin = MSTA_UNSET;
    pCons->InputSlewMaxRise = pCons->InputSlewMaxFall = MSTA_UNSET;
    pCons->InputSlewMinRise = pCons->InputSlewMinFall = MSTA_UNSET;
    pCons->DrivingCell = pCons->DrivingPin = pCons->DrivingFromPin = MSTA_NO_ID;
    pCons->DriveInSlewRise = pCons->DriveInSlewFall = MSTA_UNSET;
    pCons->DrcMaxTransition = pCons->DrcMaxCapacitance = pCons->DrcMaxFanout = MSTA_UNSET;
    pCons->DrcMinCapacitance = MSTA_UNSET;
    pCons->IdealLatencyMaxRise = pCons->IdealLatencyMaxFall = MSTA_UNSET;
    pCons->IdealLatencyMinRise = pCons->IdealLatencyMinFall = MSTA_UNSET;
    pCons->IdealTranMaxRise = pCons->IdealTranMaxFall = MSTA_UNSET;
    pCons->IdealTranMinRise = pCons->IdealTranMinFall = MSTA_UNSET;
    pCons->DriveMultiply = 1.0;
    Msta_IntMapSet( &p->netConsMap, nNet, p->vNets.nSize - 1 );
    return pCons;
}

/* set_ideal_network 标过的网络。 */
int Msta_SdcIsIdealNet( MstaSdc *p, int nNet )
{
    MstaNetCons *pCons = Msta_SdcNetCons( p, nNet );
    return pCons != NULL && pCons->fIdeal;
}

/* 该网络的理想属性不往下游传（set_ideal_network -no_propagate）。 */
int Msta_SdcIdealNoPropagate( MstaSdc *p, int nNet )
{
    MstaNetCons *pCons = Msta_SdcNetCons( p, nNet );
    return pCons != NULL && pCons->fIdealNoPropagate;
}

/* 一个理想量的 rise/fall 与 min/max 四个格子：fRise=1/0 取单个边沿，
   fRise=-1 取该分析角最保守的那个（max 角取大、min 角取小）。 */
static double Msta_SdcPickIdeal( double MaxRise, double MaxFall,
                                 double MinRise, double MinFall, int fMax, int fRise )
{
    double Rise = fMax ? MaxRise : MinRise;
    double Fall = fMax ? MaxFall : MinFall;
    if ( fRise == 1 ) return Rise;
    if ( fRise == 0 ) return Fall;
    if ( !Msta_IsSet(Rise) ) return Fall;
    if ( !Msta_IsSet(Fall) ) return Rise;
    return fMax ? ( Rise > Fall ? Rise : Fall ) : ( Rise < Fall ? Rise : Fall );
}

double Msta_SdcIdealLatency( MstaSdc *p, int nNet, int fMax, int fRise )
{
    MstaNetCons *pCons = Msta_SdcNetCons( p, nNet );
    if ( pCons == NULL ) return MSTA_UNSET;
    return Msta_SdcPickIdeal( pCons->IdealLatencyMaxRise, pCons->IdealLatencyMaxFall,
                              pCons->IdealLatencyMinRise, pCons->IdealLatencyMinFall,
                              fMax, fRise );
}

double Msta_SdcIdealTransition( MstaSdc *p, int nNet, int fMax, int fRise )
{
    MstaNetCons *pCons = Msta_SdcNetCons( p, nNet );
    if ( pCons == NULL ) return MSTA_UNSET;
    return Msta_SdcPickIdeal( pCons->IdealTranMaxRise, pCons->IdealTranMaxFall,
                              pCons->IdealTranMinRise, pCons->IdealTranMinFall,
                              fMax, fRise );
}

/* 这根网络上的时钟极性：专门的 (网络, 时钟) 条目优先，其次是对所有时钟生效的条目。 */
int Msta_SdcClockSense( MstaSdc *p, int nNet, int nClock, int *pfStop )
{
    MstaClock *pClock = Msta_SdcClockByIndex( p, nClock );
    MstaClockSense *pAny = NULL;
    int i;
    if ( pfStop ) *pfStop = 0;
    for ( i = 0; i < p->vClockSense.nSize; i++ )
    {
        MstaClockSense *pSense = MstaClockSenseArrayAt( &p->vClockSense, i );
        if ( pSense->Net != nNet )
            continue;
        if ( pSense->Clock == MSTA_NO_ID )
        {
            if ( pAny == NULL ) pAny = pSense;
            continue;
        }
        if ( pClock != NULL && pSense->Clock == pClock->Name )
        {
            if ( pfStop ) *pfStop = pSense->fStop;
            return pSense->Polarity;
        }
    }
    if ( pAny != NULL )
    {
        if ( pfStop ) *pfStop = pAny->fStop;
        return pAny->Polarity;
    }
    return 0;
}

/* 是否设过任何 DRC 限制（全局或分网络）。 */
int Msta_SdcHasDrcLimits( MstaSdc *p )
{
    int i;
    if ( Msta_IsSet(p->MaxTransition) || Msta_IsSet(p->MaxCapacitance) ||
         Msta_IsSet(p->MaxFanout) || Msta_IsSet(p->MinCapacitance) )
        return 1;
    for ( i = 0; i < p->vNets.nSize; i++ )
    {
        MstaNetCons *pCons = MstaNetConsArrayAt( &p->vNets, i );
        if ( Msta_IsSet(pCons->DrcMaxTransition) || Msta_IsSet(pCons->DrcMaxCapacitance) ||
             Msta_IsSet(pCons->DrcMaxFanout) || Msta_IsSet(pCons->DrcMinCapacitance) )
            return 1;
    }
    return 0;
}

/* 一条检查的不确定度：-from/-to 专用条目优先，其余用捕获时钟的标量值。 */
double Msta_SdcClockUncertainty( MstaSdc *p, int nLaunchClock, int nCaptureClock,
                                 int fSetup, int fLaunchRises, int fCaptureRises )
{
    MstaClock *pCapture = Msta_SdcClockByIndex(p,nCaptureClock);
    int i;
    for ( i = 0; i < p->vInterClockUnc.nSize; i++ )
    {
        MstaInterClockUnc *pUnc = MstaInterClockUncArrayAt(&p->vInterClockUnc,i);
        MstaClock *pLaunch = Msta_SdcClockByIndex( p, nLaunchClock );
        MstaId From = pLaunch ? pLaunch->Name : MSTA_NO_ID;
        if ( fSetup ? !pUnc->fSetup : !pUnc->fHold )
            continue;
        if ( pUnc->FromClock != MSTA_NO_ID && pUnc->FromClock != From )
            continue;
        if ( pUnc->ToClock != MSTA_NO_ID && pCapture &&
             pUnc->ToClock != pCapture->Name )
            continue;
        /* 边沿限定只在边沿已知时才生效，未知就退回标量值。 */
        if ( pUnc->FromRF != 0 &&
             ( fLaunchRises < 0 || pUnc->FromRF != (fLaunchRises ? 'r' : 'f') ) )
            continue;
        if ( pUnc->ToRF != 0 &&
             ( fCaptureRises < 0 || pUnc->ToRF != (fCaptureRises ? 'r' : 'f') ) )
            continue;
        return pUnc->Value;
    }
    if ( pCapture == NULL )
        return 0.0;
    return fSetup ? pCapture->UncertaintySetup : pCapture->UncertaintyHold;
}

/* 全局 derate：fMax=1 用 late 系数，fMax=0 用 early 系数。 */
double Msta_SdcDataDerate( MstaSdc *p, int fMax )
{
    if ( !p->fDerateData )
        return fMax ? p->KFactorDerateLate : p->KFactorDerateEarly;
    return ( fMax ? p->DerateLate : p->DerateEarly ) *
           ( fMax ? p->KFactorDerateLate : p->KFactorDerateEarly );
}

double Msta_SdcClockDerate( MstaSdc *p, int fMax )
{
    if ( !p->fDerateClock )
        return fMax ? p->KFactorDerateLate : p->KFactorDerateEarly;
    return ( fMax ? p->DerateLate : p->DerateEarly ) *
           ( fMax ? p->KFactorDerateLate : p->KFactorDerateEarly );
}

double Msta_SdcCheckDerate( MstaSdc *p, int fMax )
{
    return ( fMax ? p->DerateCheckLate : p->DerateCheckEarly ) *
           ( fMax ? p->KFactorDerateLate : p->KFactorDerateEarly );
}

/* 分对象 derate 查表：实例上的按实例号比，时钟上的按时钟名比；
   同一条记录里只写了一个边沿时，另一个边沿不生效。多条形如
   "同一对象写了两条" 时取最后一个（与后写覆盖先写一致）。 */
static int Msta_SdcObjDerateValue( MstaSdc *p, char Kind, int nInst, MstaId Name,
                                   int fCellCheck, int fMax, int fRise, double *pValue )
{
    int i, fFound = 0;
    for ( i = 0; i < p->vObjDerate.nSize; i++ )
    {
        MstaObjDerate *pRec = MstaObjDerateArrayAt( &p->vObjDerate, i );
        if ( pRec->Kind != Kind || pRec->fCellCheck != fCellCheck )
            continue;
        if ( Kind == 'I' ? ( pRec->Inst != nInst ) : ( pRec->Name != Name ) )
            continue;
        if ( fRise >= 0 && ( ( fRise && pRec->fFall ) || ( !fRise && pRec->fRise ) ) )
            continue;                       /* 这条记录只管另一个边沿 */
        *pValue = fMax ? pRec->Late : pRec->Early;
        fFound = 1;
    }
    return fFound;
}

double Msta_SdcDataDerateInst( MstaSdc *p, int nInst, int fMax, int fRise )
{
    double Value;
    if ( Msta_SdcObjDerateValue( p, 'I', nInst, MSTA_NO_ID, 0, fMax, fRise, &Value ) )
        return Value * ( fMax ? p->KFactorDerateLate : p->KFactorDerateEarly );
    return Msta_SdcDataDerate( p, fMax );
}

double Msta_SdcCheckDerateInst( MstaSdc *p, int nInst, int fMax )
{
    double Value;
    if ( Msta_SdcObjDerateValue( p, 'I', nInst, MSTA_NO_ID, 1, fMax, -1, &Value ) )
        return Value * ( fMax ? p->KFactorDerateLate : p->KFactorDerateEarly );
    return Msta_SdcCheckDerate( p, fMax );
}

double Msta_SdcClockDerateClock( MstaSdc *p, int nClock, int fMax )
{
    MstaClock *pClock = Msta_SdcClockByIndex( p, nClock );
    double Value;
    if ( pClock != NULL &&
         Msta_SdcObjDerateValue( p, 'C', -1, pClock->Name, 0, fMax, -1, &Value ) )
        return Value * ( fMax ? p->KFactorDerateLate : p->KFactorDerateEarly );
    return Msta_SdcClockDerate( p, fMax );
}

int Msta_SdcTimingDisabled( MstaSdc *p, int nInst, MstaId FromPin, MstaId ToPin )
{
    int i;
    for ( i = 0; i < p->vDisabledArcs.nSize; i++ )
    {
        MstaDisabledArc *pArc = MstaDisabledArcArrayAt(&p->vDisabledArcs,i);
        if ( pArc->Inst != nInst )
            continue;
        /* MSTA_NO_ID = 这条约束没写那一端，按通配匹配。 */
        if ( pArc->FromPin != MSTA_NO_ID && pArc->FromPin != FromPin )
            continue;
        if ( pArc->ToPin != MSTA_NO_ID && pArc->ToPin != ToPin )
            continue;
        if ( pArc->FromPin != MSTA_NO_ID || pArc->ToPin != MSTA_NO_ID )
            return 1;
    }
    return 0;
}

/* 合并 I/O rise/fall 约束：max 取较大延迟，min 取较小延迟，避免结果依赖命令顺序。 */
static MstaIoDelay *Msta_SdcFindPortDelay( MstaSdc *p, int nNet, int nClock,
                                          int fOutput, int fMax )
{
    MstaIoDelayArray *pArr = fOutput ? &p->vOutputDelays : &p->vInputDelays;
    MstaClock *pClock = Msta_SdcClockByIndex(p,nClock);
    MstaIoDelay *pBest = NULL;
    double Best = MSTA_UNSET;
    int i;
    if ( pClock == NULL ) return NULL;
    for ( i = 0; i < pArr->nSize; i++ )
    {
        MstaIoDelay *pEntry = MstaIoDelayArrayAt(pArr,i);
        if ( pEntry->Net != nNet ) continue;
        if ( pEntry->Clock != pClock->Name &&
             !(pEntry->Clock == MSTA_NO_ID &&
               (p->vClocks.nSize == 1 ||
                (fMax ? pEntry->RefNetMax : pEntry->RefNetMin) >= 0)) )
            continue;
        {
            double Value = fMax ? pEntry->Max : pEntry->Min;
            if ( !Msta_IsSet(Value) ) continue;
            if ( pBest == NULL || (fMax ? Value > Best : Value < Best) )
            { pBest = pEntry; Best = Value; }
        }
    }
    return pBest;
}

double Msta_SdcPortDelay( MstaSdc *p, int nNet, int nClock,
                          int fOutput, int fMax )
{
    MstaIoDelay *pEntry = Msta_SdcFindPortDelay(p,nNet,nClock,fOutput,fMax);
    return pEntry ? (fMax ? pEntry->Max : pEntry->Min) : MSTA_UNSET;
}

int Msta_SdcPortReferenceNet( MstaSdc *p, int nNet, int nClock,
                              int fOutput, int fMax )
{
    MstaIoDelay *pEntry = Msta_SdcFindPortDelay(p,nNet,nClock,fOutput,fMax);
    return pEntry ? (fMax ? pEntry->RefNetMax : pEntry->RefNetMin) : -1;
}

int Msta_SdcPortSourceLatencyIncluded( MstaSdc *p, int nNet, int nClock,
                                       int fOutput, int fMax )
{
    MstaIoDelay *pEntry = Msta_SdcFindPortDelay(p,nNet,nClock,fOutput,fMax);
    return pEntry && (fMax ? pEntry->SourceLatencyIncludedMax
                           : pEntry->SourceLatencyIncludedMin);
}

int Msta_SdcPortNetworkLatencyIncluded( MstaSdc *p, int nNet, int nClock,
                                        int fOutput, int fMax )
{
    MstaIoDelay *pEntry = Msta_SdcFindPortDelay(p,nNet,nClock,fOutput,fMax);
    return pEntry && (fMax ? pEntry->NetworkLatencyIncludedMax
                           : pEntry->NetworkLatencyIncludedMin);
}

int Msta_SdcPortClockFall( MstaSdc *p, int nNet, int nClock, int fOutput, int fMax )
{
    MstaIoDelay *pEntry = Msta_SdcFindPortDelay(p,nNet,nClock,fOutput,fMax);
    return pEntry ? pEntry->ClockFall : 0;
}

/* 名字匹配规则：
     完全相等              -> 命中
     对象名以 "模式/" 开头 -> 命中（可以只写层次路径的前缀）
     * 和 ? 按 glob 展开。 */
static int Msta_SdcNameMatch( const char *pPattern, const char *pText )
{
    size_t n;
    if ( pText == NULL )
        return 0;
    if ( !strcmp( pPattern, pText ) || Msta_SdcGlobMatch(pPattern,pText) )
        return 1;
    n = strlen( pPattern );
    return ( strncmp( pPattern, pText, n ) == 0 && pText[n] == '/' );
}

/* 拼出引脚对象的 "实例/引脚" 全名：放得下就写进 pBuf，否则 malloc 一块新的。 */
static char *Msta_SdcObjectPinPath( MstaDesign *pDes, const MstaSdcObject *pObj,
                                    char *pBuf, size_t nBuf )
{
    MstaInst *pInst = MstaInstArrayAt(&pDes->vInsts,pObj->nInst);
    MstaPin  *pPin  = MstaPinArrayAt(&pInst->pCell->vPins,pObj->nPin);
    const char *pInstName = Msta_InstName(pDes,pObj->nInst);
    const char *pPinName = Msta_NameStr(pPin->Name);
    size_t nInst = strlen( pInstName ), nPin = strlen( pPinName );
    char *pPath = pBuf;
    if ( nInst + nPin + 2 > nBuf )
    {
        pPath = (char *)malloc( nInst + nPin + 2 );
        assert( pPath );
    }
    memcpy( pPath, pInstName, nInst );
    pPath[nInst] = '/';
    memcpy( pPath + nInst + 1, pPinName, nPin + 1 );
    return pPath;
}

/* 把模式匹配到一个端点对象上：时钟只比时钟名，其余比对象名和 "实例/引脚"。
   没写模式（MSTA_NO_ID）= 通配。 */
static int Msta_SdcMatchObject( MstaDesign *pDes, MstaId PatternId, char Kind,
                                const MstaSdcObject *pObj, const char *pClock )
{
    const char *pPattern;
    if ( PatternId == MSTA_NO_ID )
        return 1;
    pPattern = Msta_NameStr( PatternId );
    if ( Kind == 'C' )
        return Msta_SdcNameMatch( pPattern, pClock );
    if ( pObj == NULL )
        return 0;
    if ( Msta_SdcNameMatch( pPattern, pObj->pText ) )
        return 1;
    if ( pObj->nInst >= 0 && pObj->nPin >= 0 )
    {
        char sPath[512];
        char *pPath = Msta_SdcObjectPinPath( pDes, pObj, sPath, sizeof(sPath) );
        int fMatch = Msta_SdcNameMatch( pPattern, pPath );
        if ( pPath != sPath )
            free( pPath );
        if ( fMatch )
            return 1;
    }
    return 0;
}

/* -rise_from/-fall_from/-rise_to/-fall_to：边沿未知时按"不命中"处理并告警，
   宁可保留这条检查，也不要悄悄多切掉一条路径。 */
static int Msta_SdcEdgeMatches( char RF, int fRises )
{
    if ( RF == 0 )
        return 1;
    if ( fRises < 0 )
    {
        Msta_WarnOnce("edge-qualified path exception is not modeled where the edge is unknown; ignored there");
        return 0;
    }
    return RF == ( fRises ? 'r' : 'f' );
}

/* 时钟集合限定参考时钟边沿；引脚/端口集合限定该对象的数据边沿。 */
static int Msta_SdcEndpointEdge( const MstaSdcEndpoint *pEnd, char Kind )
{
    if ( pEnd == NULL ) return -1;
    return Kind == 'C' || pEnd->pObj == NULL ? pEnd->fRises : pEnd->pObj->fRises;
}

/* -through 匹配：每个分组按顺序在路径上找第一个命中的对象，组内取"或"。
   -rise_through/-fall_through 还要求那个点上的信号边沿对上。
   pnHits / pRF 记下每个分组命中的路径对象下标和它的边沿要求（找次优路径时用），
   都可以传 NULL。路径例外与 group_path 共用这一份匹配。 */
static int Msta_SdcMatchThroughList( MstaDesign *pDes, const MstaThruObject *pThru,
                                     int nThru, const MstaSdcObject *pObjects, int nObjects,
                                     int *pnHits, char *pRF, int *pnHitCount )
{
    int i = 0, j = 0;
    int nGroup = 0;
    while ( i < nThru )
    {
        int fMatched = 0;
        int nHit = -1;
        char RF = 0;
        if ( pThru[i].Kind != MSTA_SDC_THRU_SEP )
            return 0;                           /* 表结构坏了，按不匹配处理 */
        i++;                                    /* 跳过分组分隔符 */
        while ( j < nObjects && !fMatched )
        {
            int k;
            for ( k = i; k < nThru && pThru[k].Kind != MSTA_SDC_THRU_SEP; k++ )
                if ( Msta_SdcMatchObject(pDes,pThru[k].Text,pThru[k].Kind,
                                         &pObjects[j], NULL) &&
                     Msta_SdcEdgeMatches(pThru[k].RF, pObjects[j].fRises) )
                { fMatched = 1; nHit = j; RF = pThru[k].RF; break; }
            j++;
        }
        if ( !fMatched )
            return 0;
        if ( pnHits != NULL && nGroup < MSTA_SDC_MAX_THRU )
            pnHits[nGroup] = nHit;
        if ( pRF != NULL && nGroup < MSTA_SDC_MAX_THRU )
            pRF[nGroup] = RF;
        nGroup++;
        while ( i < nThru && pThru[i].Kind != MSTA_SDC_THRU_SEP ) i++;
    }
    if ( pnHitCount != NULL ) *pnHitCount = nGroup;
    return 1;
}

/* 例外表的 -through 匹配，转发给上面那份共用实现。 */
static int Msta_SdcMatchThrough( MstaDesign *pDes, MstaException *pEx,
                                 const MstaSdcObject *pObjects, int nObjects,
                                 int *pnHits, char *pRF, int *pnHitCount )
{
    return Msta_SdcMatchThroughList( pDes, pEx->Thru, pEx->nThru, pObjects, nObjects,
                                     pnHits, pRF, pnHitCount );
}

/* 一条例外是否命中这条路径：端点、边沿、-through 都要对上。 */
static int Msta_SdcExceptionHits( MstaSdc *p, MstaDesign *pDes, MstaException *pEx,
                                  const MstaSdcEndpoint *pFrom, const MstaSdcEndpoint *pTo,
                                  const MstaSdcObject *pObjects, int nObjects )
{
    (void)p;
    if ( !Msta_SdcMatchObject(pDes,pEx->FromText,pEx->FromKind,
                              pFrom ? pFrom->pObj : NULL, pFrom ? pFrom->pClock : NULL) )
        return 0;
    if ( !Msta_SdcMatchObject(pDes,pEx->ToText,pEx->ToKind,
                              pTo ? pTo->pObj : NULL, pTo ? pTo->pClock : NULL) )
        return 0;
    if ( !Msta_SdcEdgeMatches(pEx->FromRF, Msta_SdcEndpointEdge(pFrom,pEx->FromKind)) ||
         !Msta_SdcEdgeMatches(pEx->ToRF, Msta_SdcEndpointEdge(pTo,pEx->ToKind)) )
        return 0;
    return Msta_SdcMatchThrough( pDes, pEx, pObjects, nObjects, NULL, NULL, NULL );
}

static int Msta_SdcCompareInt( int a, int b )
{
    return ( a > b ) - ( a < b );
}

static int Msta_SdcCompareDouble( double a, double b )
{
    return ( a > b ) - ( a < b );
}

/* 例外表索引：每条路径都逐条试全部例外太慢，所以按 -from/-to 的名字文本各建一张哈希，
   只取可能命中的候选（真正是否命中仍由 Msta_SdcExceptionHits 判定）。没写这一侧、通配名、
   时钟例外无法按文本查，放进 always 表，每次都算候选。查名字时在每个 '/' 处的前缀也查一次，
   支持按层次前缀匹配。从两侧候选少的一侧出发，只留另一侧也命中的例外；候选按例外号升序，
   保证后写的例外覆盖先写的。s_pExMark 按例外号记 to 侧(1)/from 侧(2)已命中，用完即清。 */
static MstaSdcIntArray s_vExCandidates, s_vExHitsTo, s_vExHitsFrom, s_vExMerge;
static char *s_pExMark;
static int   s_nExMarkCap;

static unsigned Msta_SdcStrHash( const char *pText, size_t nLen )
{
    unsigned h = 2166136261u;
    size_t i;
    for ( i = 0; i < nLen; i++ )
    {
        h ^= (unsigned char)pText[i];
        h *= 16777619u;
    }
    return h;
}

static void Msta_SdcExIndexFree( MstaSdcExIndex *pIdx )
{
    free( pIdx->pHead );
    free( pIdx->pNext );
    free( pIdx->pAlways );
    free( pIdx->pfAlways );
    memset( pIdx, 0, sizeof(MstaSdcExIndex) );
}

static MstaId Msta_SdcExIndexKey( const MstaException *pEx, int fTo )
{
    MstaId Text = fTo ? pEx->ToText : pEx->FromText;
    char Kind = fTo ? pEx->ToKind : pEx->FromKind;
    if ( Text == MSTA_NO_ID || Kind == 'C' || strpbrk( Msta_NameStr(Text), "*?" ) != NULL )
        return MSTA_NO_ID;
    return Text;
}

static void Msta_SdcExIndexBuild( MstaSdc *p, MstaSdcExIndex *pIdx, int fTo )
{
    int i, nEx = p->vExceptions.nSize;
    Msta_SdcExIndexFree( pIdx );
    pIdx->nHeadCap = 16;
    while ( pIdx->nHeadCap < 2 * nEx )
        pIdx->nHeadCap *= 2;
    pIdx->pHead = (int *)malloc( (size_t)pIdx->nHeadCap * sizeof(int) );
    pIdx->pNext = (int *)malloc( (size_t)(nEx + 1) * sizeof(int) );
    pIdx->pAlways = (int *)malloc( (size_t)(nEx + 1) * sizeof(int) );
    pIdx->pfAlways = (char *)calloc( (size_t)(nEx + 1), 1 );
    assert( pIdx->pHead && pIdx->pNext && pIdx->pAlways && pIdx->pfAlways );
    for ( i = 0; i < pIdx->nHeadCap; i++ )
        pIdx->pHead[i] = -1;
    for ( i = 0; i < nEx; i++ )
    {
        MstaId Key = Msta_SdcExIndexKey( MstaExceptionArrayAt(&p->vExceptions,i), fTo );
        const char *pKey;
        unsigned h;
        if ( Key == MSTA_NO_ID )
        {
            pIdx->pAlways[pIdx->nAlways++] = i;
            pIdx->pfAlways[i] = 1;
            continue;
        }
        pKey = Msta_NameStr( Key );
        h = Msta_SdcStrHash( pKey, strlen(pKey) ) & (unsigned)( pIdx->nHeadCap - 1 );
        pIdx->pNext[i] = pIdx->pHead[h];
        pIdx->pHead[h] = i;
    }
}

static void Msta_SdcExIndexRelease( MstaSdc *p )
{
    Msta_SdcExIndexFree( &p->ExIndexTo );
    Msta_SdcExIndexFree( &p->ExIndexFrom );
    p->nExIndexed = 0;
    MstaSdcIntArrayFree( &s_vExCandidates );
    MstaSdcIntArrayFree( &s_vExHitsTo );
    MstaSdcIntArrayFree( &s_vExHitsFrom );
    MstaSdcIntArrayFree( &s_vExMerge );
    free( s_pExMark );
    s_pExMark = NULL;
    s_nExMarkCap = 0;
}

static void Msta_SdcExIndexUpdate( MstaSdc *p )
{
    int nEx = p->vExceptions.nSize;
    if ( p->nExIndexed == nEx && p->ExIndexTo.pHead != NULL )
        return;
    Msta_SdcExIndexBuild( p, &p->ExIndexTo, 1 );
    Msta_SdcExIndexBuild( p, &p->ExIndexFrom, 0 );
    if ( s_nExMarkCap < nEx + 1 )
    {
        s_nExMarkCap = nEx + 1;
        free( s_pExMark );
        s_pExMark = (char *)calloc( (size_t)s_nExMarkCap, 1 );
        assert( s_pExMark );
    }
    p->nExIndexed = nEx;
}

static void Msta_SdcExIndexLookup( MstaSdc *p, MstaSdcExIndex *pIdx, int fTo,
                                   const char *pText, size_t nLen, MstaSdcIntArray *vHits )
{
    char Mark = fTo ? 1 : 2;
    unsigned h = Msta_SdcStrHash( pText, nLen ) & (unsigned)( pIdx->nHeadCap - 1 );
    int i;
    for ( i = pIdx->pHead[h]; i >= 0; i = pIdx->pNext[i] )
    {
        MstaException *pEx = MstaExceptionArrayAt( &p->vExceptions, i );
        const char *pKey = Msta_NameStr( fTo ? pEx->ToText : pEx->FromText );
        if ( ( s_pExMark[i] & Mark ) || strncmp( pKey, pText, nLen ) != 0 || pKey[nLen] != 0 )
            continue;
        s_pExMark[i] |= Mark;
        *MstaSdcIntArrayAppend( vHits ) = i;
    }
}

static void Msta_SdcExIndexLookupText( MstaSdc *p, MstaSdcExIndex *pIdx, int fTo,
                                       const char *pText, MstaSdcIntArray *vHits )
{
    size_t i;
    if ( pText == NULL )
        return;
    for ( i = 0; pText[i]; i++ )
        if ( pText[i] == '/' )
            Msta_SdcExIndexLookup( p, pIdx, fTo, pText, i, vHits );
    Msta_SdcExIndexLookup( p, pIdx, fTo, pText, i, vHits );
}

static void Msta_SdcExIndexHits( MstaSdc *p, MstaDesign *pDes, int fTo,
                                 const MstaSdcObject *pObj, MstaSdcIntArray *vHits )
{
    MstaSdcExIndex *pIdx = fTo ? &p->ExIndexTo : &p->ExIndexFrom;
    vHits->nSize = 0;
    if ( pObj == NULL )
        return;
    Msta_SdcExIndexLookupText( p, pIdx, fTo, pObj->pText, vHits );
    if ( pObj->nInst >= 0 && pObj->nPin >= 0 )
    {
        char sPath[512];
        char *pPath = Msta_SdcObjectPinPath( pDes, pObj, sPath, sizeof(sPath) );
        Msta_SdcExIndexLookupText( p, pIdx, fTo, pPath, vHits );
        if ( pPath != sPath )
            free( pPath );
    }
}

static int Msta_SdcCompareIntAsc( const void *pA, const void *pB )
{
    return Msta_SdcCompareInt( *(const int *)pA, *(const int *)pB );
}

static void Msta_SdcMergeSortedTail( MstaSdcIntArray *v, int nHead )
{
    int i = 0, j = nHead, n = 0;
    if ( v->nSize - nHead <= 0 )
        return;
    qsort( v->pData + nHead, (size_t)(v->nSize - nHead), sizeof(int), Msta_SdcCompareIntAsc );
    if ( nHead == 0 )
        return;
    s_vExMerge.nSize = 0;
    while ( i < nHead || j < v->nSize )
    {
        if ( j >= v->nSize || ( i < nHead && v->pData[i] < v->pData[j] ) )
            *MstaSdcIntArrayAppend( &s_vExMerge ) = v->pData[i++];
        else
            *MstaSdcIntArrayAppend( &s_vExMerge ) = v->pData[j++];
        n++;
    }
    memcpy( v->pData, s_vExMerge.pData, (size_t)n * sizeof(int) );
}

static void Msta_SdcExIndexCollect( MstaSdcExIndex *pIdx, MstaSdcIntArray *vHits,
                                    MstaSdcExIndex *pOther, char OtherMark,
                                    MstaSdcIntArray *vOut )
{
    int i, nHead;
    for ( i = 0; i < pIdx->nAlways; i++ )
    {
        int e = pIdx->pAlways[i];
        if ( pOther->pfAlways[e] || ( s_pExMark[e] & OtherMark ) )
            *MstaSdcIntArrayAppend( vOut ) = e;
    }
    nHead = vOut->nSize;
    for ( i = 0; i < vHits->nSize; i++ )
    {
        int e = vHits->pData[i];
        if ( pOther->pfAlways[e] || ( s_pExMark[e] & OtherMark ) )
            *MstaSdcIntArrayAppend( vOut ) = e;
    }
    Msta_SdcMergeSortedTail( vOut, nHead );
}

static void Msta_SdcExIndexClearMarks( MstaSdcIntArray *vHits )
{
    int i;
    for ( i = 0; i < vHits->nSize; i++ )
        s_pExMark[vHits->pData[i]] = 0;
}

static int Msta_SdcExceptionCandidates( MstaSdc *p, MstaDesign *pDes,
                                        const MstaSdcEndpoint *pFrom,
                                        const MstaSdcEndpoint *pTo )
{
    int nTo, nFrom;
    Msta_SdcExIndexUpdate( p );
    s_vExCandidates.nSize = 0;
    Msta_SdcExIndexHits( p, pDes, 1, pTo ? pTo->pObj : NULL, &s_vExHitsTo );
    Msta_SdcExIndexHits( p, pDes, 0, pFrom ? pFrom->pObj : NULL, &s_vExHitsFrom );
    nTo = p->ExIndexTo.nAlways + s_vExHitsTo.nSize;
    nFrom = p->ExIndexFrom.nAlways + s_vExHitsFrom.nSize;
    if ( nTo <= nFrom )
        Msta_SdcExIndexCollect( &p->ExIndexTo, &s_vExHitsTo, &p->ExIndexFrom, 2,
                                &s_vExCandidates );
    else
        Msta_SdcExIndexCollect( &p->ExIndexFrom, &s_vExHitsFrom, &p->ExIndexTo, 1,
                                &s_vExCandidates );
    Msta_SdcExIndexClearMarks( &s_vExHitsTo );
    Msta_SdcExIndexClearMarks( &s_vExHitsFrom );
    return s_vExCandidates.nSize;
}

void Msta_SdcFindExceptionPath( MstaSdc *p, MstaDesign *pDes,
                                const MstaSdcEndpoint *pFrom, const MstaSdcEndpoint *pTo,
                                int fSetup, const MstaSdcObject *pObjects, int nObjects,
                                int *pnCycles, int *pfFalse )
{
    int i, nSetupCycles = 1, nHoldShift = 0;
    int nCandidates = Msta_SdcExceptionCandidates( p, pDes, pFrom, pTo );
    *pfFalse = 0;
    for ( i = 0; i < nCandidates; i++ )
    {
        MstaException *pEx = MstaExceptionArrayAt(&p->vExceptions,s_vExCandidates.pData[i]);
        if ( !Msta_SdcExceptionHits(p,pDes,pEx,pFrom,pTo,pObjects,nObjects) )
            continue;
        if ( pEx->fApplySetup ) nSetupCycles = pEx->nSetupCycles;
        if ( pEx->fApplyHold )  nHoldShift  = pEx->nHoldShift;
        if ( fSetup ? pEx->fFalseSetup : pEx->fFalseHold )
            *pfFalse = 1;
    }
    /* SDC 规则：-setup N 把 setup 捕捉沿推后 N-1 拍，hold 沿默认比 setup 沿早一拍，
       也就跟着推后；-hold M 再把 hold 沿往回拉 M 拍。所以 hold 的拍数是 N - M（1 = 默认位置）。 */
    *pnCycles = fSetup ? nSetupCycles : nSetupCycles - nHoldShift;
}

double Msta_SdcFindPathDelay( MstaSdc *p, MstaDesign *pDes,
                              const MstaSdcEndpoint *pFrom, const MstaSdcEndpoint *pTo,
                              const MstaSdcObject *pObjects, int nObjects, int fMax )
{
    int i, nCandidates = Msta_SdcExceptionCandidates( p, pDes, pFrom, pTo );
    double Best = MSTA_UNSET;
    for ( i = 0; i < nCandidates; i++ )
    {
        MstaException *pEx = MstaExceptionArrayAt(&p->vExceptions,s_vExCandidates.pData[i]);
        if ( !pEx->fMaxDelay && !pEx->fMinDelay )
            continue;
        if ( !Msta_SdcExceptionHits(p,pDes,pEx,pFrom,pTo,pObjects,nObjects) )
            continue;
        if ( fMax && pEx->fMaxDelay && (!Msta_IsSet(Best) || pEx->MaxDelay < Best) )
            Best = pEx->MaxDelay;
        if ( !fMax && pEx->fMinDelay && (!Msta_IsSet(Best) || pEx->MinDelay > Best) )
            Best = pEx->MinDelay;
    }
    return Best;
}

/* 路径被 false path 命中后，为了找次优路径：把命中的那条例外的 -through 匹配到的
   (网络, 边沿) 收集起来，调用方排除掉这些到达时间再重算。
   -through 不带边沿时两个边沿都排除（那条例外对这根网络上的任何边沿都成立）；
   只有 -from/-to 的例外没法细分，返回 0 个。 */
int Msta_SdcPathExclusions( MstaSdc *p, MstaDesign *pDes,
                            const MstaSdcEndpoint *pFrom, const MstaSdcEndpoint *pTo,
                            int fSetup, const MstaSdcObject *pObjects, int nObjects,
                            MstaPathExclude *pOut, int nCap )
{
    int i, nOut = 0, nCandidates = Msta_SdcExceptionCandidates( p, pDes, pFrom, pTo );
    for ( i = 0; i < nCandidates && nOut < nCap; i++ )
    {
        MstaException *pEx = MstaExceptionArrayAt(&p->vExceptions,s_vExCandidates.pData[i]);
        int nHits[MSTA_SDC_MAX_THRU], nHitCount = 0, g;
        char vRF[MSTA_SDC_MAX_THRU];
        if ( pEx->nThru == 0 )
            continue;
        if ( !( fSetup ? pEx->fFalseSetup : pEx->fFalseHold ) )
            continue;
        if ( !Msta_SdcExceptionHits(p,pDes,pEx,pFrom,pTo,pObjects,nObjects) )
            continue;
        if ( !Msta_SdcMatchThrough(pDes,pEx,pObjects,nObjects,nHits,vRF,&nHitCount) )
            continue;
        for ( g = 0; g < nHitCount && nOut < nCap; g++ )
        {
            const MstaSdcObject *pObj = &pObjects[nHits[g]];
            int j, fSeen = 0;
            int EdgeMask = ( vRF[g] == 'r' ) ? 1 : ( vRF[g] == 'f' ) ? 2 : 3;
            if ( pObj->nNet < 0 )
                continue;
            for ( j = 0; j < nOut; j++ )
                if ( pOut[j].Net == pObj->nNet )
                { pOut[j].EdgeMask |= EdgeMask; fSeen = 1; break; }
            if ( !fSeen )
            {
                pOut[nOut].Net = pObj->nNet;
                pOut[nOut].EdgeMask = EdgeMask;
                nOut++;
            }
        }
    }
    return nOut;
}

static const MstaException *s_pSortExceptions;

/* 比较两条例外除 -from 名字（FromText/FromKind）以外的全部字段；返回 0 表示两者只差 -from。 */
static int Msta_SdcCompareExceptionRest( const MstaException *pA, const MstaException *pB )
{
    int k, r;
    if ( (r = Msta_SdcCompareInt(pA->ToText, pB->ToText)) != 0 ) return r;
    if ( (r = Msta_SdcCompareInt(pA->ToKind, pB->ToKind)) != 0 ) return r;
    if ( (r = Msta_SdcCompareInt(pA->FromRF, pB->FromRF)) != 0 ) return r;
    if ( (r = Msta_SdcCompareInt(pA->ToRF, pB->ToRF)) != 0 ) return r;
    if ( (r = Msta_SdcCompareInt(pA->nThru, pB->nThru)) != 0 ) return r;
    for ( k = 0; k < pA->nThru; k++ )
    {
        if ( (r = Msta_SdcCompareInt(pA->Thru[k].Text, pB->Thru[k].Text)) != 0 ) return r;
        if ( (r = Msta_SdcCompareInt(pA->Thru[k].Kind, pB->Thru[k].Kind)) != 0 ) return r;
        if ( (r = Msta_SdcCompareInt(pA->Thru[k].RF, pB->Thru[k].RF)) != 0 ) return r;
    }
    if ( (r = Msta_SdcCompareInt(pA->nSetupCycles, pB->nSetupCycles)) != 0 ) return r;
    if ( (r = Msta_SdcCompareInt(pA->nHoldShift, pB->nHoldShift)) != 0 ) return r;
    if ( (r = Msta_SdcCompareInt(pA->fApplySetup, pB->fApplySetup)) != 0 ) return r;
    if ( (r = Msta_SdcCompareInt(pA->fApplyHold, pB->fApplyHold)) != 0 ) return r;
    if ( (r = Msta_SdcCompareInt(pA->fFalseSetup, pB->fFalseSetup)) != 0 ) return r;
    if ( (r = Msta_SdcCompareInt(pA->fFalseHold, pB->fFalseHold)) != 0 ) return r;
    if ( (r = Msta_SdcCompareInt(pA->fMaxDelay, pB->fMaxDelay)) != 0 ) return r;
    if ( (r = Msta_SdcCompareInt(pA->fMinDelay, pB->fMinDelay)) != 0 ) return r;
    if ( pA->fMaxDelay && (r = Msta_SdcCompareDouble(pA->MaxDelay, pB->MaxDelay)) != 0 ) return r;
    if ( pA->fMinDelay && (r = Msta_SdcCompareDouble(pA->MinDelay, pB->MinDelay)) != 0 ) return r;
    return 0;
}

static int Msta_SdcCompareExceptionIndex( const void *pA, const void *pB )
{
    int a = *(const int *)pA, b = *(const int *)pB;
    int r = Msta_SdcCompareExceptionRest( &s_pSortExceptions[a], &s_pSortExceptions[b] );
    return r ? r : Msta_SdcCompareInt( a, b );
}

int Msta_SdcExceptionCount( MstaSdc *p )
{
    return p->vExceptions.nSize;
}

/* 给 -from 是非时钟对象的例外编组号（其余记 -1），只差 -from 名字的例外同组。
   -from 例外只对部分起点生效，命中例外不同的起点不能合在一起传播；引擎按起点命中的组号
   给起点分类（msta_timing.c 的 BuildStartClasses），同组例外对起点效果相同，合组可减少分类数。 */
int Msta_SdcFromExceptionGroups( MstaSdc *p, int *pGroups )
{
    int i, n = 0, nGroups = 0;
    int *pOrder = (int *)malloc( (size_t)(p->vExceptions.nSize + 1) * sizeof(int) );
    assert( pOrder );
    for ( i = 0; i < p->vExceptions.nSize; i++ )
    {
        MstaException *pEx = MstaExceptionArrayAt(&p->vExceptions,i);
        pGroups[i] = -1;
        if ( pEx->FromText != MSTA_NO_ID && pEx->FromKind != 'C' )
            pOrder[n++] = i;
    }
    s_pSortExceptions = p->vExceptions.pData;
    qsort( pOrder, (size_t)n, sizeof(int), Msta_SdcCompareExceptionIndex );
    for ( i = 0; i < n; i++ )
    {
        if ( i == 0 || Msta_SdcCompareExceptionRest( &s_pSortExceptions[pOrder[i-1]],
                                                     &s_pSortExceptions[pOrder[i]] ) != 0 )
            nGroups++;
        pGroups[pOrder[i]] = nGroups - 1;
    }
    free( pOrder );
    return nGroups;
}

int Msta_SdcFromExceptionMatches( MstaSdc *p, MstaDesign *pDes, const MstaSdcObject *pObj,
                                  const int **ppExceptions )
{
    int i, n = 0, nHead;
    Msta_SdcExIndexUpdate( p );
    Msta_SdcExIndexHits( p, pDes, 0, pObj, &s_vExHitsFrom );
    Msta_SdcExIndexClearMarks( &s_vExHitsFrom );
    s_vExCandidates.nSize = 0;
    for ( i = 0; i < p->ExIndexFrom.nAlways; i++ )
    {
        MstaException *pEx = MstaExceptionArrayAt( &p->vExceptions, p->ExIndexFrom.pAlways[i] );
        if ( pEx->FromText != MSTA_NO_ID && pEx->FromKind != 'C' )
            *MstaSdcIntArrayAppend( &s_vExCandidates ) = p->ExIndexFrom.pAlways[i];
    }
    nHead = s_vExCandidates.nSize;
    for ( i = 0; i < s_vExHitsFrom.nSize; i++ )
        *MstaSdcIntArrayAppend( &s_vExCandidates ) = s_vExHitsFrom.pData[i];
    Msta_SdcMergeSortedTail( &s_vExCandidates, nHead );
    for ( i = 0; i < s_vExCandidates.nSize; i++ )
    {
        MstaException *pEx = MstaExceptionArrayAt( &p->vExceptions, s_vExCandidates.pData[i] );
        if ( pEx->FromText == MSTA_NO_ID || pEx->FromKind == 'C' ||
             !Msta_SdcMatchObject( pDes, pEx->FromText, pEx->FromKind, pObj, NULL ) )
            continue;
        s_vExCandidates.pData[n++] = s_vExCandidates.pData[i];
    }
    s_vExCandidates.nSize = n;
    *ppExceptions = s_vExCandidates.pData;
    return n;
}

int Msta_SdcPathGroupCount( MstaSdc *p )
{
    return p->vPathGroups.nSize;
}

MstaPathGroup *Msta_SdcPathGroupByIndex( MstaSdc *p, int i )
{
    return MstaPathGroupArrayAt( &p->vPathGroups, i );
}

int Msta_SdcPathGroupsUsed( MstaSdc *p )
{
    return p->vPathGroups.nSize > 0;
}

/* 分组记录的端点、边沿、-through 是否命中一条路径（与例外用同一套匹配）。 */
static int Msta_SdcPathGroupHits( MstaSdc *p, MstaDesign *pDes, MstaPathGroup *pGroup,
                                  const MstaSdcEndpoint *pFrom, const MstaSdcEndpoint *pTo,
                                  const MstaSdcObject *pObjects, int nObjects )
{
    (void)p;
    if ( !Msta_SdcMatchObject(pDes,pGroup->FromText,pGroup->FromKind,
                              pFrom ? pFrom->pObj : NULL, pFrom ? pFrom->pClock : NULL) )
        return 0;
    if ( !Msta_SdcMatchObject(pDes,pGroup->ToText,pGroup->ToKind,
                              pTo ? pTo->pObj : NULL, pTo ? pTo->pClock : NULL) )
        return 0;
    if ( !Msta_SdcEdgeMatches(pGroup->FromRF, Msta_SdcEndpointEdge(pFrom,pGroup->FromKind)) ||
         !Msta_SdcEdgeMatches(pGroup->ToRF, Msta_SdcEndpointEdge(pTo,pGroup->ToKind)) )
        return 0;
    return Msta_SdcMatchThroughList( pDes, pGroup->Thru, pGroup->nThru,
                                     pObjects, nObjects, NULL, NULL, NULL );
}

MstaId Msta_SdcFindPathGroup( MstaSdc *p, MstaDesign *pDes,
                              const MstaSdcEndpoint *pFrom, const MstaSdcEndpoint *pTo,
                              const MstaSdcObject *pObjects, int nObjects )
{
    int i, fDefault = 0;
    for ( i = 0; i < p->vPathGroups.nSize; i++ )
    {
        MstaPathGroup *pGroup = MstaPathGroupArrayAt( &p->vPathGroups, i );
        if ( pGroup->fDefault )
            { fDefault = 1; continue; }        /* 兜底组留到最后再看 */
        if ( Msta_SdcPathGroupHits(p,pDes,pGroup,pFrom,pTo,pObjects,nObjects) )
            return pGroup->Name;
    }
    if ( !fDefault )
        return MSTA_NO_ID;
    for ( i = 0; i < p->vPathGroups.nSize; i++ )
    {
        MstaPathGroup *pGroup = MstaPathGroupArrayAt( &p->vPathGroups, i );
        if ( pGroup->fDefault &&
             Msta_SdcPathGroupHits(p,pDes,pGroup,pFrom,pTo,pObjects,nObjects) )
            return Msta_NameId( MSTA_SDC_GROUP_DEFAULT );
    }
    return MSTA_NO_ID;
}

double Msta_SdcPathGroupWeight( MstaSdc *p, MstaId NameId )
{
    int i;
    for ( i = 0; i < p->vPathGroups.nSize; i++ )
    {
        MstaPathGroup *pGroup = MstaPathGroupArrayAt( &p->vPathGroups, i );
        if ( pGroup->fDefault ) continue;
        if ( pGroup->Name == NameId ) return pGroup->Weight;
    }
    return 1.0;
}

double Msta_SdcMaxTimeBorrow( MstaSdc *p, int nInst )
{
    int i;
    double Value = MSTA_UNSET;
    /* 先看这个实例的专用值，再看全局默认（后写的覆盖先写的）。 */
    for ( i = 0; i < p->vBorrow.nSize; i++ )
    {
        MstaBorrowSdc *pRec = MstaBorrowSdcArrayAt( &p->vBorrow, i );
        if ( pRec->Inst == nInst ) Value = pRec->Value;
    }
    if ( Msta_IsSet(Value) )
        return Value;
    for ( i = 0; i < p->vBorrow.nSize; i++ )
    {
        MstaBorrowSdc *pRec = MstaBorrowSdcArrayAt( &p->vBorrow, i );
        if ( pRec->Inst == -1 ) Value = pRec->Value;
    }
    return Value;
}

void Msta_SdcClockGatingValue( MstaSdc *p, int nInst, int fMax,
                               double *pValue, int *pfSet )
{
    int i, fDefault = 0;
    *pValue = MSTA_UNSET;
    *pfSet = 0;
    /* 先看这个实例的专用条目，再看全局默认（后写的覆盖先写的）。 */
    for ( i = 0; i < p->vClockGating.nSize; i++ )
    {
        MstaClockGatingSdc *pRec = MstaClockGatingSdcArrayAt( &p->vClockGating, i );
        double v;
        if ( pRec->Inst == -1 ) { fDefault = 1; continue; }
        if ( pRec->Inst != nInst ) continue;
        v = fMax ? pRec->Setup : pRec->Hold;
        if ( Msta_IsSet(v) ) { *pValue = v; *pfSet = 1; }
    }
    if ( !*pfSet && fDefault )
        for ( i = 0; i < p->vClockGating.nSize; i++ )
        {
            MstaClockGatingSdc *pRec = MstaClockGatingSdcArrayAt( &p->vClockGating, i );
            double v;
            if ( pRec->Inst != -1 ) continue;
            v = fMax ? pRec->Setup : pRec->Hold;
            if ( Msta_IsSet(v) ) { *pValue = v; *pfSet = 1; }
        }
}

void Msta_SdcPrintClocks( MstaSdc *p, MstaDesign *pDes, FILE *pFile )
{
    int i;
    fprintf( pFile, "%-12s %11s %10s %10s  %s\n", "clock", "period(ns)", "setup_unc",
             "hold_unc", "source" );
    for ( i = 0; i < p->vClocks.nSize; i++ )
    {
        MstaClock *pClock = MstaClockArrayAt( &p->vClocks, i );
        fprintf( pFile, "%-12s %11.3f %10.3f %10.3f  %s\n",
                 Msta_NameStr( pClock->Name ), pClock->Period / 1000.0,
                 pClock->UncertaintySetup / 1000.0, pClock->UncertaintyHold / 1000.0,
                 pClock->SourceNet >= 0 ? Msta_NetName( pDes, pClock->SourceNet ) : "(virtual)" );
        fprintf( pFile, "  edges %.3f/%.3f ns  source latency max/min %.3f/%.3f ns  network latency max/min %.3f/%.3f ns  %s\n",
                 pClock->RiseEdge / 1000.0, pClock->FallEdge / 1000.0,
                 pClock->SourceLatencyMax / 1000.0, pClock->SourceLatencyMin / 1000.0,
                 pClock->NetworkLatencyMax / 1000.0, pClock->NetworkLatencyMin / 1000.0,
                 pClock->fPropagated ? "propagated" : "ideal network" );
    }
    if ( p->vClocks.nSize == 0 )
        fprintf( pFile, "(no clock -- say create_clock)\n" );
}
