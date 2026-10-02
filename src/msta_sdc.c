/**CFile***************************************************************

  FileName    [msta_sdc.c]

  Synopsis    [SDC 子集的读入与查询。]

  Tcl 前端负责 SDC 语法、变量、集合和 source；这里接收类型化的命令参数，
  解析成约束对象，再由时序引擎应用。
  每条命令的参数按它的选项表由通用解析器拆开（"选项表与通用解析器"一节），
  再交给它的处理函数（"各条命令"一节）；分发表是 s_vSdcCommands。
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

MstaArrayDefine( int, MstaSdcIntArray )

static MstaSdcArgList s_Args;
static int  *s_pArgHash;
static int   s_nArgHashCap;
static void **s_ppArena;
static int    s_nArenaUsed, s_nArenaCap;
static char s_pBridgePath[4096] = "scripts/sdc_bridge.tcl";
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
   数值与对象的小工具
   ===================================================================== */

/* 判断一个词是不是完整的浮点数。 */
static int Msta_SdcIsNumber( const char *pText )
{
    char *pEnd;
    if ( pText == NULL || pText[0] == 0 )
        return 0;
    (void)strtod( pText, &pEnd );
    return pEnd != pText && *pEnd == 0;
}

/* SDC 时间默认取库单位（set_units 可覆盖），内部一律 ps。 */
static double Msta_SdcToPs( MstaSdc *p, const char *pText )
{
    return atof( pText ) * p->TimeScalePs;
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

/* =====================================================================
   选项表与通用解析器

   每条命令的语法写在分发表 s_vSdcCommands 的一行里（MstaSdcCmdDef）：
   选项表、位置参数的形状（开头有没有一个值、对象个数的上下限）和处理函数。
   通用解析器 Msta_SdcParseCmd 按它从左到右读 argv，拆成一个 MstaSdcCmd，
   并统一检查语法（规则见 docs/sdc.md 的"命令解析规则"）：
   - 只有"以 - 开头、后面跟字母"的词才是选项；-5 这样的负数永远是值；
   - 不认识的选项、带值选项缺值、同一个选项写了两次：作废整条约束；
   - 位置参数开头可以有一个值，其余是对象；多出来的值、对象太多或太少：
     同样作废整条约束。
   处理函数只做语义，取结果用 Msta_SdcHasFlag / Msta_SdcOptValue /
   Msta_SdcOptList 和 pValue / ppObjs。
   ===================================================================== */

/* 选项的种类：决定解析器怎样取它的值。Tcl 里选项的值是紧跟的下一个参数；
   一个参数可能是 get_* 集合，展开后是多个名字。 */
typedef enum {
    MSTA_SDC_FLAG,         /* 开关，如 -add：出现就算数 */
    MSTA_SDC_VALUE,        /* 带一个值，如 -period 5：下一个参数，只能是一个词 */
    MSTA_SDC_OBJECTS,      /* 带一组对象，如 -clock [get_clocks {a b}]：下一个参数里的
                              所有名字（Tcl 列表 {a b} 也拆开） */
    MSTA_SDC_LIST          /* 带一串对象，如 -from a b：Tcl 桥会把 -from/-to/-through/-group
                              后面的列表拆成多个参数，所以一直取到下一个选项或数值为止 */
} MstaSdcOptKind;

/* 选项的附加属性，可以用 | 组合。 */
#define MSTA_SDC_RF      1 /* 也认 -rise_xxx / -fall_xxx 写法，并记下用的是哪个边沿 */
#define MSTA_SDC_REPEAT  2 /* 可以写多次，每次是单独的一组，如 -through、-group（只用于 LIST） */
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
    const char  **ppValue;     /* MSTA_SDC_VALUE 选项的值 */
    char         *pEdge;       /* MSTA_SDC_RF 选项用的写法：'r' / 'f' / 0 */
    int          *pListOpt;    /* 对象类选项（OBJECTS / LIST）的每次出现，按顺序：选项表下标、 */
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

/* 作废整条约束：告警 "<命令>: <原因>; constraint rejected"，并计入忽略数。
   命令的所有语法和取值错误都走这里，措辞因此一致。 */
static void Msta_SdcReject( MstaSdc *p, const MstaSdcCmd *pCmd, const char *pFormat, ... )
{
    char sReason[400];
    va_list Args;
    va_start( Args, pFormat );
    vsnprintf( sReason, sizeof(sReason), pFormat, Args );
    va_end( Args );
    Msta_WarnOnce( "%s: %s; constraint rejected", pCmd->pName, sReason );
    p->nCommandsIgnored++;
}

/* 只提醒、约束照常生效："<命令>: <说明>"。 */
static void Msta_SdcNote( const MstaSdcCmd *pCmd, const char *pFormat, ... )
{
    char sText[400];
    va_list Args;
    va_start( Args, pFormat );
    vsnprintf( sText, sizeof(sText), pFormat, Args );
    va_end( Args );
    Msta_WarnOnce( "%s: %s", pCmd->pName, sText );
}

/* 在本条命令的临时内存上分配 n 个清零的元素（多给一个，n 为 0 也能用）。 */
static void *Msta_SdcArenaZeros( int n, size_t nSize )
{
    void *pMem = calloc( (size_t)n + 1, nSize );
    assert( pMem );
    return Msta_SdcArenaKeep( pMem );
}

/* 选项词：以 - 开头、后面是字母或下划线，并且不是数值（-inf 这类算数值）。
   所以 -5、-0.1、"-0.1 0 0" 都是值，不是选项。 */
static int Msta_SdcLooksLikeOption( const char *pWord )
{
    return pWord[0] == '-' && ( isalpha( (unsigned char)pWord[1] ) || pWord[1] == '_' ) &&
           !Msta_SdcIsNumber( pWord );
}

/* pWord 是不是选项 pOpt：带 MSTA_SDC_RF 的选项还认 -rise_/-fall_ 前缀的变体，
   边沿写进 *pEdge（不带前缀写 0）。 */
static int Msta_SdcWordIsOpt( const char *pWord, const MstaSdcOpt *pOpt, char *pEdge )
{
    *pEdge = 0;
    if ( !strcmp( pWord, pOpt->pName ) )
        return 1;
    if ( !( pOpt->Attr & MSTA_SDC_RF ) )
        return 0;
    /* 选项名去掉开头的 '-' 再接到前缀后面：-from -> -rise_from */
    if ( !strncmp( pWord, "-rise_", 6 ) && !strcmp( pWord + 6, pOpt->pName + 1 ) )
    { *pEdge = 'r'; return 1; }
    if ( !strncmp( pWord, "-fall_", 6 ) && !strcmp( pWord + 6, pOpt->pName + 1 ) )
    { *pEdge = 'f'; return 1; }
    return 0;
}

/* pWord 是选项表里的哪一项：返回下标，边沿写进 *pEdge；不是选项返回 -1。 */
static int Msta_SdcFindOpt( const MstaSdcOpt *pOpts, const char *pWord, char *pEdge )
{
    int k;
    for ( k = 0; pOpts[k].pName; k++ )
        if ( Msta_SdcWordIsOpt( pWord, &pOpts[k], pEdge ) )
            return k;
    return -1;
}

/* 位置参数拆成"开头的值 + 对象"，并按分发表检查个数。ppPos[i] 在 argv 的
   下标是 pPosAt[i]。有错时告警作废并返回 0。 */
static int Msta_SdcSplitPositional( MstaSdc *p, const MstaSdcCmdDef *pDef, MstaSdcCmd *pCmd,
                                    char **ppPos, int *pPosAt, int nPos )
{
    int i, iFirst = 0;
    char Edge;
    if ( pDef->Value != MSTA_SDC_NO_VALUE )
    {
        if ( nPos == 0 )
        { Msta_SdcReject( p, pCmd, "needs a value" ); return 0; }
        if ( pDef->Value == MSTA_SDC_NUMBER && !Msta_SdcIsNumber( ppPos[0] ) )
        { Msta_SdcReject( p, pCmd, "value \"%s\" is not a number", ppPos[0] ); return 0; }
        pCmd->pValue = ppPos[0];
        iFirst = 1;
    }
    /* 对象里不能再有数值：只有一个值的命令写了两个数。紧跟在开关后面的数
       是 "-max 2 -min 1" 这类写法，告警里点明。 */
    for ( i = iFirst; i < nPos; i++ )
    {
        const char *pPrev = pCmd->argv[ pPosAt[i] - 1 ];
        int k;
        if ( !Msta_SdcIsNumber( ppPos[i] ) )
            continue;
        k = Msta_SdcFindOpt( pCmd->pOpts, pPrev, &Edge );
        if ( k >= 0 && pCmd->pOpts[k].Kind == MSTA_SDC_FLAG )
            Msta_SdcReject( p, pCmd, "\"%s %s\" is not SDC 1.8 syntax (%s takes no value)",
                            pPrev, ppPos[i], pPrev );
        else
            Msta_SdcReject( p, pCmd, "unexpected value \"%s\"", ppPos[i] );
        return 0;
    }
    pCmd->ppObjs = ppPos + iFirst;
    pCmd->nObjs  = nPos - iFirst;
    if ( pCmd->nObjs < pDef->nMinObjs )
    { Msta_SdcReject( p, pCmd, "needs an object list" ); return 0; }
    if ( pDef->nMaxObjs >= 0 && pCmd->nObjs > pDef->nMaxObjs )
    {
        Msta_SdcReject( p, pCmd, "unexpected argument \"%s\"", pCmd->ppObjs[pDef->nMaxObjs] );
        return 0;
    }
    return 1;
}

/* 把 Tcl 列表文本（如 -clock {a b} 传来的 "a b"）按空白拆成多个词，挂在临时内存上。 */
static int Msta_SdcSplitWords( const char *pText, char ***pppWords )
{
    char *pCopy = Msta_SdcArena( "%s", pText ), *pTok;
    char **ppWords = (char **)Msta_SdcArenaZeros( (int)strlen(pText), sizeof(char *) );
    int n = 0;
    for ( pTok = strtok( pCopy, " \t" ); pTok != NULL; pTok = strtok( NULL, " \t" ) )
        ppWords[n++] = pTok;
    *pppWords = ppWords;
    return n;
}

/* 通用解析器：按 pDef 把 argv 从左到右拆进 *pCmd。pArg[i] 是 argv[i] 来自第几个
   Tcl 参数，用来确定选项的值有多少个词。选项词按种类取值；其余的词都是位置参数。
   语法有错时告警、作废整条约束（计入忽略数）并返回 0，处理函数就不会被调用。 */
static int Msta_SdcParseCmd( MstaSdc *p, const MstaSdcCmdDef *pDef, int argc, char **argv,
                             const int *pArg, MstaSdcCmd *pCmd )
{
    char **ppPos = (char **)Msta_SdcArenaZeros( argc, sizeof(char *) );
    int *pPosAt  = (int *)Msta_SdcArenaZeros( argc, sizeof(int) );
    int nOpts = 0, nPos = 0, i, j, k;
    char Edge;
    while ( pDef->pOpts[nOpts].pName )
        nOpts++;
    memset( pCmd, 0, sizeof(MstaSdcCmd) );
    pCmd->pOpts        = pDef->pOpts;
    pCmd->pName        = argv[0];
    pCmd->argc         = argc;
    pCmd->argv         = argv;
    pCmd->pAt          = (int *)Msta_SdcArenaZeros( nOpts, sizeof(int) );
    pCmd->ppValue      = (const char **)Msta_SdcArenaZeros( nOpts, sizeof(char *) );
    pCmd->pEdge        = (char *)Msta_SdcArenaZeros( nOpts, sizeof(char) );
    pCmd->pListOpt     = (int *)Msta_SdcArenaZeros( argc, sizeof(int) );
    pCmd->pListEdge    = (char *)Msta_SdcArenaZeros( argc, sizeof(char) );
    pCmd->pppListWords = (char ***)Msta_SdcArenaZeros( argc, sizeof(char **) );
    pCmd->pListCount   = (int *)Msta_SdcArenaZeros( argc, sizeof(int) );
    for ( i = 1; i < argc; i++ )
    {
        const MstaSdcOpt *pOpt;
        char **ppWords = NULL;
        int nWords = 0;
        if ( !Msta_SdcLooksLikeOption( argv[i] ) )
        {
            pPosAt[nPos] = i;
            ppPos[nPos++] = argv[i];
            continue;
        }
        k = Msta_SdcFindOpt( pDef->pOpts, argv[i], &Edge );
        pOpt = k >= 0 ? &pDef->pOpts[k] : NULL;
        if ( pOpt == NULL || ( pOpt->Attr & MSTA_SDC_REJECT ) )
        { Msta_SdcReject( p, pCmd, "option \"%s\" is not modeled", argv[i] ); return 0; }
        if ( pCmd->pAt[k] > 0 && !( pOpt->Attr & MSTA_SDC_REPEAT ) )
        { Msta_SdcReject( p, pCmd, "option \"%s\" is given more than once", pOpt->pName ); return 0; }
        if ( pCmd->pAt[k] == 0 )
            pCmd->pAt[k] = i;
        pCmd->pEdge[k] = Edge;
        if ( pOpt->Kind == MSTA_SDC_FLAG )
            ;
        else if ( pOpt->Kind == MSTA_SDC_LIST )
        {
            /* 对象一直取到下一个选项词或数值：对象名不会是数，数是位置参数里的值。 */
            for ( j = i + 1; j < argc; j++ )
                if ( Msta_SdcLooksLikeOption( argv[j] ) || Msta_SdcIsNumber( argv[j] ) )
                    break;
            ppWords = argv + i + 1;
            nWords  = j - i - 1;
            if ( nWords == 0 )
            { Msta_SdcReject( p, pCmd, "option \"%s\" has an empty object list", argv[i] ); return 0; }
        }
        else
        {
            /* VALUE 和 OBJECTS 取紧跟的下一个参数（它展开出的所有词）。 */
            if ( i + 1 >= argc || Msta_SdcLooksLikeOption( argv[i+1] ) )
            { Msta_SdcReject( p, pCmd, "option \"%s\" needs a value", argv[i] ); return 0; }
            for ( j = i + 1; j < argc && pArg[j] == pArg[i+1]; j++ )
                ;
            ppWords = argv + i + 1;
            nWords  = j - i - 1;
            if ( pOpt->Kind == MSTA_SDC_VALUE && nWords > 1 )
            { Msta_SdcReject( p, pCmd, "option \"%s\" takes a single value", argv[i] ); return 0; }
            if ( pOpt->Kind == MSTA_SDC_VALUE )
                pCmd->ppValue[k] = argv[i+1];
            else if ( nWords == 1 && strpbrk( argv[i+1], " \t" ) != NULL )
                nWords = Msta_SdcSplitWords( argv[i+1], &ppWords );
        }
        if ( pOpt->Kind == MSTA_SDC_LIST || pOpt->Kind == MSTA_SDC_OBJECTS )
        {
            pCmd->pListOpt[pCmd->nLists]     = k;
            pCmd->pListEdge[pCmd->nLists]    = Edge;
            pCmd->pppListWords[pCmd->nLists] = ppWords;
            pCmd->pListCount[pCmd->nLists]   = nWords;
            pCmd->nLists++;
        }
        if ( pOpt->Attr & MSTA_SDC_IGNORE )
            Msta_SdcNote( pCmd, "option \"%s\" is not modeled; ignored", argv[i] );
        /* 跳过刚取走的值或对象（拆开的 Tcl 列表在 argv 里只占一个词） */
        if ( pOpt->Kind != MSTA_SDC_FLAG )
            i = j - 1;
    }
    return Msta_SdcSplitPositional( p, pDef, pCmd, ppPos, pPosAt, nPos );
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

/* 选项是否出现过。 */
static int Msta_SdcHasFlag( const MstaSdcCmd *pCmd, const char *pName )
{
    return pCmd->pAt[ Msta_SdcOptIndex( pCmd, pName ) ] > 0;
}

/* MSTA_SDC_VALUE 选项的值，没写返回 NULL。 */
static const char *Msta_SdcOptValue( const MstaSdcCmd *pCmd, const char *pName )
{
    return pCmd->ppValue[ Msta_SdcOptIndex( pCmd, pName ) ];
}

/* 带 MSTA_SDC_RF 的选项是用哪种写法给的：-rise_xxx 返回 'r'，-fall_xxx 返回 'f'，否则 0。 */
static char Msta_SdcOptEdge( const MstaSdcCmd *pCmd, const char *pName )
{
    return pCmd->pEdge[ Msta_SdcOptIndex( pCmd, pName ) ];
}

/* 只能写一次的对象类选项（OBJECTS / LIST）：对象写进 *pppWords，返回个数；没写返回 0。 */
static int Msta_SdcOptList( const MstaSdcCmd *pCmd, const char *pName, char ***pppWords )
{
    int k = Msta_SdcOptIndex( pCmd, pName ), l;
    *pppWords = NULL;
    for ( l = 0; l < pCmd->nLists; l++ )
        if ( pCmd->pListOpt[l] == k )
        {
            *pppWords = pCmd->pppListWords[l];
            return pCmd->pListCount[l];
        }
    return 0;
}

/* 必须写的选项没写：告警作废，返回 0。 */
static int Msta_SdcRequire( MstaSdc *p, const MstaSdcCmd *pCmd, const char *pName )
{
    if ( Msta_SdcHasFlag( pCmd, pName ) )
        return 1;
    Msta_SdcReject( p, pCmd, "option \"%s\" is required", pName );
    return 0;
}

/* ---------------- 公共子语法：几条命令共用的写法 ---------------- */

/* 数值的取值范围。 */
#define MSTA_SDC_ANY       0   /* 任意数 */
#define MSTA_SDC_NONNEG    1   /* 不能为负，如负载、摆率、面积 */
#define MSTA_SDC_POSITIVE  2   /* 必须为正，如周期、电压、倍数 */

/* 把 pText 读成数值并检查范围；pWhat 是告警里对它的称呼（如 "-weight"、"value"）。
   不合格时告警作废并返回 0。 */
static int Msta_SdcGetNumber( MstaSdc *p, const MstaSdcCmd *pCmd, const char *pWhat,
                              const char *pText, int Range, double *pValue )
{
    if ( !Msta_SdcIsNumber( pText ) )
    { Msta_SdcReject( p, pCmd, "%s \"%s\" is not a number", pWhat, pText ); return 0; }
    *pValue = atof( pText );
    if ( Range == MSTA_SDC_POSITIVE && *pValue <= 0.0 )
    { Msta_SdcReject( p, pCmd, "%s must be positive (got %s)", pWhat, pText ); return 0; }
    if ( Range == MSTA_SDC_NONNEG && *pValue < 0.0 )
    { Msta_SdcReject( p, pCmd, "%s must not be negative (got %s)", pWhat, pText ); return 0; }
    return 1;
}

/* -min/-max 与 -rise/-fall 决定一个值落在哪几个 [角][边沿] 格子里。
   角 0 = min、1 = max；边沿 0 = fall、1 = rise（与 MstaClock::Slew 等数组的下标一致）。
   一对限定都没写（或都写了）等于两个都选：只写 -max 选中 max 的 rise 和 fall
   两格，什么都不写四格全选。 */
static void Msta_SdcMinMaxRiseFall( int fMin, int fMax, int fRise, int fFall, int Sel[2][2] )
{
    int m, e;
    for ( m = 0; m < 2; m++ )
        for ( e = 0; e < 2; e++ )
            Sel[m][e] = ( m ? ( fMax || !fMin ) : ( fMin || !fMax ) ) &&
                        ( e ? ( fRise || !fFall ) : ( fFall || !fRise ) );
}

/* 从命令里读 -min/-max/-rise/-fall 四个开关，算出掩码。 */
static void Msta_SdcCmdMinMaxRiseFall( const MstaSdcCmd *pCmd, int Sel[2][2] )
{
    Msta_SdcMinMaxRiseFall( Msta_SdcHasFlag( pCmd, "-min" ), Msta_SdcHasFlag( pCmd, "-max" ),
                            Msta_SdcHasFlag( pCmd, "-rise" ), Msta_SdcHasFlag( pCmd, "-fall" ), Sel );
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

/* 同一种掩码，写进按 Max/Min × Rise/Fall 分开命名的四个字段
   （如 MstaNetCons 的 InputSlewMaxRise / InputSlewMaxFall / InputSlewMinRise / InputSlewMinFall）。
   参数顺序与字段名一致：MaxRise、MaxFall、MinRise、MinFall。 */
static void Msta_SdcStoreMinMaxRiseFall4( int Sel[2][2], double Value,
                                          double *pMaxRise, double *pMaxFall,
                                          double *pMinRise, double *pMinFall )
{
    if ( Sel[1][1] ) *pMaxRise = Value;
    if ( Sel[1][0] ) *pMaxFall = Value;
    if ( Sel[0][1] ) *pMinRise = Value;
    if ( Sel[0][0] ) *pMinFall = Value;
}

/* 只写 -rise 或只写 -fall 时返回 'r' / 'f'，否则（都没写或都写了）返回 0。 */
static char Msta_SdcCmdEdge( const MstaSdcCmd *pCmd )
{
    int fRise = Msta_SdcHasFlag( pCmd, "-rise" ), fFall = Msta_SdcHasFlag( pCmd, "-fall" );
    return ( fRise && !fFall ) ? 'r' : ( fFall && !fRise ) ? 'f' : 0;
}

/* 对象列表 -> 网络。每个对象可以是端口、引脚、网络名或总线基名；找不到的对象
   当场告警并跳过。一个网络都没找到时告警作废并返回 -1。数组挂在临时内存上。 */
static int Msta_SdcObjectNets( MstaSdc *p, MstaDesign *pDes, const MstaSdcCmd *pCmd,
                               char **ppNames, int nNames, int **ppNets )
{
    MstaSdcIntArray vNets;
    int i;
    MstaSdcIntArrayInit( &vNets );
    for ( i = 0; i < nNames; i++ )
        Msta_SdcResolveNetsInto( pDes, ppNames[i], &vNets );
    *ppNets = (int *)Msta_SdcArenaKeep( vNets.pData );
    if ( vNets.nSize == 0 )
    {
        Msta_SdcReject( p, pCmd, "none of the objects was found" );
        return -1;
    }
    return vNets.nSize;
}

/* 对象列表 -> 时钟（vClocks 里的下标）。不认识的时钟当场告警并跳过；
   一个都不认识时告警作废并返回 -1。 */
static int Msta_SdcObjectClocks( MstaSdc *p, const MstaSdcCmd *pCmd,
                                 char **ppNames, int nNames, int **ppClocks )
{
    int *pClocks = (int *)Msta_SdcArenaZeros( nNames, sizeof(int) );
    int i, n = 0;
    for ( i = 0; i < nNames; i++ )
    {
        int j = Msta_SdcClockIndexOf( p, Msta_NameId( ppNames[i] ) );
        if ( j < 0 )
            Msta_SdcNote( pCmd, "unknown clock \"%s\"", ppNames[i] );
        else
            pClocks[n++] = j;
    }
    *ppClocks = pClocks;
    if ( n == 0 )
    {
        Msta_SdcReject( p, pCmd, "none of the clocks was found" );
        return -1;
    }
    return n;
}

/* 对象列表 -> 实例。fCellNames=1 时找不到的名字再当库单元名，取用到这个单元的
   所有实例。找不到的对象当场告警并跳过；一个实例都没得到时告警作废并返回 -1。 */
static int Msta_SdcObjectInsts( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, const MstaSdcCmd *pCmd,
                                char **ppNames, int nNames, int fCellNames, int **ppInsts )
{
    MstaSdcIntArray vInsts;
    int i, k;
    MstaSdcIntArrayInit( &vInsts );
    for ( i = 0; i < nNames; i++ )
    {
        int nInst = Msta_DesignFindInstByName( pDes, ppNames[i] );
        MstaCell *pCell = ( nInst < 0 && fCellNames ) ? Msta_LibFindCell( pLib, ppNames[i] ) : NULL;
        if ( nInst >= 0 )
            *MstaSdcIntArrayAppend( &vInsts ) = nInst;
        else if ( pCell != NULL )
        {
            for ( k = 0; k < pDes->vInsts.nSize; k++ )
                if ( MstaInstArrayAt(&pDes->vInsts,k)->pCell == pCell )
                    *MstaSdcIntArrayAppend( &vInsts ) = k;
        }
        else
            Msta_SdcNote( pCmd, fCellNames ? "unknown instance or cell \"%s\"" : "unknown instance \"%s\"",
                          ppNames[i] );
    }
    *ppInsts = (int *)Msta_SdcArenaKeep( vInsts.pData );
    if ( vInsts.nSize == 0 )
    {
        Msta_SdcReject( p, pCmd, "none of the objects was found" );
        return -1;
    }
    return vInsts.nSize;
}

/* 路径描述：路径类命令（set_false_path、set_multicycle_path、set_max_delay、
   set_min_delay、group_path）共用的 -from / -through / -to 三段，在选项表里写成
   三个带 MSTA_SDC_RF 的 MSTA_SDC_LIST 选项（-through 还带 MSTA_SDC_REPEAT），
   所以 -rise_from、-fall_through 这类写法也认。Tcl 桥已经把这些选项后面的列表
   拆成了多个词。
   每个 -through 是一组可以互相替代的对象（组内取"或"），多个 -through 按路径上
   的先后顺序依次经过。 */
typedef struct {
    char         **ppFrom;     /* -from 的对象名（指向 argv），nFrom 个；没写时为 0 个 */
    int            nFrom;
    char           FromRF;     /* -rise_from 记 'r'，-fall_from 记 'f'，否则 0 */
    char         **ppTo;
    int            nTo;
    char           ToRF;
    MstaThruObject Thru[MSTA_SDC_MAX_THRU];  /* 每个 -through 先放一个分组分隔条目，
                                               再放这一组的对象 */
    int            nThru;
} MstaSdcPath;

/* 从解析结果里取出路径描述。-through 的条目总数超过 MSTA_SDC_MAX_THRU 时
   告警作废并返回 0。 */
static int Msta_SdcGetPath( MstaSdc *p, const MstaSdcCmd *pCmd, MstaSdcPath *pPath )
{
    int l, k;
    memset( pPath, 0, sizeof(MstaSdcPath) );
    pPath->nFrom  = Msta_SdcOptList( pCmd, "-from", &pPath->ppFrom );
    pPath->FromRF = Msta_SdcOptEdge( pCmd, "-from" );
    pPath->nTo    = Msta_SdcOptList( pCmd, "-to", &pPath->ppTo );
    pPath->ToRF   = Msta_SdcOptEdge( pCmd, "-to" );
    for ( l = 0; l < pCmd->nLists; l++ )
    {
        char **ppWords = pCmd->pppListWords[l];
        if ( strcmp( pCmd->pOpts[ pCmd->pListOpt[l] ].pName, "-through" ) )
            continue;
        if ( pPath->nThru + 1 + pCmd->pListCount[l] > MSTA_SDC_MAX_THRU )
        {
            Msta_SdcReject( p, pCmd, "more than %d -through objects", MSTA_SDC_MAX_THRU );
            return 0;
        }
        pPath->Thru[pPath->nThru].Text = MSTA_NO_ID;      /* 新的一组从分隔条目开始 */
        pPath->Thru[pPath->nThru].Kind = MSTA_SDC_THRU_SEP;
        pPath->Thru[pPath->nThru].RF   = 0;
        pPath->nThru++;
        for ( k = 0; k < pCmd->pListCount[l]; k++ )
        {
            pPath->Thru[pPath->nThru].Text = Msta_NameId( ppWords[k] );
            pPath->Thru[pPath->nThru].Kind = Msta_SdcKindOf( ppWords[k] );
            pPath->Thru[pPath->nThru].RF   = pCmd->pListEdge[l];
            pPath->nThru++;
        }
    }
    return 1;
}

/* =====================================================================
   各条命令

   每条命令一张选项表加一个处理函数；位置参数的形状写在分发表
   s_vSdcCommands 里（"读入与分发"一节）。处理函数被调用时语法已经检查过，
   只需检查取值、查对象、写模型。
   ===================================================================== */

/* ---------------- 时钟 ---------------- */

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

/* 网络 nNet 上是否已经有时钟。 */
static int Msta_SdcNetHasClock( MstaSdc *p, int nNet )
{
    int i;
    for ( i = 0; i < p->vClocks.nSize; i++ )
        if ( Msta_SdcClockByIndex(p,i)->SourceNet == nNet )
            return 1;
    return 0;
}

/* create_clock -period 周期 [-name 名字] [-waveform {上升 下降}] [-add] [源端口]
   -period 的 "多周期波形"（{4 8}）写法不支持；只认标量。不写 -name 时，
   源端口名就是时钟名；同一个源上的第二个时钟要写 -add。 */
static const MstaSdcOpt s_vCreateClockOpts[] = {
    { "-name",     MSTA_SDC_VALUE, 0 },
    { "-period",   MSTA_SDC_VALUE, 0 },
    { "-waveform", MSTA_SDC_VALUE, 0 },
    { "-add",      MSTA_SDC_FLAG,  0 },
    { NULL,        MSTA_SDC_FLAG,  0 } };

static void Msta_SdcCreateClock( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    const char *pName = Msta_SdcOptValue( pCmd, "-name" );
    const char *pWave = Msta_SdcOptValue( pCmd, "-waveform" );
    double Period, Rise = 0.0, Fall;
    int nSource = MSTA_NO_ID, *pNets;
    MstaClock *pClock;

    if ( !Msta_SdcRequire( p, pCmd, "-period" ) ||
         !Msta_SdcGetNumber( p, pCmd, "-period", Msta_SdcOptValue( pCmd, "-period" ),
                             MSTA_SDC_POSITIVE, &Period ) )
        return;
    Fall = 0.5 * Period;
    if ( pWave )
    {
        char Extra;
        if ( sscanf( pWave, "%lf %lf %c", &Rise, &Fall, &Extra ) != 2 )
        { Msta_SdcReject( p, pCmd, "-waveform needs exactly two edge times" ); return; }
        if ( Rise < 0.0 || Fall <= Rise || Fall >= Period )
        { Msta_SdcReject( p, pCmd, "-waveform must satisfy 0 <= rise < fall < period" ); return; }
    }
    if ( pCmd->nObjs > 1 )
    { Msta_SdcReject( p, pCmd, "multiple source objects are not modeled" ); return; }
    if ( pName == NULL && pCmd->nObjs == 0 )
    { Msta_SdcReject( p, pCmd, "needs -name or a source object" ); return; }
    if ( pName == NULL )
        pName = pCmd->ppObjs[0];
    if ( Msta_SdcFindClock( p, pName ) )
    { Msta_SdcReject( p, pCmd, "clock \"%s\" already exists", pName ); return; }
    if ( pCmd->nObjs == 1 )
    {
        if ( Msta_SdcResolveNets( pDes, pCmd->ppObjs[0], &pNets ) != 1 )
        { Msta_SdcReject( p, pCmd, "the source must resolve to one net" ); return; }
        nSource = pNets[0];
        /* 不写 -add 是"换掉这个源上的时钟"，msta 不做替换。 */
        if ( !Msta_SdcHasFlag( pCmd, "-add" ) && Msta_SdcNetHasClock( p, nSource ) )
        { Msta_SdcReject( p, pCmd, "the source already has a clock (use -add for another one)" ); return; }
    }
    pClock = Msta_SdcNewClock( p, pName );
    pClock->Period   = Period * p->TimeScalePs;
    pClock->RiseEdge = Rise * p->TimeScalePs;
    pClock->FallEdge = Fall * p->TimeScalePs;
    if ( pCmd->nObjs == 1 )
    {
        pClock->SourceNet  = nSource;
        pClock->SourceText = Msta_NameId( pCmd->ppObjs[0] );
    }
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

/* 分频/倍频系数：必须是 1..1000000 的整数。 */
static int Msta_SdcGetRatio( MstaSdc *p, const MstaSdcCmd *pCmd, const char *pName, double *pRatio )
{
    const char *pText = Msta_SdcOptValue( pCmd, pName );
    *pRatio = 1.0;
    if ( pText == NULL )
        return 1;
    if ( !Msta_SdcGetNumber( p, pCmd, pName, pText, MSTA_SDC_POSITIVE, pRatio ) )
        return 0;
    if ( *pRatio > 1000000.0 || *pRatio != (double)(int)*pRatio )
    { Msta_SdcReject( p, pCmd, "%s must be a positive integer (got %s)", pName, pText ); return 0; }
    return 1;
}

/* create_generated_clock -source 主时钟源 [-name 名字] [-master_clock 主时钟]
       [-divide_by n | -multiply_by n] [-duty_cycle d] [-invert]
       [-edges {e1 e2 e3} [-edge_shift {s1 s2 s3}]] 目标引脚
   创建支持分频、倍频和反相的生成时钟。-add / -combinational 不建模。 */
static const MstaSdcOpt s_vGeneratedClockOpts[] = {
    { "-name",          MSTA_SDC_VALUE, 0               },
    { "-source",        MSTA_SDC_VALUE, 0               },
    { "-master_clock",  MSTA_SDC_VALUE, 0               },
    { "-divide_by",     MSTA_SDC_VALUE, 0               },
    { "-multiply_by",   MSTA_SDC_VALUE, 0               },
    { "-duty_cycle",    MSTA_SDC_VALUE, 0               },
    { "-edges",         MSTA_SDC_VALUE, 0               },
    { "-edge_shift",    MSTA_SDC_VALUE, 0               },
    { "-invert",        MSTA_SDC_FLAG,  0               },
    { "-add",           MSTA_SDC_FLAG,  MSTA_SDC_REJECT },
    { "-combinational", MSTA_SDC_FLAG,  MSTA_SDC_REJECT },
    { NULL,             MSTA_SDC_FLAG,  0               } };

static void Msta_SdcCreateGeneratedClock( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    const char *pName       = Msta_SdcOptValue( pCmd, "-name" );
    const char *pSource     = Msta_SdcOptValue( pCmd, "-source" );
    const char *pMasterName = Msta_SdcOptValue( pCmd, "-master_clock" );
    const char *pDuty       = Msta_SdcOptValue( pCmd, "-duty_cycle" );
    const char *pEdges      = Msta_SdcOptValue( pCmd, "-edges" );
    const char *pShifts     = Msta_SdcOptValue( pCmd, "-edge_shift" );
    const char *pTarget     = pCmd->ppObjs[0];
    MstaClock *pMaster = NULL, *pClock;
    int *pNets, i;
    double Div, Mult, Duty = 50.0;
    double vEdges[3], vShifts[3] = { 0.0, 0.0, 0.0 };

    if ( !Msta_SdcRequire( p, pCmd, "-source" ) )
        return;
    if ( pEdges != NULL && ( Msta_SdcHasFlag( pCmd, "-divide_by" ) ||
                             Msta_SdcHasFlag( pCmd, "-multiply_by" ) || pDuty != NULL ) )
    { Msta_SdcReject( p, pCmd, "-edges cannot be combined with -divide_by/-multiply_by/-duty_cycle" ); return; }
    if ( Msta_SdcHasFlag( pCmd, "-divide_by" ) && Msta_SdcHasFlag( pCmd, "-multiply_by" ) )
    { Msta_SdcReject( p, pCmd, "-divide_by and -multiply_by cannot be combined" ); return; }
    if ( !Msta_SdcGetRatio( p, pCmd, "-divide_by", &Div ) ||
         !Msta_SdcGetRatio( p, pCmd, "-multiply_by", &Mult ) )
        return;
    if ( pDuty != NULL && !Msta_SdcGetNumber( p, pCmd, "-duty_cycle", pDuty, MSTA_SDC_ANY, &Duty ) )
        return;
    if ( Duty <= 0.0 || Duty >= 100.0 )
    { Msta_SdcReject( p, pCmd, "-duty_cycle must be between 0 and 100 (got %s)", pDuty ); return; }
    if ( pShifts != NULL && pEdges == NULL )
    { Msta_SdcReject( p, pCmd, "-edge_shift needs -edges" ); return; }
    /* -edges {e1 e2 e3}：用主时钟的第 e1/e2/e3 个边沿定义新时钟的
       上升沿、下降沿和下一个上升沿。边号从 1 开始数：1 = 首个上升沿，
       2 = 首个下降沿，3 = 第二个上升沿……。 */
    if ( pEdges != NULL )
    {
        if ( Msta_SdcParseNumberList( pEdges, vEdges, 3 ) != 3 )
        { Msta_SdcReject( p, pCmd, "-edges needs exactly three edge numbers" ); return; }
        if ( pShifts != NULL && Msta_SdcParseNumberList( pShifts, vShifts, 3 ) != 3 )
        { Msta_SdcReject( p, pCmd, "-edge_shift needs as many values as -edges" ); return; }
        for ( i = 0; i < 3; i++ )
        {
            if ( vEdges[i] < 1.0 || vEdges[i] != (double)(int)vEdges[i] )
            { Msta_SdcReject( p, pCmd, "-edges values must be positive integers" ); return; }
            if ( i > 0 && vEdges[i] <= vEdges[i-1] )
            { Msta_SdcReject( p, pCmd, "-edges values must increase" ); return; }
        }
    }
    /* 主时钟：-master_clock 点名，或者 -source 所在网络上的那个时钟。 */
    if ( pMasterName != NULL )
    {
        pMaster = Msta_SdcFindClock( p, pMasterName );
        if ( pMaster == NULL )
        { Msta_SdcReject( p, pCmd, "unknown master clock \"%s\"", pMasterName ); return; }
    }
    else if ( Msta_SdcResolveNets( pDes, pSource, &pNets ) > 0 )
        for ( i = 0; i < p->vClocks.nSize && pMaster == NULL; i++ )
            if ( Msta_SdcClockByIndex(p,i)->SourceNet == pNets[0] )
                pMaster = Msta_SdcClockByIndex(p,i);
    if ( pMaster == NULL )
    { Msta_SdcReject( p, pCmd, "no master clock for \"%s\"", pSource ); return; }
    if ( Msta_SdcResolveNets( pDes, pTarget, &pNets ) != 1 )
    { Msta_SdcReject( p, pCmd, "the target must resolve to one net" ); return; }
    if ( Msta_SdcNetHasClock( p, pNets[0] ) )
    { Msta_SdcReject( p, pCmd, "the target already has a clock" ); return; }
    if ( pName == NULL )
        pName = pTarget;
    if ( Msta_SdcFindClock( p, pName ) )
    { Msta_SdcReject( p, pCmd, "clock \"%s\" already exists", pName ); return; }
    /* Append 可能重新分配 vClocks，先把主时钟的各项值取出来。 */
    {
        double Period = pMaster->Period * Div / Mult;
        double Rise = pMaster->RiseEdge;
        double Fall = Rise + Period * Duty / 100.0;
        MstaId MasterName = pMaster->Name;
        if ( pEdges != NULL )
        {
            Rise = Msta_SdcClockEdgeTime( pMaster, (int)vEdges[0] ) + vShifts[0] * p->TimeScalePs;
            Fall = Msta_SdcClockEdgeTime( pMaster, (int)vEdges[1] ) + vShifts[1] * p->TimeScalePs;
            Period = Msta_SdcClockEdgeTime( pMaster, (int)vEdges[2] ) + vShifts[2] * p->TimeScalePs - Rise;
            if ( Period <= 0.0 || Fall <= Rise )
            { Msta_SdcReject( p, pCmd, "-edges gives a non-positive period" ); return; }
        }
        pClock = Msta_SdcNewClock( p, pName );
        pClock->MasterClock = MasterName;
        pClock->Period   = Period;
        pClock->RiseEdge = Msta_SdcHasFlag( pCmd, "-invert" ) ? Fall : Rise;
        pClock->FallEdge = Msta_SdcHasFlag( pCmd, "-invert" ) ? Rise : Fall;
        pClock->SourceNet  = pNets[0];
        pClock->SourceText = Msta_NameId( pTarget );
    }
}

/* set_clock_uncertainty [-setup] [-hold] [-rise|-fall] 值 时钟列表
   set_clock_uncertainty [-setup] [-hold] -from|-rise_from|-fall_from 时钟列表
                         -to|-rise_to|-fall_to 时钟列表 值
   给时钟、或者两组时钟之间设置 setup/hold 不确定度。-setup/-hold 都没写时两者
   都设；裸 -rise/-fall 限定的是 -to 一侧（捕获沿）的边沿。 */
static const MstaSdcOpt s_vClockUncertaintyOpts[] = {
    { "-setup", MSTA_SDC_FLAG, 0           },
    { "-hold",  MSTA_SDC_FLAG, 0           },
    { "-rise",  MSTA_SDC_FLAG, 0           },
    { "-fall",  MSTA_SDC_FLAG, 0           },
    { "-from",  MSTA_SDC_LIST, MSTA_SDC_RF },
    { "-to",    MSTA_SDC_LIST, MSTA_SDC_RF },
    { NULL,     MSTA_SDC_FLAG, 0           } };

static void Msta_SdcSetClockUncertainty( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    int fSetup = Msta_SdcHasFlag( pCmd, "-setup" ) || !Msta_SdcHasFlag( pCmd, "-hold" );
    int fHold  = Msta_SdcHasFlag( pCmd, "-hold" ) || !Msta_SdcHasFlag( pCmd, "-setup" );
    double Value = Msta_SdcToPs( p, pCmd->pValue );
    char **ppFrom, **ppTo, ToRF = Msta_SdcOptEdge( pCmd, "-to" );
    int nFrom = Msta_SdcOptList( pCmd, "-from", &ppFrom );
    int nTo   = Msta_SdcOptList( pCmd, "-to", &ppTo );
    int *pFrom = NULL, *pTo = NULL, *pClocks, nClocks, i, j, c;

    if ( ToRF == 0 )
        ToRF = Msta_SdcCmdEdge( pCmd );
    if ( nFrom == 0 && nTo == 0 )
    {
        if ( pCmd->nObjs == 0 )
        { Msta_SdcReject( p, pCmd, "needs a clock list or -from/-to" ); return; }
        nClocks = Msta_SdcObjectClocks( p, pCmd, pCmd->ppObjs, pCmd->nObjs, &pClocks );
        for ( i = 0; i < nClocks; i++ )
        {
            MstaClock *pClock = Msta_SdcClockByIndex( p, pClocks[i] );
            if ( fSetup ) pClock->UncertaintySetup = Value;
            if ( fHold )  pClock->UncertaintyHold  = Value;
        }
        return;
    }
    /* -from/-to 形式：两组时钟两两之间各记一条；没写的一侧表示任意时钟。 */
    if ( pCmd->nObjs > 0 )
    { Msta_SdcReject( p, pCmd, "a clock list cannot be combined with -from/-to" ); return; }
    if ( nFrom > 0 && ( nFrom = Msta_SdcObjectClocks( p, pCmd, ppFrom, nFrom, &pFrom ) ) < 0 )
        return;
    if ( nTo > 0 && ( nTo = Msta_SdcObjectClocks( p, pCmd, ppTo, nTo, &pTo ) ) < 0 )
        return;
    for ( i = 0; i < ( nFrom > 0 ? nFrom : 1 ); i++ )
        for ( j = 0; j < ( nTo > 0 ? nTo : 1 ); j++ )
            for ( c = 0; c < 2; c++ )
            {
                MstaInterClockUnc *pUnc;
                if ( c == 0 ? !fSetup : !fHold )
                    continue;
                pUnc = MstaInterClockUncArrayAppend( &p->vInterClockUnc );
                pUnc->FromClock = nFrom > 0 ? Msta_SdcClockByIndex( p, pFrom[i] )->Name : MSTA_NO_ID;
                pUnc->ToClock   = nTo   > 0 ? Msta_SdcClockByIndex( p, pTo[j] )->Name   : MSTA_NO_ID;
                pUnc->FromRF = Msta_SdcOptEdge( pCmd, "-from" );
                pUnc->ToRF   = ToRF;
                pUnc->fSetup = c == 0;
                pUnc->fHold  = c == 1;
                pUnc->Value  = Value;
            }
}

/* set_clock_latency [-source] [-min|-max|-early|-late] [-rise|-fall] [-clock 时钟列表] 延迟 [时钟列表]
   设置理想时钟分析的源延迟 / 网络延迟。-early/-late 与 -min/-max 同义；
   要设的时钟可以写在 -clock 后面，也可以写成对象列表。 */
static const MstaSdcOpt s_vClockLatencyOpts[] = {
    { "-clock",  MSTA_SDC_OBJECTS, 0 },
    { "-source", MSTA_SDC_FLAG,    0 },
    { "-min",    MSTA_SDC_FLAG, 0 },
    { "-max",    MSTA_SDC_FLAG, 0 },
    { "-early",  MSTA_SDC_FLAG, 0 },
    { "-late",   MSTA_SDC_FLAG, 0 },
    { "-rise",   MSTA_SDC_FLAG, 0 },
    { "-fall",   MSTA_SDC_FLAG, 0 },
    { NULL,      MSTA_SDC_FLAG, 0 } };

static void Msta_SdcSetClockLatency( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    int fMax = Msta_SdcHasFlag( pCmd, "-max" ) || Msta_SdcHasFlag( pCmd, "-late" );
    int fMin = Msta_SdcHasFlag( pCmd, "-min" ) || Msta_SdcHasFlag( pCmd, "-early" );
    double Delay = Msta_SdcToPs( p, pCmd->pValue );
    char **ppOpt, **ppNames;
    int nOpt = Msta_SdcOptList( pCmd, "-clock", &ppOpt );
    int Sel[2][2], *pClocks, nClocks, i;

    /* 要设的时钟：-clock 的列表加上对象列表。 */
    ppNames = (char **)Msta_SdcArenaZeros( nOpt + pCmd->nObjs, sizeof(char *) );
    for ( i = 0; i < nOpt; i++ )
        ppNames[i] = ppOpt[i];
    for ( i = 0; i < pCmd->nObjs; i++ )
        ppNames[nOpt + i] = pCmd->ppObjs[i];
    if ( nOpt + pCmd->nObjs == 0 )
    { Msta_SdcReject( p, pCmd, "needs a clock list" ); return; }
    nClocks = Msta_SdcObjectClocks( p, pCmd, ppNames, nOpt + pCmd->nObjs, &pClocks );
    Msta_SdcMinMaxRiseFall( fMin, fMax, Msta_SdcHasFlag( pCmd, "-rise" ),
                            Msta_SdcHasFlag( pCmd, "-fall" ), Sel );
    for ( i = 0; i < nClocks; i++ )
    {
        MstaClock *pClock = Msta_SdcClockByIndex( p, pClocks[i] );
        if ( Msta_SdcHasFlag( pCmd, "-source" ) )
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
               估计值代替沿时钟树算出的真实延迟，所以这个时钟按理想时钟处理。
               它也撤销此前的 set_propagated_clock（SDC 里后写的为准）。 */
            pClock->fPropagated = pClock->fPropagatedSet = 0;
        }
    }
}

/* 没有选项的命令共用的空选项表。 */
static const MstaSdcOpt s_vNoOpts[] = {
    { NULL, MSTA_SDC_FLAG, 0 } };

/* set_propagated_clock 时钟列表
   把选中的时钟改成按时钟树传播。 */
static void Msta_SdcSetPropagatedClock( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    int *pClocks, nClocks, i;
    nClocks = Msta_SdcObjectClocks( p, pCmd, pCmd->ppObjs, pCmd->nObjs, &pClocks );
    for ( i = 0; i < nClocks; i++ )
    {
        MstaClock *pClock = Msta_SdcClockByIndex( p, pClocks[i] );
        pClock->fPropagated = pClock->fPropagatedSet = 1;
    }
}

/* 只有 -min/-max/-rise/-fall 四个开关的命令共用的选项表
   （set_clock_transition、set_ideal_latency、set_ideal_transition）。 */
static const MstaSdcOpt s_vMinMaxRiseFallOpts[] = {
    { "-min",  MSTA_SDC_FLAG, 0 },
    { "-max",  MSTA_SDC_FLAG, 0 },
    { "-rise", MSTA_SDC_FLAG, 0 },
    { "-fall", MSTA_SDC_FLAG, 0 },
    { NULL,    MSTA_SDC_FLAG, 0 } };

/* set_clock_transition [-rise|-fall] [-min|-max] 摆率 时钟列表
   给时钟源指定摆率；不写这里就用时钟网络上的输入摆率或默认值。
   虚拟时钟没有源，跳过。 */
static void Msta_SdcSetClockTransition( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    int Sel[2][2], *pClocks, nClocks, i;
    double Slew;
    if ( !Msta_SdcGetNumber( p, pCmd, "value", pCmd->pValue, MSTA_SDC_NONNEG, &Slew ) )
        return;
    Msta_SdcCmdMinMaxRiseFall( pCmd, Sel );
    nClocks = Msta_SdcObjectClocks( p, pCmd, pCmd->ppObjs, pCmd->nObjs, &pClocks );
    for ( i = 0; i < nClocks; i++ )
    {
        MstaClock *pClock = Msta_SdcClockByIndex( p, pClocks[i] );
        if ( pClock->SourceNet < 0 )
            Msta_SdcNote( pCmd, "virtual clock \"%s\" has no source; ignored", Msta_NameStr(pClock->Name) );
        else
            Msta_SdcStoreMinMaxRiseFall( pClock->Slew, Sel, Slew * p->TimeScalePs );
    }
}

/* set_clock_sense [-positive|-negative] [-stop_propagation] [-clock 时钟列表] 引脚列表
   记下某个脚/网络上的时钟极性（-negative）和"这个时钟到这里不再往下传"。
   不写 -clock 时对所有时钟生效。-pulse 不建模。 */
static const MstaSdcOpt s_vClockSenseOpts[] = {
    { "-positive",         MSTA_SDC_FLAG,  0               },
    { "-negative",         MSTA_SDC_FLAG,  0               },
    { "-stop_propagation", MSTA_SDC_FLAG,  0               },
    { "-clock",            MSTA_SDC_OBJECTS, 0             },
    { "-pulse",            MSTA_SDC_VALUE, MSTA_SDC_REJECT },
    { NULL,                MSTA_SDC_FLAG,  0               } };

static void Msta_SdcSetClockSense( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    int fPos  = Msta_SdcHasFlag( pCmd, "-positive" );
    int fNeg  = Msta_SdcHasFlag( pCmd, "-negative" );
    int fStop = Msta_SdcHasFlag( pCmd, "-stop_propagation" );
    char **ppClocks;
    int nClockNames = Msta_SdcOptList( pCmd, "-clock", &ppClocks );
    int *pClocks = NULL, nClocks = 0, *pNets, nNets, i, j;

    if ( fPos && fNeg )
    { Msta_SdcReject( p, pCmd, "-positive and -negative cannot be combined" ); return; }
    if ( !fPos && !fNeg && !fStop )
    { Msta_SdcReject( p, pCmd, "needs -positive, -negative or -stop_propagation" ); return; }
    for ( i = 0; i < pCmd->nObjs; i++ )
        if ( Msta_SdcKindOf( pCmd->ppObjs[i] ) == 'C' )
        { Msta_SdcReject( p, pCmd, "clock objects are not modeled (use -clock)" ); return; }
    if ( nClockNames > 0 &&
         ( nClocks = Msta_SdcObjectClocks( p, pCmd, ppClocks, nClockNames, &pClocks ) ) < 0 )
        return;
    if ( ( nNets = Msta_SdcObjectNets( p, pDes, pCmd, pCmd->ppObjs, pCmd->nObjs, &pNets ) ) < 0 )
        return;
    for ( i = 0; i < nNets; i++ )
        for ( j = 0; j < ( nClocks > 0 ? nClocks : 1 ); j++ )
        {
            MstaClockSense *pSense = MstaClockSenseArrayAppend( &p->vClockSense );
            pSense->Net = pNets[i];
            pSense->Clock = nClocks > 0 ? Msta_SdcClockByIndex( p, pClocks[j] )->Name : MSTA_NO_ID;
            pSense->Polarity = fNeg ? -1 : ( fPos ? 1 : 0 );
            pSense->fStop = fStop;
        }
}

/* set_clock_groups -asynchronous|-logically_exclusive|-physically_exclusive
                    [-name 名字] -group 时钟列表 -group 时钟列表 ...
   不同时钟组之间没有 setup/hold 关系，两两展开成例外。-allow_paths 不建模。 */
static const MstaSdcOpt s_vClockGroupsOpts[] = {
    { "-asynchronous",         MSTA_SDC_FLAG,  0               },
    { "-logically_exclusive",  MSTA_SDC_FLAG,  0               },
    { "-physically_exclusive", MSTA_SDC_FLAG,  0               },
    { "-name",                 MSTA_SDC_VALUE, 0               },
    { "-allow_paths",          MSTA_SDC_FLAG,  MSTA_SDC_REJECT },
    { "-group",                MSTA_SDC_LIST,  MSTA_SDC_REPEAT },
    { NULL,                    MSTA_SDC_FLAG,  0               } };

static void Msta_SdcSetClockGroups( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    int *pGroups = (int *)Msta_SdcArenaZeros( p->vClocks.nSize, sizeof(int) );
    int *pClocks, nClocks, nGroups = 0, l, i, j;

    if ( !Msta_SdcHasFlag( pCmd, "-asynchronous" ) && !Msta_SdcHasFlag( pCmd, "-logically_exclusive" ) &&
         !Msta_SdcHasFlag( pCmd, "-physically_exclusive" ) )
    { Msta_SdcReject( p, pCmd, "needs -asynchronous, -logically_exclusive or -physically_exclusive" ); return; }
    /* 每个 -group 是一组；pGroups[时钟] 记它属于第几组（从 1 数）。 */
    for ( l = 0; l < pCmd->nLists; l++ )
    {
        nClocks = Msta_SdcObjectClocks( p, pCmd, pCmd->pppListWords[l], pCmd->pListCount[l], &pClocks );
        if ( nClocks < 0 )
            return;
        nGroups++;
        for ( i = 0; i < nClocks; i++ )
            pGroups[pClocks[i]] = nGroups;
    }
    if ( nGroups < 2 )
    { Msta_SdcReject( p, pCmd, "needs at least two -group lists" ); return; }
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

/* ---------------- I/O 与网络属性 ---------------- */

/* 存一条 I/O 延迟（ps）；-add_delay 时保留同一网络上的其他时钟。
   fMax/fMin 表示这条命令约束哪个角。 */
static void Msta_SdcStorePortDelay( MstaSdc *p, int nNet, MstaId Clock, int fOutput,
                                    int fMax, int fMin, double Value, int fAdd,
                                    int fClockFall, int DataRise, int RefNet,
                                    int fSourceLatencyIncluded, int fNetworkLatencyIncluded )
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
            if ( fMax ) pOld->Max = MSTA_UNSET;
            if ( fMin ) pOld->Min = MSTA_UNSET;
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
    if ( fMax )
    {
        if ( !fAdd || !Msta_IsSet(pEntry->Max) || Value > pEntry->Max )
            pEntry->Max = Value;
        pEntry->RefNetMax = RefNet;
        pEntry->SourceLatencyIncludedMax = fSourceLatencyIncluded;
        pEntry->NetworkLatencyIncludedMax = fNetworkLatencyIncluded;
    }
    if ( fMin )
    {
        if ( !fAdd || !Msta_IsSet(pEntry->Min) || Value < pEntry->Min )
            pEntry->Min = Value;
        pEntry->RefNetMin = RefNet;
        pEntry->SourceLatencyIncludedMin = fSourceLatencyIncluded;
        pEntry->NetworkLatencyIncludedMin = fNetworkLatencyIncluded;
    }
}

/* set_input_delay / set_output_delay [-clock 时钟] [-clock_fall] [-rise|-fall]
       [-max] [-min] [-add_delay] [-reference_pin 引脚]
       [-source_latency_included] [-network_latency_included] 延迟 端口列表
   不写 -max/-min 时一个值同时约束两个角；写了就只改对应角。
   -level_sensitive 不建模。 */
static const MstaSdcOpt s_vPortDelayOpts[] = {
    { "-clock",                    MSTA_SDC_VALUE, 0               },
    { "-reference_pin",            MSTA_SDC_VALUE, 0               },
    { "-clock_fall",               MSTA_SDC_FLAG,  0               },
    { "-rise",                     MSTA_SDC_FLAG,  0               },
    { "-fall",                     MSTA_SDC_FLAG,  0               },
    { "-max",                      MSTA_SDC_FLAG,  0               },
    { "-min",                      MSTA_SDC_FLAG,  0               },
    { "-add_delay",                MSTA_SDC_FLAG,  0               },
    { "-source_latency_included",  MSTA_SDC_FLAG,  0               },
    { "-network_latency_included", MSTA_SDC_FLAG,  0               },
    { "-level_sensitive",          MSTA_SDC_FLAG,  MSTA_SDC_REJECT },
    { NULL,                        MSTA_SDC_FLAG,  0               } };

/* fOutput=0 是 set_input_delay，=1 是 set_output_delay。 */
static void Msta_SdcSetPortDelay( MstaSdc *p, MstaDesign *pDes, int fOutput, MstaSdcCmd *pCmd )
{
    const char *pClockText    = Msta_SdcOptValue( pCmd, "-clock" );
    const char *pReferencePin = Msta_SdcOptValue( pCmd, "-reference_pin" );
    int fSourceIncluded  = Msta_SdcHasFlag( pCmd, "-source_latency_included" );
    int fNetworkIncluded = Msta_SdcHasFlag( pCmd, "-network_latency_included" );
    int fMax = Msta_SdcHasFlag( pCmd, "-max" ) || !Msta_SdcHasFlag( pCmd, "-min" );
    int fMin = Msta_SdcHasFlag( pCmd, "-min" ) || !Msta_SdcHasFlag( pCmd, "-max" );
    char Edge = Msta_SdcCmdEdge( pCmd );
    int DataRise = Edge == 'r' ? 1 : Edge == 'f' ? 0 : -1;
    int RefNet = -1, *pNets, nNets, i;

    if ( pClockText && Msta_SdcFindClock( p, pClockText ) == NULL )
    { Msta_SdcReject( p, pCmd, "unknown clock \"%s\"", pClockText ); return; }
    if ( pReferencePin != NULL )
    {
        char RefKind = Msta_SdcKindOf( pReferencePin );
        if ( ( RefKind != 0 && RefKind != 'G' && RefKind != 'P' ) ||
             Msta_SdcResolveNets( pDes, pReferencePin, &pNets ) != 1 )
        { Msta_SdcReject( p, pCmd, "-reference_pin must resolve to exactly one pin or port" ); return; }
        RefNet = pNets[0];
        if ( fSourceIncluded || fNetworkIncluded )
            Msta_SdcNote( pCmd, "latency-included flags are ignored with -reference_pin" );
        fSourceIncluded = fNetworkIncluded = 0;
    }
    /* 不写 -clock 时先记成"未指定"：SDC 允许 create_clock 写在 I/O 约束之后，
       所以真正用哪个时钟留到查询时再按当时只有一个时钟来判定。 */
    if ( pClockText == NULL && pReferencePin == NULL && Msta_SdcClockCount(p) > 1 )
        Msta_SdcNote( pCmd, "no -clock while there are several clocks; the delay applies "
                      "only while the design has a single clock" );
    if ( ( nNets = Msta_SdcObjectNets( p, pDes, pCmd, pCmd->ppObjs, pCmd->nObjs, &pNets ) ) < 0 )
        return;
    for ( i = 0; i < nNets; i++ )
    {
        Msta_SdcNetConsOrCreate( p, pNets[i] );
        Msta_SdcStorePortDelay( p, pNets[i], pClockText ? Msta_NameId(pClockText) : MSTA_NO_ID,
                                fOutput, fMax, fMin, Msta_SdcToPs( p, pCmd->pValue ),
                                Msta_SdcHasFlag( pCmd, "-add_delay" ),
                                Msta_SdcHasFlag( pCmd, "-clock_fall" ), DataRise, RefNet,
                                fSourceIncluded, fNetworkIncluded );
    }
}

static void Msta_SdcSetInputDelay( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    Msta_SdcSetPortDelay( p, pDes, 0, pCmd );
}

static void Msta_SdcSetOutputDelay( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    Msta_SdcSetPortDelay( p, pDes, 1, pCmd );
}

/* set_load [-min|-max] [-subtract_pin_load] [-pin_load|-wire_load] 值 对象列表
   值默认按库的电容单位，set_units 可覆盖。
   -pin_load / -wire_load 是"这到底是脚负载还是线负载"的标注；msta 把它们
   都当外加负载加在网络上。-subtract_pin_load 只对网络有意义（减的是网络上
   的脚电容），不能用在端口上。 */
static const MstaSdcOpt s_vLoadOpts[] = {
    { "-min",               MSTA_SDC_FLAG, 0 },
    { "-max",               MSTA_SDC_FLAG, 0 },
    { "-subtract_pin_load", MSTA_SDC_FLAG, 0 },
    { "-pin_load",          MSTA_SDC_FLAG, 0 },
    { "-wire_load",         MSTA_SDC_FLAG, 0 },
    { NULL,                 MSTA_SDC_FLAG, 0 } };

static void Msta_SdcSetLoad( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    int fSubtract = Msta_SdcHasFlag( pCmd, "-subtract_pin_load" );
    int fMax = Msta_SdcHasFlag( pCmd, "-max" ) || !Msta_SdcHasFlag( pCmd, "-min" );
    int fMin = Msta_SdcHasFlag( pCmd, "-min" ) || !Msta_SdcHasFlag( pCmd, "-max" );
    int *pNets, nNets, i;
    double Load;
    if ( Msta_SdcHasFlag( pCmd, "-pin_load" ) && Msta_SdcHasFlag( pCmd, "-wire_load" ) )
    { Msta_SdcReject( p, pCmd, "-pin_load and -wire_load cannot be combined" ); return; }
    if ( !Msta_SdcGetNumber( p, pCmd, "value", pCmd->pValue, MSTA_SDC_NONNEG, &Load ) )
        return;
    for ( i = 0; fSubtract && i < pCmd->nObjs; i++ )
        if ( Msta_SdcKindOf( pCmd->ppObjs[i] ) == 'P' )
        { Msta_SdcReject( p, pCmd, "-subtract_pin_load cannot be used on port \"%s\"", pCmd->ppObjs[i] ); return; }
    Load *= p->CapScaleFf > 0.0 ? p->CapScaleFf : pLib->CapScale;
    if ( ( nNets = Msta_SdcObjectNets( p, pDes, pCmd, pCmd->ppObjs, pCmd->nObjs, &pNets ) ) < 0 )
        return;
    for ( i = 0; i < nNets; i++ )
    {
        MstaNetCons *pCons = Msta_SdcNetConsOrCreate( p, pNets[i] );
        if ( fMax ) pCons->LoadMax = Load;
        if ( fMin ) pCons->LoadMin = Load;
        if ( fSubtract ) pCons->fSubtractPinLoad = 1;
    }
}

/* set_input_transition [-rise|-fall] [-min|-max] [-clock 时钟] [-clock_fall] 摆率 端口列表
   可以按 -rise/-fall、-max/-min 分别给值，时序引擎用逐边沿字段起步。
   -clock / -clock_fall 只是标注，认下来不起作用。 */
static const MstaSdcOpt s_vInputTransitionOpts[] = {
    { "-min",        MSTA_SDC_FLAG,  0 },
    { "-max",        MSTA_SDC_FLAG,  0 },
    { "-rise",       MSTA_SDC_FLAG,  0 },
    { "-fall",       MSTA_SDC_FLAG,  0 },
    { "-clock",      MSTA_SDC_VALUE, 0 },
    { "-clock_fall", MSTA_SDC_FLAG,  0 },
    { NULL,          MSTA_SDC_FLAG,  0 } };

static void Msta_SdcSetInputTransition( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    int Sel[2][2], *pNets, nNets, i;
    double Slew;
    if ( !Msta_SdcGetNumber( p, pCmd, "value", pCmd->pValue, MSTA_SDC_NONNEG, &Slew ) )
        return;
    if ( ( nNets = Msta_SdcObjectNets( p, pDes, pCmd, pCmd->ppObjs, pCmd->nObjs, &pNets ) ) < 0 )
        return;
    Msta_SdcCmdMinMaxRiseFall( pCmd, Sel );
    for ( i = 0; i < nNets; i++ )
    {
        MstaNetCons *pCons = Msta_SdcNetConsOrCreate( p, pNets[i] );
        Msta_SdcStoreMinMaxRiseFall4( Sel, Slew * p->TimeScalePs,
                                      &pCons->InputSlewMaxRise, &pCons->InputSlewMaxFall,
                                      &pCons->InputSlewMinRise, &pCons->InputSlewMinFall );
    }
}

/* set_driving_cell -lib_cell 单元 [-library 库] [-pin 输出脚] [-from_pin 输入脚]
       [-multiply_by 倍数] [-input_transition_rise 值] [-input_transition_fall 值]
       [-min|-max] 端口列表
   根据驱动单元的输出转换表设置输入端口摆率，并记下驱动单元供分析时算延迟。
   -min/-max 只影响哪一角的摆率，msta 两个角都记同一份驱动单元。 */
static const MstaSdcOpt s_vDrivingCellOpts[] = {
    { "-lib_cell",              MSTA_SDC_VALUE, 0               },
    { "-library",               MSTA_SDC_VALUE, 0               },
    { "-pin",                   MSTA_SDC_VALUE, 0               },
    { "-from_pin",              MSTA_SDC_VALUE, 0               },
    { "-multiply_by",           MSTA_SDC_VALUE, 0               },
    { "-input_transition_rise", MSTA_SDC_VALUE, 0               },
    { "-input_transition_fall", MSTA_SDC_VALUE, 0               },
    { "-min",                   MSTA_SDC_FLAG,  0               },
    { "-max",                   MSTA_SDC_FLAG,  0               },
    { "-clock",                 MSTA_SDC_VALUE, MSTA_SDC_IGNORE },
    { "-rise",                  MSTA_SDC_FLAG,  MSTA_SDC_IGNORE },
    { "-fall",                  MSTA_SDC_FLAG,  MSTA_SDC_IGNORE },
    { "-dont_scale",            MSTA_SDC_FLAG,  MSTA_SDC_IGNORE },
    { "-no_design_rule",        MSTA_SDC_FLAG,  MSTA_SDC_IGNORE },
    { "-clock_fall",            MSTA_SDC_FLAG,  MSTA_SDC_IGNORE },
    { NULL,                     MSTA_SDC_FLAG,  0               } };

static void Msta_SdcSetDrivingCell( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    const char *pCellName = Msta_SdcOptValue( pCmd, "-lib_cell" );
    const char *pLibName  = Msta_SdcOptValue( pCmd, "-library" );
    const char *pPinName  = Msta_SdcOptValue( pCmd, "-pin" );
    const char *pFromPin  = Msta_SdcOptValue( pCmd, "-from_pin" );
    const char *pMult     = Msta_SdcOptValue( pCmd, "-multiply_by" );
    const char *pInRise   = Msta_SdcOptValue( pCmd, "-input_transition_rise" );
    const char *pInFall   = Msta_SdcOptValue( pCmd, "-input_transition_fall" );
    double Mult = 1.0, InRise = 0.0, InFall = 0.0, Slew = 0.0;
    char sCellQual[512];
    MstaCell *pCell;
    int *pNets, nNets, i, k;

    if ( !Msta_SdcRequire( p, pCmd, "-lib_cell" ) )
        return;
    if ( ( pMult   && !Msta_SdcGetNumber( p, pCmd, "-multiply_by", pMult, MSTA_SDC_POSITIVE, &Mult ) ) ||
         ( pInRise && !Msta_SdcGetNumber( p, pCmd, "-input_transition_rise", pInRise, MSTA_SDC_NONNEG, &InRise ) ) ||
         ( pInFall && !Msta_SdcGetNumber( p, pCmd, "-input_transition_fall", pInFall, MSTA_SDC_NONNEG, &InFall ) ) )
        return;
    /* 多库时用 "库名/cell 名" 限定（SDC 手册的 -library + -lib_cell 组合）。 */
    if ( pLibName != NULL && strchr( pCellName, '/' ) == NULL )
    {
        snprintf( sCellQual, sizeof(sCellQual), "%s/%s", pLibName, pCellName );
        pCellName = sCellQual;
    }
    pCell = Msta_LibFindCell( pLib, pCellName );
    if ( pCell == NULL )
    { Msta_SdcReject( p, pCmd, "cell \"%s\" is not in the requested library", pCellName ); return; }
    for ( k = 0; k < pCell->vArcs.nSize && Slew <= 0.0; k++ )
    {
        MstaArc *pArc = MstaArcArrayAt( &pCell->vArcs, k );
        if ( pPinName && pArc->OutPin != Msta_NameId(pPinName) ) continue;
        if ( Msta_TableExists( &pArc->TransRise ) )
            Slew = pArc->TransRise.pValues[0] * pLib->TimeScale;
        else if ( Msta_TableExists( &pArc->TransFall ) )
            Slew = pArc->TransFall.pValues[0] * pLib->TimeScale;
    }
    if ( ( nNets = Msta_SdcObjectNets( p, pDes, pCmd, pCmd->ppObjs, pCmd->nObjs, &pNets ) ) < 0 )
        return;
    for ( i = 0; i < nNets; i++ )
    {
        MstaNetCons *pCons = Msta_SdcNetConsOrCreate( p, pNets[i] );
        pCons->DrivingCell    = Msta_NameId( pCellName );
        pCons->DrivingPin     = pPinName ? Msta_NameId( pPinName ) : MSTA_NO_ID;
        pCons->DrivingFromPin = pFromPin ? Msta_NameId( pFromPin ) : MSTA_NO_ID;
        pCons->DriveMultiply  = Mult;
        if ( pInRise ) pCons->DriveInSlewRise = InRise * p->TimeScalePs;
        if ( pInFall ) pCons->DriveInSlewFall = InFall * p->TimeScalePs;
        if ( !Msta_IsSet( pCons->InputSlewMaxRise ) ) pCons->InputSlewMaxRise = Slew;
        if ( !Msta_IsSet( pCons->InputSlewMaxFall ) ) pCons->InputSlewMaxFall = Slew;
        if ( !Msta_IsSet( pCons->InputSlewMinRise ) ) pCons->InputSlewMinRise = Slew;
        if ( !Msta_IsSet( pCons->InputSlewMinFall ) ) pCons->InputSlewMinFall = Slew;
    }
}

/* 理想网络类命令的对象都是网络；时钟对象没建模。 */
static int Msta_SdcIdealNets( MstaSdc *p, MstaDesign *pDes, const MstaSdcCmd *pCmd, int **ppNets )
{
    int i;
    for ( i = 0; i < pCmd->nObjs; i++ )
        if ( Msta_SdcKindOf( pCmd->ppObjs[i] ) == 'C' )
        { Msta_SdcReject( p, pCmd, "clock objects are not modeled" ); return -1; }
    return Msta_SdcObjectNets( p, pDes, pCmd, pCmd->ppObjs, pCmd->nObjs, ppNets );
}

/* set_ideal_network [-no_propagate] 对象列表
   把对象标成理想网络；不带 -no_propagate 时理想属性沿组合扇出继续往下传。
   -no_propagation 是方言拼写，按 -no_propagate 处理；-force 不建模。 */
static const MstaSdcOpt s_vIdealNetworkOpts[] = {
    { "-no_propagate",   MSTA_SDC_FLAG, 0               },
    { "-no_propagation", MSTA_SDC_FLAG, 0               },
    { "-force",          MSTA_SDC_FLAG, MSTA_SDC_IGNORE },
    { NULL,              MSTA_SDC_FLAG, 0               } };

static void Msta_SdcSetIdealNetwork( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    int fNoProp = Msta_SdcHasFlag( pCmd, "-no_propagate" ) || Msta_SdcHasFlag( pCmd, "-no_propagation" );
    int *pNets, nNets, i;
    if ( Msta_SdcHasFlag( pCmd, "-no_propagation" ) )
        Msta_SdcNote( pCmd, "-no_propagation is not SDC 1.8 syntax; honored as -no_propagate" );
    if ( ( nNets = Msta_SdcIdealNets( p, pDes, pCmd, &pNets ) ) < 0 )
        return;
    for ( i = 0; i < nNets; i++ )
    {
        MstaNetCons *pCons = Msta_SdcNetConsOrCreate( p, pNets[i] );
        pCons->fIdeal = 1;
        if ( fNoProp ) pCons->fIdealNoPropagate = 1;
    }
}

/* set_ideal_latency [-rise|-fall] [-min|-max] 延迟 对象列表 */
static void Msta_SdcSetIdealLatency( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    int *pNets, nNets, i, Sel[2][2];
    if ( ( nNets = Msta_SdcIdealNets( p, pDes, pCmd, &pNets ) ) < 0 )
        return;
    Msta_SdcCmdMinMaxRiseFall( pCmd, Sel );
    for ( i = 0; i < nNets; i++ )
    {
        MstaNetCons *pCons = Msta_SdcNetConsOrCreate( p, pNets[i] );
        Msta_SdcStoreMinMaxRiseFall4( Sel, Msta_SdcToPs( p, pCmd->pValue ),
                                      &pCons->IdealLatencyMaxRise, &pCons->IdealLatencyMaxFall,
                                      &pCons->IdealLatencyMinRise, &pCons->IdealLatencyMinFall );
    }
}

/* set_ideal_transition [-rise|-fall] [-min|-max] 摆率 对象列表 */
static void Msta_SdcSetIdealTransition( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    int *pNets, nNets, i, Sel[2][2];
    double Slew;
    if ( !Msta_SdcGetNumber( p, pCmd, "value", pCmd->pValue, MSTA_SDC_NONNEG, &Slew ) )
        return;
    if ( ( nNets = Msta_SdcIdealNets( p, pDes, pCmd, &pNets ) ) < 0 )
        return;
    Msta_SdcCmdMinMaxRiseFall( pCmd, Sel );
    for ( i = 0; i < nNets; i++ )
    {
        MstaNetCons *pCons = Msta_SdcNetConsOrCreate( p, pNets[i] );
        Msta_SdcStoreMinMaxRiseFall4( Sel, Slew * p->TimeScalePs,
                                      &pCons->IdealTranMaxRise, &pCons->IdealTranMaxFall,
                                      &pCons->IdealTranMinRise, &pCons->IdealTranMinFall );
    }
}

/* 把网络钉成常量。nCase 是 fCaseValue 编码：1 = 逻辑 0，2 = 逻辑 1，
   3 = dc（无关值，按"不传播"处理，路径到这里断开；见 msta_net.h）。 */
static void Msta_SdcSetConstNets( MstaSdc *p, MstaDesign *pDes, MstaSdcCmd *pCmd, int nCase )
{
    int *pNets, nNets, i;
    if ( ( nNets = Msta_SdcObjectNets( p, pDes, pCmd, pCmd->ppObjs, pCmd->nObjs, &pNets ) ) < 0 )
        return;
    for ( i = 0; i < nNets; i++ )
    {
        MstaNet *pNet = MstaNetArrayAt( &pDes->vNets, pNets[i] );
        pNet->fCaseValue = nCase;
        pNet->fConst = nCase == 3 ? 0 : nCase;
    }
}

/* set_case_analysis 0|1|zero|one 对象列表
   把选中的网络钉成常量。rise/fall 这类"只能沿某个边沿翻转"的值不建模。 */
static void Msta_SdcSetCaseAnalysis( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    const char *pValue = pCmd->pValue;
    if ( !strcmp( pValue, "0" ) || !strcmp( pValue, "zero" ) )
        Msta_SdcSetConstNets( p, pDes, pCmd, 1 );
    else if ( !strcmp( pValue, "1" ) || !strcmp( pValue, "one" ) )
        Msta_SdcSetConstNets( p, pDes, pCmd, 2 );
    else if ( !strcmp( pValue, "rise" ) || !strcmp( pValue, "rising" ) ||
              !strcmp( pValue, "fall" ) || !strcmp( pValue, "falling" ) )
        Msta_SdcReject( p, pCmd, "value \"%s\" is not modeled (only 0, 1, zero and one)", pValue );
    else
        Msta_SdcReject( p, pCmd, "value \"%s\" must be 0, 1, zero or one", pValue );
}

/* set_logic_zero / set_logic_one / set_logic_dc 端口列表 */
static void Msta_SdcSetLogicZero( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    Msta_SdcSetConstNets( p, pDes, pCmd, 1 );
}

static void Msta_SdcSetLogicOne( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    Msta_SdcSetConstNets( p, pDes, pCmd, 2 );
}

static void Msta_SdcSetLogicDc( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    Msta_SdcSetConstNets( p, pDes, pCmd, 3 );
}

/* set_disable_timing [-from 引脚名] [-to 引脚名] 实例列表
   屏蔽指定实例上从 -from 到 -to 的时序弧。两个端点可以只写一个，
   缺的那个按通配处理（与 SDC 一致）。-from/-to 各是一个库引脚名。 */
static const MstaSdcOpt s_vDisableTimingOpts[] = {
    { "-from", MSTA_SDC_VALUE, 0 },
    { "-to",   MSTA_SDC_VALUE, 0 },
    { NULL,    MSTA_SDC_FLAG,  0 } };

static void Msta_SdcSetDisableTiming( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    const char *pFrom = Msta_SdcOptValue( pCmd, "-from" );
    const char *pTo   = Msta_SdcOptValue( pCmd, "-to" );
    int *pInsts, nInsts, i;
    if ( pFrom == NULL && pTo == NULL )
    { Msta_SdcReject( p, pCmd, "needs -from and/or -to" ); return; }
    if ( ( nInsts = Msta_SdcObjectInsts( p, pDes, pLib, pCmd, pCmd->ppObjs, pCmd->nObjs, 0, &pInsts ) ) < 0 )
        return;
    for ( i = 0; i < nInsts; i++ )
    {
        MstaInst *pInst = MstaInstArrayAt( &pDes->vInsts, pInsts[i] );
        MstaDisabledArc *pArc = MstaDisabledArcArrayAppend( &p->vDisabledArcs );
        pArc->Inst = pInsts[i];
        pArc->FromPin = pFrom ? Msta_NameId(pFrom) : MSTA_NO_ID;
        pArc->ToPin = pTo ? Msta_NameId(pTo) : MSTA_NO_ID;
        if ( ( pFrom && Msta_CellPinIndexOf(pInst->pCell,pArc->FromPin) < 0 ) ||
             ( pTo && Msta_CellPinIndexOf(pInst->pCell,pArc->ToPin) < 0 ) )
            Msta_SdcNote( pCmd, "instance \"%s\" has no matching pins", Msta_InstName(pDes,pInsts[i]) );
    }
}

/* ---------------- 路径例外与附加检查 ---------------- */

/* 把路径描述写进例外表：-from 与 -to 的对象取笛卡尔积，每一对一条记录；
   没写 -from（或 -to）时那一端不限定。 */
static void Msta_SdcAddPathExceptions( MstaSdc *p, const MstaSdcPath *pPath, int fFalse,
                                       int fSetup, int fHold, int nCycles,
                                       int fMaxDelay, int fMinDelay, double Delay )
{
    int nFrom = pPath->nFrom > 0 ? pPath->nFrom : 1;
    int nTo   = pPath->nTo   > 0 ? pPath->nTo   : 1;
    int j, k;
    for ( j = 0; j < nFrom; j++ )
        for ( k = 0; k < nTo; k++ )
        {
            MstaException *pEx = MstaExceptionArrayAppend(&p->vExceptions);
            const char *pFrom = pPath->nFrom > 0 ? pPath->ppFrom[j] : NULL;
            const char *pTo   = pPath->nTo   > 0 ? pPath->ppTo[k]   : NULL;
            pEx->FromText = pFrom ? Msta_NameId(pFrom) : MSTA_NO_ID;
            pEx->ToText = pTo ? Msta_NameId(pTo) : MSTA_NO_ID;
            pEx->FromKind = pFrom ? Msta_SdcKindOf(pFrom) : 0;
            pEx->ToKind = pTo ? Msta_SdcKindOf(pTo) : 0;
            pEx->FromRF = pPath->FromRF;
            pEx->ToRF = pPath->ToRF;
            memcpy( pEx->Thru, pPath->Thru, sizeof(pPath->Thru) );
            pEx->nThru = pPath->nThru;
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

/* set_false_path [-setup] [-hold] [-rise|-fall] [路径选项...]
   set_multicycle_path [-setup] [-hold] [-rise|-fall] [路径选项...] 周期数
   路径选项是 -from/-through/-to 及其 -rise_/-fall_ 写法；只写 -rise 或 -fall 时
   它限定 -to 一端的边沿。周期数必须是正整数。 */
static const MstaSdcOpt s_vPathExceptionOpts[] = {
    { "-setup",   MSTA_SDC_FLAG, 0                             },
    { "-hold",    MSTA_SDC_FLAG, 0                             },
    { "-rise",    MSTA_SDC_FLAG, 0                             },
    { "-fall",    MSTA_SDC_FLAG, 0                             },
    { "-from",    MSTA_SDC_LIST, MSTA_SDC_RF                   },
    { "-through", MSTA_SDC_LIST, MSTA_SDC_RF | MSTA_SDC_REPEAT },
    { "-to",      MSTA_SDC_LIST, MSTA_SDC_RF                   },
    { NULL,       MSTA_SDC_FLAG, 0                             } };

static void Msta_SdcSetPathException( MstaSdc *p, int fFalse, MstaSdcCmd *pCmd )
{
    double Cycles = 1.0;
    MstaSdcPath Path;
    if ( !fFalse )
    {
        if ( !Msta_SdcGetNumber( p, pCmd, "cycle count", pCmd->pValue, MSTA_SDC_POSITIVE, &Cycles ) )
            return;
        if ( Cycles != (double)(int)Cycles )
        { Msta_SdcReject( p, pCmd, "cycle count must be a positive integer (got %s)", pCmd->pValue ); return; }
    }
    if ( !Msta_SdcGetPath( p, pCmd, &Path ) )
        return;
    if ( Path.ToRF == 0 )
        Path.ToRF = Msta_SdcCmdEdge( pCmd );
    Msta_SdcAddPathExceptions( p, &Path, fFalse, Msta_SdcHasFlag( pCmd, "-setup" ),
                               Msta_SdcHasFlag( pCmd, "-hold" ), (int)Cycles, 0, 0, 0.0 );
}

static void Msta_SdcSetFalsePath( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    Msta_SdcSetPathException( p, 1, pCmd );
}

static void Msta_SdcSetMulticyclePath( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    Msta_SdcSetPathException( p, 0, pCmd );
}

/* set_max_delay / set_min_delay [路径选项...] 延迟
   路径选项同 set_false_path。 */
static const MstaSdcOpt s_vPathDelayOpts[] = {
    { "-from",    MSTA_SDC_LIST, MSTA_SDC_RF                   },
    { "-through", MSTA_SDC_LIST, MSTA_SDC_RF | MSTA_SDC_REPEAT },
    { "-to",      MSTA_SDC_LIST, MSTA_SDC_RF                   },
    { NULL,       MSTA_SDC_FLAG, 0                             } };

static void Msta_SdcSetPathDelay( MstaSdc *p, int fMax, MstaSdcCmd *pCmd )
{
    MstaSdcPath Path;
    if ( !Msta_SdcGetPath( p, pCmd, &Path ) )
        return;
    Msta_SdcAddPathExceptions( p, &Path, 0, 0, 0, 1, fMax, !fMax, Msta_SdcToPs( p, pCmd->pValue ) );
}

static void Msta_SdcSetMaxDelay( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    Msta_SdcSetPathDelay( p, 1, pCmd );
}

static void Msta_SdcSetMinDelay( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    Msta_SdcSetPathDelay( p, 0, pCmd );
}

/* group_path -name 组名 | -default [-weight 权重] [-critical_range 值] [路径选项...]
   把命中的路径归到一个分组里，报告按组统计 WNS/TNS。
   分组不影响 slack；-weight 只记录并在报告里显示，不参与 WNS/TNS 等数字的计算。
   -name 与 -default 必须二选一。不写路径选项就是"所有路径"（-default 常这么用）。
   -critical_range 不建模，分组收下所有命中的路径。 */
static const MstaSdcOpt s_vGroupPathOpts[] = {
    { "-default",        MSTA_SDC_FLAG,  0                             },
    { "-name",           MSTA_SDC_VALUE, 0                             },
    { "-weight",         MSTA_SDC_VALUE, 0                             },
    { "-critical_range", MSTA_SDC_VALUE, MSTA_SDC_IGNORE               },
    { "-from",           MSTA_SDC_LIST,  MSTA_SDC_RF                   },
    { "-through",        MSTA_SDC_LIST,  MSTA_SDC_RF | MSTA_SDC_REPEAT },
    { "-to",             MSTA_SDC_LIST,  MSTA_SDC_RF                   },
    { NULL,              MSTA_SDC_FLAG,  0                             } };

static void Msta_SdcSetGroupPath( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    const char *pName   = Msta_SdcOptValue( pCmd, "-name" );
    const char *pWeight = Msta_SdcOptValue( pCmd, "-weight" );
    int fDefault = Msta_SdcHasFlag( pCmd, "-default" );
    double Weight = 1.0;
    MstaSdcPath Path;
    int j, k;

    if ( fDefault == ( pName != NULL ) )
    { Msta_SdcReject( p, pCmd, "needs exactly one of -name and -default" ); return; }
    if ( pWeight && !Msta_SdcGetNumber( p, pCmd, "-weight", pWeight, MSTA_SDC_POSITIVE, &Weight ) )
        return;
    if ( !Msta_SdcGetPath( p, pCmd, &Path ) )
        return;
    /* -from/-to 的多个对象按笛卡尔积摊成多条记录，名字与权重相同（与例外一致）。 */
    for ( j = 0; j < ( Path.nFrom > 0 ? Path.nFrom : 1 ); j++ )
        for ( k = 0; k < ( Path.nTo > 0 ? Path.nTo : 1 ); k++ )
        {
            const char *pFrom = Path.nFrom > 0 ? Path.ppFrom[j] : NULL;
            const char *pTo   = Path.nTo   > 0 ? Path.ppTo[k]   : NULL;
            MstaPathGroup *pGroup = MstaPathGroupArrayAppend( &p->vPathGroups );
            pGroup->Name    = fDefault ? MSTA_NO_ID : Msta_NameId( pName );
            pGroup->fDefault= fDefault;
            pGroup->Weight  = Weight;
            pGroup->FromText= pFrom ? Msta_NameId(pFrom) : MSTA_NO_ID;
            pGroup->ToText  = pTo   ? Msta_NameId(pTo)   : MSTA_NO_ID;
            pGroup->FromKind= pFrom ? Msta_SdcKindOf(pFrom) : 0;
            pGroup->ToKind  = pTo   ? Msta_SdcKindOf(pTo)   : 0;
            pGroup->FromRF  = Path.FromRF;
            pGroup->ToRF    = Path.ToRF;
            memcpy( pGroup->Thru, Path.Thru, sizeof(Path.Thru) );
            pGroup->nThru   = Path.nThru;
        }
}

/* set_data_check -from|-rise_from|-fall_from A -to|-rise_to|-fall_to B
       [-setup|-hold] [-clock C] 余量
   两条数据路径之间的检查，-from 是参照；-from/-to 各是一个对象，解析成一个网络。
   -clock 读入但不使用；-rise/-fall 不建模。不写 -setup/-hold 时两个都查。 */
static const MstaSdcOpt s_vDataCheckOpts[] = {
    { "-from",  MSTA_SDC_LIST,  MSTA_SDC_RF     },
    { "-to",    MSTA_SDC_LIST,  MSTA_SDC_RF     },
    { "-clock", MSTA_SDC_VALUE, 0               },
    { "-setup", MSTA_SDC_FLAG,  0               },
    { "-hold",  MSTA_SDC_FLAG,  0               },
    { "-rise",  MSTA_SDC_FLAG,  MSTA_SDC_IGNORE },
    { "-fall",  MSTA_SDC_FLAG,  MSTA_SDC_IGNORE },
    { NULL,     MSTA_SDC_FLAG,  0               } };

static void Msta_SdcSetDataCheck( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    char **ppFrom, **ppTo;
    int nFrom = Msta_SdcOptList( pCmd, "-from", &ppFrom );
    int nTo   = Msta_SdcOptList( pCmd, "-to", &ppTo );
    int *pFromNets, *pToNets;
    MstaDataCheck *pCheck;

    if ( nFrom == 0 || nTo == 0 )
    { Msta_SdcReject( p, pCmd, "needs -from and -to" ); return; }
    if ( nFrom != 1 || nTo != 1 ||
         Msta_SdcResolveNets( pDes, ppFrom[0], &pFromNets ) != 1 ||
         Msta_SdcResolveNets( pDes, ppTo[0], &pToNets ) != 1 )
    { Msta_SdcReject( p, pCmd, "-from/-to must each resolve to one net" ); return; }
    pCheck = MstaDataCheckArrayAppend( &p->vDataChecks );
    pCheck->FromNet  = pFromNets[0];
    pCheck->ToNet    = pToNets[0];
    pCheck->FromText = Msta_NameId( ppFrom[0] );
    pCheck->ToText   = Msta_NameId( ppTo[0] );
    pCheck->FromRF   = Msta_SdcOptEdge( pCmd, "-from" );
    pCheck->ToRF     = Msta_SdcOptEdge( pCmd, "-to" );
    pCheck->fSetup   = Msta_SdcHasFlag( pCmd, "-setup" ) || !Msta_SdcHasFlag( pCmd, "-hold" );
    pCheck->fHold    = Msta_SdcHasFlag( pCmd, "-hold" ) || !Msta_SdcHasFlag( pCmd, "-setup" );
    pCheck->Value    = Msta_SdcToPs( p, pCmd->pValue );
}

/* set_max_time_borrow 延迟 [锁存器实例或库单元...]
   锁存器 D 脚允许比使能脚关闭沿晚到多久。不写对象列表就是所有锁存器的默认值
   （手册里对象列表是必写的，这里放宽）。 */
static void Msta_SdcSetMaxTimeBorrow( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    MstaBorrowSdc *pRec;
    double Value;
    int *pInsts, nInsts = 1, i;
    if ( !Msta_SdcGetNumber( p, pCmd, "value", pCmd->pValue, MSTA_SDC_NONNEG, &Value ) )
        return;
    if ( pCmd->nObjs > 0 &&
         ( nInsts = Msta_SdcObjectInsts( p, pDes, pLib, pCmd, pCmd->ppObjs, pCmd->nObjs, 1, &pInsts ) ) < 0 )
        return;
    for ( i = 0; i < nInsts; i++ )
    {
        pRec = MstaBorrowSdcArrayAppend( &p->vBorrow );
        pRec->Inst  = pCmd->nObjs > 0 ? pInsts[i] : -1;
        pRec->Value = Value * p->TimeScalePs;
    }
}

/* set_clock_gating_check [-setup 值] [-hold 值] [-rise|-fall|-high|-low] [门控实例或库单元...]
   门控单元使能脚相对时钟脚的 setup/hold 值。
   没有对象列表时作为全局默认；带对象列表时按实例（或库单元名）覆盖。
   -rise/-fall/-high/-low 不建模：检查对象总按库里声明的有效沿。 */
static const MstaSdcOpt s_vClockGatingCheckOpts[] = {
    { "-setup", MSTA_SDC_VALUE, 0               },
    { "-hold",  MSTA_SDC_VALUE, 0               },
    { "-rise",  MSTA_SDC_FLAG,  MSTA_SDC_IGNORE },
    { "-fall",  MSTA_SDC_FLAG,  MSTA_SDC_IGNORE },
    { "-high",  MSTA_SDC_FLAG,  MSTA_SDC_IGNORE },
    { "-low",   MSTA_SDC_FLAG,  MSTA_SDC_IGNORE },
    { NULL,     MSTA_SDC_FLAG,  0               } };

static void Msta_SdcSetClockGatingCheck( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    const char *pSetup = Msta_SdcOptValue( pCmd, "-setup" );
    const char *pHold  = Msta_SdcOptValue( pCmd, "-hold" );
    double Setup = MSTA_UNSET, Hold = MSTA_UNSET;
    int *pInsts, nInsts = 1, i;
    if ( pSetup == NULL && pHold == NULL )
    { Msta_SdcReject( p, pCmd, "needs -setup and/or -hold" ); return; }
    if ( ( pSetup && !Msta_SdcGetNumber( p, pCmd, "-setup", pSetup, MSTA_SDC_ANY, &Setup ) ) ||
         ( pHold  && !Msta_SdcGetNumber( p, pCmd, "-hold",  pHold,  MSTA_SDC_ANY, &Hold ) ) )
        return;
    if ( pSetup ) Setup *= p->TimeScalePs;
    if ( pHold )  Hold  *= p->TimeScalePs;
    if ( pCmd->nObjs > 0 &&
         ( nInsts = Msta_SdcObjectInsts( p, pDes, pLib, pCmd, pCmd->ppObjs, pCmd->nObjs, 1, &pInsts ) ) < 0 )
        return;
    for ( i = 0; i < nInsts; i++ )
    {
        MstaClockGatingSdc *pRec = MstaClockGatingSdcArrayAppend( &p->vClockGating );
        pRec->Inst  = pCmd->nObjs > 0 ? pInsts[i] : -1;
        pRec->Setup = Setup;
        pRec->Hold  = Hold;
        pRec->fRise = pRec->fFall = pRec->fHigh = pRec->fLow = 0;
    }
}

/* ---------------- 设计规则、derate、工作条件与单位 ---------------- */

/* set_max_transition / set_max_fanout / set_max_capacitance / set_min_capacitance
       [-clock_path] [-data_path] [-rise] [-fall] 限制值 对象列表
   [current_design] 是全局限制；给端口/网络/引脚/单元时记在对应的网络上
   （单元记在它驱动的网络上），检查在时序分析之后统一做（按对象优先，没写对象
   的用全局值）。时钟对象（时钟域限制）不建模。-clock_path/-data_path/-rise/-fall
   不建模，msta 一律按最坏角检查。 */
static const MstaSdcOpt s_vDrcLimitOpts[] = {
    { "-clock_path", MSTA_SDC_FLAG, MSTA_SDC_IGNORE },
    { "-data_path",  MSTA_SDC_FLAG, MSTA_SDC_IGNORE },
    { "-rise",       MSTA_SDC_FLAG, MSTA_SDC_IGNORE },
    { "-fall",       MSTA_SDC_FLAG, MSTA_SDC_IGNORE },
    { NULL,          MSTA_SDC_FLAG, 0               } };

/* nWhich：0 = max_transition，1 = max_fanout，2 = max_capacitance，3 = min_capacitance。
   pTarget 是 MstaSdc 的全局字段或 MstaNetCons 的分对象字段（四个按 nWhich 排）。 */
static void Msta_SdcStoreDrcLimit( double *pMaxTran, double *pMaxFanout, double *pMaxCap,
                                   double *pMinCap, int nWhich, double Value )
{
    if      ( nWhich == 0 ) *pMaxTran   = Value;
    else if ( nWhich == 1 ) *pMaxFanout = Value;
    else if ( nWhich == 2 ) *pMaxCap    = Value;
    else                    *pMinCap    = Value;
}

static void Msta_SdcSetDrcLimit( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib,
                                 int nWhich, MstaSdcCmd *pCmd )
{
    MstaSdcIntArray vNets;
    double Value;
    int i, j, fDesign = 0;

    if ( !Msta_SdcGetNumber( p, pCmd, "limit", pCmd->pValue, MSTA_SDC_POSITIVE, &Value ) )
        return;
    if ( nWhich == 0 )
        Value *= p->TimeScalePs;
    else if ( nWhich >= 2 )
        Value *= p->CapScaleFf > 0.0 ? p->CapScaleFf : pLib->CapScale;
    for ( i = 0; i < pCmd->nObjs; i++ )
        if ( Msta_SdcKindOf( pCmd->ppObjs[i] ) == 'C' )
        { Msta_SdcReject( p, pCmd, "clock objects (clock-domain limits) are not modeled" ); return; }
    /* 先把所有对象都解析成网络，一个都没有时整条作废。 */
    MstaSdcIntArrayInit( &vNets );
    for ( i = 0; i < pCmd->nObjs; i++ )
    {
        const char *pObj = pCmd->ppObjs[i];
        char Kind = Msta_SdcKindOf( pObj );
        if ( Kind == 'D' )
            fDesign = 1;
        else if ( Kind == 'I' )
        {
            int nInst = Msta_DesignFindInstByName( pDes, pObj );
            MstaInst *pInst;
            if ( nInst < 0 )
            { Msta_SdcNote( pCmd, "unknown instance \"%s\"", pObj ); continue; }
            pInst = MstaInstArrayAt( &pDes->vInsts, nInst );
            for ( j = 0; j < pInst->nPins && j < pInst->pCell->vPins.nSize; j++ )
                if ( pInst->pNets[j] >= 0 && pInst->pCell->vPins.pData[j].Dir == MSTA_DIR_OUTPUT )
                    *MstaSdcIntArrayAppend( &vNets ) = pInst->pNets[j];
        }
        else
            Msta_SdcResolveNetsInto( pDes, pObj, &vNets );
    }
    Msta_SdcArenaKeep( vNets.pData );
    if ( !fDesign && vNets.nSize == 0 )
    { Msta_SdcReject( p, pCmd, "none of the objects was found" ); return; }
    if ( fDesign )
        Msta_SdcStoreDrcLimit( &p->MaxTransition, &p->MaxFanout, &p->MaxCapacitance,
                               &p->MinCapacitance, nWhich, Value );
    for ( i = 0; i < vNets.nSize; i++ )
    {
        MstaNetCons *pCons = Msta_SdcNetConsOrCreate( p, vNets.pData[i] );
        Msta_SdcStoreDrcLimit( &pCons->DrcMaxTransition, &pCons->DrcMaxFanout,
                               &pCons->DrcMaxCapacitance, &pCons->DrcMinCapacitance, nWhich, Value );
    }
}

static void Msta_SdcSetMaxTransition( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    Msta_SdcSetDrcLimit( p, pDes, pLib, 0, pCmd );
}

static void Msta_SdcSetMaxFanout( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    Msta_SdcSetDrcLimit( p, pDes, pLib, 1, pCmd );
}

static void Msta_SdcSetMaxCapacitance( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    Msta_SdcSetDrcLimit( p, pDes, pLib, 2, pCmd );
}

static void Msta_SdcSetMinCapacitance( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    Msta_SdcSetDrcLimit( p, pDes, pLib, 3, pCmd );
}

/* set_max_area 面积
   整个设计的面积目标，单位跟随库。 */
static void Msta_SdcSetMaxArea( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    double Area;
    if ( Msta_SdcGetNumber( p, pCmd, "area", pCmd->pValue, MSTA_SDC_NONNEG, &Area ) )
        p->MaxArea = Area;
}

/* set_timing_derate [-early|-late] [-cell_delay|-net_delay|-cell_check]
                     [-clock|-data] [-rise|-fall] 系数 [实例或时钟列表]
   不带对象时是全局系数；带对象时只对那个实例/时钟生效（分对象的值覆盖全局值，
   不是相乘）。-clock 管时钟树上的弧，-data 管数据路径上的单元延迟
   （含 FF 的 clk-to-Q），不写这两个开关时两者都算。msta 没有线延迟，
   只写 -net_delay 的约束作废。 */
static const MstaSdcOpt s_vTimingDerateOpts[] = {
    { "-early",      MSTA_SDC_FLAG, 0 },
    { "-late",       MSTA_SDC_FLAG, 0 },
    { "-cell_delay", MSTA_SDC_FLAG, 0 },
    { "-cell_check", MSTA_SDC_FLAG, 0 },
    { "-net_delay",  MSTA_SDC_FLAG, 0 },
    { "-clock",      MSTA_SDC_FLAG, 0 },
    { "-data",       MSTA_SDC_FLAG, 0 },
    { "-rise",       MSTA_SDC_FLAG, 0 },
    { "-fall",       MSTA_SDC_FLAG, 0 },
    { NULL,          MSTA_SDC_FLAG, 0 } };

static void Msta_SdcSetTimingDerate( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    int fEarly     = Msta_SdcHasFlag( pCmd, "-early" ) || !Msta_SdcHasFlag( pCmd, "-late" );
    int fLate      = Msta_SdcHasFlag( pCmd, "-late" ) || !Msta_SdcHasFlag( pCmd, "-early" );
    int fCellCheck = Msta_SdcHasFlag( pCmd, "-cell_check" );
    int fCellDelay = Msta_SdcHasFlag( pCmd, "-cell_delay" ) ||
                     ( !fCellCheck && !Msta_SdcHasFlag( pCmd, "-net_delay" ) );
    int fClock     = Msta_SdcHasFlag( pCmd, "-clock" ) || !Msta_SdcHasFlag( pCmd, "-data" );
    int fData      = Msta_SdcHasFlag( pCmd, "-data" ) || !Msta_SdcHasFlag( pCmd, "-clock" );
    char Edge      = Msta_SdcCmdEdge( pCmd );
    double Factor;
    int i, nApplied = 0;

    if ( !fCellDelay && !fCellCheck )
    { Msta_SdcReject( p, pCmd, "-net_delay is not modeled (msta has no net delay)" ); return; }
    if ( !Msta_SdcGetNumber( p, pCmd, "factor", pCmd->pValue, MSTA_SDC_POSITIVE, &Factor ) )
        return;
    /* 分对象只支持实例（get_cells）和时钟（get_clocks）；其他种类的对象在 msta
       的模型里没有对应量，整条作废。 */
    for ( i = 0; i < pCmd->nObjs; i++ )
    {
        char Kind = Msta_SdcKindOf( pCmd->ppObjs[i] );
        if ( Kind != 0 && Kind != 'C' && Kind != 'I' )
        {
            Msta_SdcReject( p, pCmd, "object \"%s\" is not modeled (only instances and clocks)",
                            pCmd->ppObjs[i] );
            return;
        }
    }
    if ( pCmd->nObjs == 0 )
    {
        p->fDerateClock = fCellDelay && fClock;
        p->fDerateData  = fCellDelay && fData;
        if ( fEarly ) p->DerateEarly = Factor;
        if ( fLate )  p->DerateLate  = Factor;
        if ( fCellCheck && fEarly ) p->DerateCheckEarly = Factor;
        if ( fCellCheck && fLate )  p->DerateCheckLate  = Factor;
        return;
    }
    for ( i = 0; i < pCmd->nObjs; i++ )
    {
        const char *pObj = pCmd->ppObjs[i];
        MstaClock *pClock = Msta_SdcKindOf( pObj ) == 'C' ? Msta_SdcFindClock( p, pObj ) : NULL;
        int nInst = pClock ? -1 : Msta_DesignFindInstByName( pDes, pObj );
        MstaObjDerate *pRec;
        if ( pClock == NULL && nInst < 0 )
        {
            Msta_SdcNote( pCmd, Msta_SdcKindOf( pObj ) == 'C' ? "unknown clock \"%s\"" : "unknown instance \"%s\"", pObj );
            continue;
        }
        if ( pClock && Edge != 0 )
            Msta_SdcNote( pCmd, "-rise/-fall on a clock object is not modeled; "
                          "the clock tree keeps rise/fall merged" );
        pRec = MstaObjDerateArrayAppend( &p->vObjDerate );
        pRec->Kind  = pClock ? 'C' : 'I';
        pRec->Inst  = nInst;
        pRec->Name  = pClock ? pClock->Name : MSTA_NO_ID;
        pRec->fRise = Edge == 'r';
        pRec->fFall = Edge == 'f';
        pRec->fCellCheck = fCellCheck;
        pRec->Early = fEarly ? Factor : 1.0;
        pRec->Late  = fLate  ? Factor : 1.0;
        nApplied++;
    }
    if ( nApplied == 0 )
        Msta_SdcReject( p, pCmd, "none of the objects was found" );
}

/* 没点名工作条件时用的角：库里 default_operating_conditions 指的那个，没有就取
   第一个。nLibrary 不是 MSTA_NO_ID 时只看这个库。返回角所在的库，角写进 *ppCond。 */
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
   库没声明 K 因子时系数保持 1.0，电压/温度只记录、报告。pCmd 是触发它的命令
   （告警里用）；fWarnNoK=0 时"库里没有 K 因子"由调用方自己告警。
   返回 1 表示至少一个角用上了库里的 K 因子。 */
static int Msta_SdcUpdateKFactor( MstaSdc *p, MstaLib *pLib, const MstaSdcCmd *pCmd, int fWarnNoK )
{
    int c, fHasK = 0;
    if ( pLib == NULL )
        return 0;
    for ( c = 0; c < 2; c++ )
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
            if ( fWarnNoK &&
                 ( ( Msta_IsSet(v) && pInfo->NomVoltage >= 0.0 && v != pInfo->NomVoltage ) ||
                   ( Msta_IsSet(t) && pInfo->NomTemperature >= 0.0 && t != pInfo->NomTemperature ) ) )
                Msta_SdcNote( pCmd, "the library has no k_volt/k_temp factor; delay tables are "
                              "used as read (the requested voltage/temperature is recorded and "
                              "reported only)" );
            continue;
        }
        fHasK = 1;
        dV = ( Msta_IsSet(v) && pInfo->NomVoltage >= 0.0 ) ? v - pInfo->NomVoltage : 0.0;
        dT = ( Msta_IsSet(t) && pInfo->NomTemperature >= 0.0 ) ? t - pInfo->NomTemperature : 0.0;
        if ( pCond->KVolt >= 0.0 )    Derate += pCond->KVolt * dV;
        if ( pCond->KTemp >= 0.0 )    Derate += pCond->KTemp * dT;
        if ( Derate <= 0.0 ) Derate = 1.0;
        if ( fMax ) p->KFactorDerateLate = Derate;
        else        p->KFactorDerateEarly = Derate;
        if ( Derate != 1.0 )
            Msta_SdcNote( pCmd, "K-factor derate %.4f applied (%.3f V / %.1f C); K factors are "
                          "a linear approximation of the characterized tables",
                          Derate, Msta_IsSet(v) ? v : 0.0, Msta_IsSet(t) ? t : 0.0 );
    }
    return fHasK;
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

/* 库名是否是已读入的某个库。 */
static int Msta_SdcLibraryExists( MstaLib *pLib, MstaId nLibrary )
{
    int j;
    for ( j = 0; j < pLib->vLibs.nSize; j++ )
        if ( MstaLibInfoArrayAt(&pLib->vLibs,j)->Name == nLibrary )
            return 1;
    return 0;
}

/* set_operating_conditions [条件名] [-max 条件名] [-min 条件名] [-library 库]
       [-max_library 库] [-min_library 库] [-analysis_type single|bc_wc]
       [-voltage 电压] [-temperature 温度]
   选择 max/min 两个角的工作条件与对应的 Liberty 库。不写 -max/-min 时，
   位置参数里的条件名两个角共用。-object_list 不建模；
   -analysis_type on_chip_variation 不建模。 */
static const MstaSdcOpt s_vOperatingConditionsOpts[] = {
    { "-library",       MSTA_SDC_VALUE, 0               },
    { "-max_library",   MSTA_SDC_VALUE, 0               },
    { "-min_library",   MSTA_SDC_VALUE, 0               },
    { "-max",           MSTA_SDC_VALUE, 0               },
    { "-min",           MSTA_SDC_VALUE, 0               },
    { "-analysis_type", MSTA_SDC_VALUE, 0               },
    { "-voltage",       MSTA_SDC_VALUE, 0               },
    { "-temperature",   MSTA_SDC_VALUE, 0               },
    { "-object_list",   MSTA_SDC_OBJECTS, MSTA_SDC_IGNORE },
    { NULL,             MSTA_SDC_FLAG,  0               } };

static void Msta_SdcSetOperatingConditions( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    const char *pLibName    = Msta_SdcOptValue( pCmd, "-library" );
    const char *pMaxLibName = Msta_SdcOptValue( pCmd, "-max_library" );
    const char *pMinLibName = Msta_SdcOptValue( pCmd, "-min_library" );
    const char *pMax        = Msta_SdcOptValue( pCmd, "-max" );
    const char *pMin        = Msta_SdcOptValue( pCmd, "-min" );
    const char *pAnalysis   = Msta_SdcOptValue( pCmd, "-analysis_type" );
    const char *pVolt       = Msta_SdcOptValue( pCmd, "-voltage" );
    const char *pTemp       = Msta_SdcOptValue( pCmd, "-temperature" );
    int fMaxLibrary = pLibName != NULL || pMaxLibName != NULL;
    int fMinLibrary = pLibName != NULL || pMinLibName != NULL;
    MstaId nLibraryMax = pMaxLibName ? Msta_NameId(pMaxLibName) : pLibName ? Msta_NameId(pLibName) : MSTA_NO_ID;
    MstaId nLibraryMin = pMinLibName ? Msta_NameId(pMinLibName) : pLibName ? Msta_NameId(pLibName) : MSTA_NO_ID;
    MstaLibInfo *pMaxInfo = NULL, *pMinInfo = NULL;
    double Volt = 0.0, Temp = 0.0;

    if ( pAnalysis && strcasecmp(pAnalysis,"on_chip_variation") == 0 )
    { Msta_SdcReject( p, pCmd, "-analysis_type on_chip_variation is not modeled" ); return; }
    if ( pAnalysis && strcasecmp(pAnalysis,"single") != 0 && strcasecmp(pAnalysis,"bc_wc") != 0 )
    { Msta_SdcReject( p, pCmd, "unknown -analysis_type \"%s\"", pAnalysis ); return; }
    if ( pAnalysis && strcasecmp(pAnalysis,"single") == 0 &&
         ( pMax != NULL || pMin != NULL || pMaxLibName != NULL || pMinLibName != NULL ) )
    { Msta_SdcReject( p, pCmd, "-analysis_type single cannot use -max/-min or -max_library/-min_library" ); return; }
    if ( ( pVolt && !Msta_SdcGetNumber( p, pCmd, "-voltage", pVolt, MSTA_SDC_POSITIVE, &Volt ) ) ||
         ( pTemp && !Msta_SdcGetNumber( p, pCmd, "-temperature", pTemp, MSTA_SDC_ANY, &Temp ) ) )
        return;
    if ( fMaxLibrary && !Msta_SdcLibraryExists( pLib, nLibraryMax ) )
    { Msta_SdcReject( p, pCmd, "unknown library \"%s\"", Msta_NameStr(nLibraryMax) ); return; }
    if ( fMinLibrary && !Msta_SdcLibraryExists( pLib, nLibraryMin ) )
    { Msta_SdcReject( p, pCmd, "unknown library \"%s\"", Msta_NameStr(nLibraryMin) ); return; }
    if ( pMax == NULL && pMin == NULL && pCmd->nObjs == 1 )
        pMax = pMin = pCmd->ppObjs[0];
    else if ( pCmd->nObjs == 1 )
    { Msta_SdcReject( p, pCmd, "a condition name cannot be combined with -max/-min" ); return; }
    if ( pMax == NULL && pMin == NULL && pVolt == NULL && pTemp == NULL && !fMaxLibrary && !fMinLibrary )
    { Msta_SdcReject( p, pCmd, "needs a condition name, a library, or -voltage/-temperature" ); return; }
    if ( pMax != NULL && ( pMaxInfo = Msta_LibFindOpCond( pLib, pMax, nLibraryMax, NULL ) ) == NULL )
    { Msta_SdcReject( p, pCmd, "no library declares operating condition \"%s\"", pMax ); return; }
    if ( pMin != NULL && ( pMinInfo = Msta_LibFindOpCond( pLib, pMin, nLibraryMin, NULL ) ) == NULL )
    { Msta_SdcReject( p, pCmd, "no library declares operating condition \"%s\"", pMin ); return; }

    /* 给出的名字都有效，库与工艺角的改动一起生效。 */
    if ( fMaxLibrary ) p->OpCondLibraryMax = nLibraryMax;
    if ( fMinLibrary ) p->OpCondLibraryMin = nLibraryMin;
    if ( pMaxInfo != NULL )
    {
        p->OpCondMax = Msta_NameId(pMax);
        if ( !fMaxLibrary )
        {
            if ( Msta_SdcOpCondLibraryCount(pLib,pMax) > 1 )
                Msta_SdcNote( pCmd, "max corner \"%s\" is in multiple libraries; specify -max_library", pMax );
            p->OpCondLibraryMax = pMaxInfo->Name;
        }
    }
    else if ( fMaxLibrary && p->OpCondMax != MSTA_NO_ID &&
              Msta_LibFindOpCond(pLib,Msta_NameStr(p->OpCondMax),nLibraryMax,NULL) == NULL )
        p->OpCondMax = MSTA_NO_ID;
    if ( pMinInfo != NULL )
    {
        p->OpCondMin = Msta_NameId(pMin);
        if ( !fMinLibrary )
        {
            if ( Msta_SdcOpCondLibraryCount(pLib,pMin) > 1 )
                Msta_SdcNote( pCmd, "min corner \"%s\" is in multiple libraries; specify -min_library", pMin );
            p->OpCondLibraryMin = pMinInfo->Name;
        }
    }
    else if ( fMinLibrary && p->OpCondMin != MSTA_NO_ID &&
              Msta_LibFindOpCond(pLib,Msta_NameStr(p->OpCondMin),nLibraryMin,NULL) == NULL )
        p->OpCondMin = MSTA_NO_ID;

    /* 只给电压/温度的写法，对带 K 因子的库同样有效。 */
    if ( pVolt != NULL ) p->VoltageMax = p->VoltageMin = Volt;
    if ( pTemp != NULL ) p->TempMax = p->TempMin = Temp;
    Msta_SdcUpdateKFactor( p, pLib, pCmd, 1 );
}

/* set_voltage 最高电压 [-min 最低电压] [-object_list 电源网络]
   记录设计的工作电压。分对象的电压要电源域模型，不建模。 */
static const MstaSdcOpt s_vVoltageOpts[] = {
    { "-min",         MSTA_SDC_VALUE, 0               },
    { "-object_list", MSTA_SDC_OBJECTS, MSTA_SDC_IGNORE },
    { NULL,           MSTA_SDC_FLAG,  0               } };

static void Msta_SdcSetVoltage( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    const char *pMin = Msta_SdcOptValue( pCmd, "-min" );
    double Max, Min;
    if ( pCmd->nObjs > 0 )
    { Msta_SdcReject( p, pCmd, "per-object voltage is not modeled" ); return; }
    if ( !Msta_SdcGetNumber( p, pCmd, "voltage", pCmd->pValue, MSTA_SDC_POSITIVE, &Max ) ||
         ( pMin && !Msta_SdcGetNumber( p, pCmd, "-min", pMin, MSTA_SDC_POSITIVE, &Min ) ) )
        return;
    p->VoltageMax = Max;
    p->VoltageMin = pMin ? Min : Max;
    /* 延迟表只能靠库里的 k 因子随电压缩放；没有 k 因子时 set_voltage 只被记录、
       不改变延迟，电压偏离标称值时要告警，免得用户以为结果已按新电压算过。
       有 k 因子时缩放系数由 Msta_SdcUpdateKFactor 报出。 */
    if ( !Msta_SdcUpdateKFactor( p, pLib, pCmd, 0 ) && pLib != NULL && pLib->vLibs.nSize > 0 )
    {
        double Nominal = MstaLibInfoArrayAt(&pLib->vLibs,0)->NomVoltage;
        if ( Nominal >= 0.0 && fabs(p->VoltageMax - Nominal) > 1e-6 )
            Msta_SdcNote( pCmd, "%.3f V is recorded, but the library has no k_volt factor; "
                          "delay tables are used as read (nominal %.3f V)", p->VoltageMax, Nominal );
    }
}

/* 把时间/电容单位文本换算成内部比例（ps / fF）；不认识时返回 0。 */
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

/* set_units [-time 单位] [-capacitance 单位]
   把单位用到后面命令的数值上。其他单位（-resistance 等）不建模。 */
static const MstaSdcOpt s_vUnitsOpts[] = {
    { "-time",        MSTA_SDC_VALUE, 0               },
    { "-capacitance", MSTA_SDC_VALUE, 0               },
    { "-resistance",  MSTA_SDC_VALUE, MSTA_SDC_IGNORE },
    { "-voltage",     MSTA_SDC_VALUE, MSTA_SDC_IGNORE },
    { "-current",     MSTA_SDC_VALUE, MSTA_SDC_IGNORE },
    { "-power",       MSTA_SDC_VALUE, MSTA_SDC_IGNORE },
    { NULL,           MSTA_SDC_FLAG,  0               } };

static void Msta_SdcSetUnits( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    const char *pTime = Msta_SdcOptValue( pCmd, "-time" );
    const char *pCap  = Msta_SdcOptValue( pCmd, "-capacitance" );
    double TimeScale = pTime ? Msta_SdcUnitScale( pTime, 1 ) : 0.0;
    double CapScale  = pCap  ? Msta_SdcUnitScale( pCap, 0 )  : 0.0;
    if ( pTime && TimeScale <= 0.0 )
    { Msta_SdcReject( p, pCmd, "unsupported time unit \"%s\"", pTime ); return; }
    if ( pCap && CapScale <= 0.0 )
    { Msta_SdcReject( p, pCmd, "unsupported capacitance unit \"%s\"", pCap ); return; }
    if ( pTime ) p->TimeScalePs = TimeScale;
    if ( pCap )  p->CapScaleFf  = CapScale;
}

/* =====================================================================
   读入与分发
   ===================================================================== */

static int Msta_SdcRunOne( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, int argc, char **argv,
                           const int *pArg );

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
   选项表与处理函数见"各条命令"一节。 */
static const MstaSdcCmdDef s_vSdcCommands[] = {
    { "create_clock",             s_vCreateClockOpts,         MSTA_SDC_NO_VALUE, 0, -1, Msta_SdcCreateClock            },
    { "create_generated_clock",   s_vGeneratedClockOpts,      MSTA_SDC_NO_VALUE, 1,  1, Msta_SdcCreateGeneratedClock   },
    { "set_clock_uncertainty",    s_vClockUncertaintyOpts,    MSTA_SDC_NUMBER,   0, -1, Msta_SdcSetClockUncertainty    },
    { "set_clock_latency",        s_vClockLatencyOpts,        MSTA_SDC_NUMBER,   0, -1, Msta_SdcSetClockLatency        },
    { "set_propagated_clock",     s_vNoOpts,                  MSTA_SDC_NO_VALUE, 1, -1, Msta_SdcSetPropagatedClock     },
    { "set_clock_transition",     s_vMinMaxRiseFallOpts,      MSTA_SDC_NUMBER,   1, -1, Msta_SdcSetClockTransition     },
    { "set_clock_sense",          s_vClockSenseOpts,          MSTA_SDC_NO_VALUE, 1, -1, Msta_SdcSetClockSense          },
    { "set_clock_groups",         s_vClockGroupsOpts,         MSTA_SDC_NO_VALUE, 0,  0, Msta_SdcSetClockGroups         },
    { "set_input_delay",          s_vPortDelayOpts,           MSTA_SDC_NUMBER,   1, -1, Msta_SdcSetInputDelay          },
    { "set_output_delay",         s_vPortDelayOpts,           MSTA_SDC_NUMBER,   1, -1, Msta_SdcSetOutputDelay         },
    { "set_load",                 s_vLoadOpts,                MSTA_SDC_NUMBER,   1, -1, Msta_SdcSetLoad                },
    { "set_input_transition",     s_vInputTransitionOpts,     MSTA_SDC_NUMBER,   1, -1, Msta_SdcSetInputTransition     },
    { "set_driving_cell",         s_vDrivingCellOpts,         MSTA_SDC_NO_VALUE, 1, -1, Msta_SdcSetDrivingCell         },
    { "set_ideal_network",        s_vIdealNetworkOpts,        MSTA_SDC_NO_VALUE, 1, -1, Msta_SdcSetIdealNetwork        },
    { "set_ideal_latency",        s_vMinMaxRiseFallOpts,      MSTA_SDC_NUMBER,   1, -1, Msta_SdcSetIdealLatency        },
    { "set_ideal_transition",     s_vMinMaxRiseFallOpts,      MSTA_SDC_NUMBER,   1, -1, Msta_SdcSetIdealTransition     },
    { "set_case_analysis",        s_vNoOpts,                  MSTA_SDC_WORD,     1, -1, Msta_SdcSetCaseAnalysis        },
    { "set_logic_zero",           s_vNoOpts,                  MSTA_SDC_NO_VALUE, 1, -1, Msta_SdcSetLogicZero           },
    { "set_logic_one",            s_vNoOpts,                  MSTA_SDC_NO_VALUE, 1, -1, Msta_SdcSetLogicOne            },
    { "set_logic_dc",             s_vNoOpts,                  MSTA_SDC_NO_VALUE, 1, -1, Msta_SdcSetLogicDc             },
    { "set_disable_timing",       s_vDisableTimingOpts,       MSTA_SDC_NO_VALUE, 1, -1, Msta_SdcSetDisableTiming       },
    { "set_false_path",           s_vPathExceptionOpts,       MSTA_SDC_NO_VALUE, 0,  0, Msta_SdcSetFalsePath           },
    { "set_multicycle_path",      s_vPathExceptionOpts,       MSTA_SDC_NUMBER,   0,  0, Msta_SdcSetMulticyclePath      },
    { "set_max_delay",            s_vPathDelayOpts,           MSTA_SDC_NUMBER,   0,  0, Msta_SdcSetMaxDelay            },
    { "set_min_delay",            s_vPathDelayOpts,           MSTA_SDC_NUMBER,   0,  0, Msta_SdcSetMinDelay            },
    { "group_path",               s_vGroupPathOpts,           MSTA_SDC_NO_VALUE, 0,  0, Msta_SdcSetGroupPath           },
    { "set_data_check",           s_vDataCheckOpts,           MSTA_SDC_NUMBER,   0,  0, Msta_SdcSetDataCheck           },
    { "set_clock_gating_check",   s_vClockGatingCheckOpts,    MSTA_SDC_NO_VALUE, 0, -1, Msta_SdcSetClockGatingCheck    },
    { "set_max_time_borrow",      s_vNoOpts,                  MSTA_SDC_NUMBER,   0, -1, Msta_SdcSetMaxTimeBorrow       },
    { "set_timing_derate",        s_vTimingDerateOpts,        MSTA_SDC_NUMBER,   0, -1, Msta_SdcSetTimingDerate        },
    { "set_max_transition",       s_vDrcLimitOpts,            MSTA_SDC_NUMBER,   1, -1, Msta_SdcSetMaxTransition       },
    { "set_max_fanout",           s_vDrcLimitOpts,            MSTA_SDC_NUMBER,   1, -1, Msta_SdcSetMaxFanout           },
    { "set_max_capacitance",      s_vDrcLimitOpts,            MSTA_SDC_NUMBER,   1, -1, Msta_SdcSetMaxCapacitance      },
    { "set_min_capacitance",      s_vDrcLimitOpts,            MSTA_SDC_NUMBER,   1, -1, Msta_SdcSetMinCapacitance      },
    { "set_max_area",             s_vNoOpts,                  MSTA_SDC_NUMBER,   0,  0, Msta_SdcSetMaxArea             },
    { "set_operating_conditions", s_vOperatingConditionsOpts, MSTA_SDC_NO_VALUE, 0,  1, Msta_SdcSetOperatingConditions },
    { "set_voltage",              s_vVoltageOpts,             MSTA_SDC_NUMBER,   0, -1, Msta_SdcSetVoltage             },
    { "set_units",                s_vUnitsOpts,               MSTA_SDC_NO_VALUE, 0,  0, Msta_SdcSetUnits               },
    { NULL,                       NULL,                       MSTA_SDC_NO_VALUE, 0,  0, NULL                           } };

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

int Msta_SdcDataCheckCount( MstaSdc *p )
{
    return p->vDataChecks.nSize;
}

MstaDataCheck *Msta_SdcDataCheckByIndex( MstaSdc *p, int i )
{
    return ( i < 0 || i >= p->vDataChecks.nSize ) ? NULL
                                                  : MstaDataCheckArrayAt(&p->vDataChecks,i);
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
