/**CFile***************************************************************

  FileName    [msta_sdc_parse.c]

  Synopsis    [SDC 命令的通用解析：数值与对象的小工具、选项表解析器、公共子语法。]

  各条命令的处理函数（msta_sdc_clock.c / msta_sdc_io.c / msta_sdc_except.c /
  msta_sdc_env.c）只做语义，取参数、查对象、报错都用这里的函数；
  msta_sdc.c 的 Msta_SdcRunOne 先调 Msta_SdcParseCmd 拆参数再交给处理函数。
  这里用到的临时内存与集合类型来自 msta_sdc.c（Msta_SdcArena / Msta_SdcKindOf）。
  类型与函数声明在 msta_sdc_int.h。

***********************************************************************/

#include <ctype.h>
#include <stdarg.h>
#include "msta_sdc_int.h"

/* =====================================================================
   数值与对象的小工具
   ===================================================================== */

/* Tcl 的 * / ? 通配；总线名里的方括号按普通字符处理。 */
int Msta_SdcGlobMatch( const char *pPattern, const char *pText )
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
double Msta_SdcToPs( MstaSdc *p, const char *pText )
{
    return atof( pText ) * p->TimeScalePs;
}

/* 端口名 / "实例路径/引脚名" / 网络名 -> 一组全局网络号。
   总线端口（如 input [8:0] dma_ack_i）在展平后只有 dma_ack_i[0..8] 这些名字，
   所以这里允许只写基名，自动展开成它的所有位。返回找到的个数。 */
int Msta_SdcResolveNetsInto( MstaDesign *pDes, const char *pTarget, MstaSdcIntArray *vOut )
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
            Msta_WarnOnce("sdc：模式 \"%s\" 没有匹配到任何网络",pTarget);
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
                Msta_WarnOnce( "sdc：对象 \"%s\" 不是本设计里的网络", pTarget );
            return 0;
        }
        *pSlash = 0;
        nInst = Msta_DesignFindInstByName( pDes, pBuf );
        if ( nInst < 0 )
        {
            if ( !Msta_SdcIsQuiet(pTarget) )
                Msta_WarnOnce( "sdc：对象 \"%s\" 既不是网络也不是实例引脚", pTarget );
            return 0;
        }
        nNet = Msta_DesignInstPinNet( pDes, nInst, pSlash + 1 );
        if ( nNet < 0 )
        {
            Msta_WarnOnce( "sdc：实例 \"%s\" 没有引脚 \"%s\"", pBuf, pSlash + 1 );
            return 0;
        }
        *MstaSdcIntArrayAppend( vOut ) = nNet;
    }
    return 1;
}

int Msta_SdcResolveNets( MstaDesign *pDes, const char *pTarget, int **ppNets )
{
    MstaSdcIntArray vNets;
    MstaSdcIntArrayInit( &vNets );
    Msta_SdcResolveNetsInto( pDes, pTarget, &vNets );
    *ppNets = (int *)Msta_SdcArenaKeep( vNets.pData );
    return vNets.nSize;
}

/* =====================================================================
   选项表与通用解析器

   每条命令的语法写在分发表 s_vSdcCommands（msta_sdc.c）的一行里（MstaSdcCmdDef）：
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

/* 作废整条约束：告警 "<命令>：<原因>；约束作废"，并计入忽略数。
   命令的所有语法和取值错误都走这里，措辞因此一致。 */
void Msta_SdcReject( MstaSdc *p, const MstaSdcCmd *pCmd, const char *pFormat, ... )
{
    char sReason[800];
    va_list Args;
    va_start( Args, pFormat );
    vsnprintf( sReason, sizeof(sReason), pFormat, Args );
    va_end( Args );
    Msta_WarnOnce( "%s：%s；约束作废", pCmd->pName, sReason );
    p->nCommandsIgnored++;
}

/* 只提醒、约束照常生效："<命令>：<说明>"。 */
void Msta_SdcNote( const MstaSdcCmd *pCmd, const char *pFormat, ... )
{
    char sText[800];
    va_list Args;
    va_start( Args, pFormat );
    vsnprintf( sText, sizeof(sText), pFormat, Args );
    va_end( Args );
    Msta_WarnOnce( "%s：%s", pCmd->pName, sText );
}

/* 在本条命令的临时内存上分配 n 个清零的元素（多给一个，n 为 0 也能用）。 */
void *Msta_SdcArenaZeros( int n, size_t nSize )
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
        { Msta_SdcReject( p, pCmd, "缺少数值" ); return 0; }
        if ( pDef->Value == MSTA_SDC_NUMBER && !Msta_SdcIsNumber( ppPos[0] ) )
        { Msta_SdcReject( p, pCmd, "值 \"%s\" 不是数", ppPos[0] ); return 0; }
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
            Msta_SdcReject( p, pCmd, "\"%s %s\" 不是 SDC 1.8 语法（%s 后面不带值）",
                            pPrev, ppPos[i], pPrev );
        else
            Msta_SdcReject( p, pCmd, "多出了数值 \"%s\"", ppPos[i] );
        return 0;
    }
    pCmd->ppObjs = ppPos + iFirst;
    pCmd->nObjs  = nPos - iFirst;
    if ( pCmd->nObjs < pDef->nMinObjs )
    { Msta_SdcReject( p, pCmd, "缺少对象列表" ); return 0; }
    if ( pDef->nMaxObjs >= 0 && pCmd->nObjs > pDef->nMaxObjs )
    {
        Msta_SdcReject( p, pCmd, "多出了参数 \"%s\"", pCmd->ppObjs[pDef->nMaxObjs] );
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
int Msta_SdcParseCmd( MstaSdc *p, const MstaSdcCmdDef *pDef, int argc, char **argv,
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
        { Msta_SdcReject( p, pCmd, "选项 \"%s\" 未建模", argv[i] ); return 0; }
        if ( pCmd->pAt[k] > 0 && !( pOpt->Attr & MSTA_SDC_REPEAT ) )
        { Msta_SdcReject( p, pCmd, "选项 \"%s\" 写了不止一次", pOpt->pName ); return 0; }
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
            { Msta_SdcReject( p, pCmd, "选项 \"%s\" 后面没有对象", argv[i] ); return 0; }
        }
        else
        {
            /* VALUE 和 OBJECTS 取紧跟的下一个参数（它展开出的所有词）。 */
            if ( i + 1 >= argc || Msta_SdcLooksLikeOption( argv[i+1] ) )
            { Msta_SdcReject( p, pCmd, "选项 \"%s\" 缺少值", argv[i] ); return 0; }
            for ( j = i + 1; j < argc && pArg[j] == pArg[i+1]; j++ )
                ;
            ppWords = argv + i + 1;
            nWords  = j - i - 1;
            if ( pOpt->Kind == MSTA_SDC_VALUE && nWords > 1 )
            { Msta_SdcReject( p, pCmd, "选项 \"%s\" 只能带一个值", argv[i] ); return 0; }
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
            Msta_SdcNote( pCmd, "选项 \"%s\" 未建模，已忽略", argv[i] );
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
int Msta_SdcHasFlag( const MstaSdcCmd *pCmd, const char *pName )
{
    return pCmd->pAt[ Msta_SdcOptIndex( pCmd, pName ) ] > 0;
}

/* MSTA_SDC_VALUE 选项的值，没写返回 NULL。 */
const char *Msta_SdcOptValue( const MstaSdcCmd *pCmd, const char *pName )
{
    return pCmd->ppValue[ Msta_SdcOptIndex( pCmd, pName ) ];
}

/* 带 MSTA_SDC_RF 的选项是用哪种写法给的：-rise_xxx 返回 'r'，-fall_xxx 返回 'f'，否则 0。 */
char Msta_SdcOptEdge( const MstaSdcCmd *pCmd, const char *pName )
{
    return pCmd->pEdge[ Msta_SdcOptIndex( pCmd, pName ) ];
}

/* 只能写一次的对象类选项（OBJECTS / LIST）：对象写进 *pppWords，返回个数；没写返回 0。 */
int Msta_SdcOptList( const MstaSdcCmd *pCmd, const char *pName, char ***pppWords )
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
int Msta_SdcRequire( MstaSdc *p, const MstaSdcCmd *pCmd, const char *pName )
{
    if ( Msta_SdcHasFlag( pCmd, pName ) )
        return 1;
    Msta_SdcReject( p, pCmd, "缺少必需的选项 \"%s\"", pName );
    return 0;
}

/* ---------------- 公共子语法：几条命令共用的写法 ---------------- */

/* 没有选项的命令共用的空选项表。 */
const MstaSdcOpt Msta_SdcNoOpts[] = {
    { NULL, MSTA_SDC_FLAG, 0 } };

/* 只有 -min/-max/-rise/-fall 四个开关的命令共用的选项表
   （set_clock_transition、set_ideal_latency、set_ideal_transition）。 */
const MstaSdcOpt Msta_SdcMinMaxRiseFallOpts[] = {
    { "-min",  MSTA_SDC_FLAG, 0 },
    { "-max",  MSTA_SDC_FLAG, 0 },
    { "-rise", MSTA_SDC_FLAG, 0 },
    { "-fall", MSTA_SDC_FLAG, 0 },
    { NULL,    MSTA_SDC_FLAG, 0 } };

/* 把 pText 读成数值并检查范围；pWhat 是告警里对它的称呼（如 "-weight"、"值"）。
   不合格时告警作废并返回 0。 */
int Msta_SdcGetNumber( MstaSdc *p, const MstaSdcCmd *pCmd, const char *pWhat,
                       const char *pText, int Range, double *pValue )
{
    /* 选项名（如 -period）后面空一格再接中文，中文称呼（如 "值"）直接接。 */
    const char *pSep = ( (unsigned char)pWhat[0] < 0x80 ) ? " " : "";
    if ( !Msta_SdcIsNumber( pText ) )
    { Msta_SdcReject( p, pCmd, "%s \"%s\" 不是数", pWhat, pText ); return 0; }
    *pValue = atof( pText );
    if ( Range == MSTA_SDC_POSITIVE && *pValue <= 0.0 )
    { Msta_SdcReject( p, pCmd, "%s%s必须大于 0（给的是 %s）", pWhat, pSep, pText ); return 0; }
    if ( Range == MSTA_SDC_NONNEG && *pValue < 0.0 )
    { Msta_SdcReject( p, pCmd, "%s%s不能为负（给的是 %s）", pWhat, pSep, pText ); return 0; }
    return 1;
}

/* -min/-max 与 -rise/-fall 决定一个值落在哪几个 [角][边沿] 格子里。
   角 0 = min、1 = max；边沿 0 = fall、1 = rise（与 MstaClock::Slew 等数组的下标一致）。
   一对限定都没写（或都写了）等于两个都选：只写 -max 选中 max 的 rise 和 fall
   两格，什么都不写四格全选。 */
void Msta_SdcMinMaxRiseFall( int fMin, int fMax, int fRise, int fFall, int Sel[2][2] )
{
    int m, e;
    for ( m = 0; m < 2; m++ )
        for ( e = 0; e < 2; e++ )
            Sel[m][e] = ( m ? ( fMax || !fMin ) : ( fMin || !fMax ) ) &&
                        ( e ? ( fRise || !fFall ) : ( fFall || !fRise ) );
}

/* 从命令里读 -min/-max/-rise/-fall 四个开关，算出掩码。 */
void Msta_SdcCmdMinMaxRiseFall( const MstaSdcCmd *pCmd, int Sel[2][2] )
{
    Msta_SdcMinMaxRiseFall( Msta_SdcHasFlag( pCmd, "-min" ), Msta_SdcHasFlag( pCmd, "-max" ),
                            Msta_SdcHasFlag( pCmd, "-rise" ), Msta_SdcHasFlag( pCmd, "-fall" ), Sel );
}

/* 把 Value 写进 Target 里被 Sel 选中的格子。 */
void Msta_SdcStoreMinMaxRiseFall( double Target[2][2], int Sel[2][2], double Value )
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
void Msta_SdcStoreMinMaxRiseFall4( int Sel[2][2], double Value,
                                   double *pMaxRise, double *pMaxFall,
                                   double *pMinRise, double *pMinFall )
{
    if ( Sel[1][1] ) *pMaxRise = Value;
    if ( Sel[1][0] ) *pMaxFall = Value;
    if ( Sel[0][1] ) *pMinRise = Value;
    if ( Sel[0][0] ) *pMinFall = Value;
}

/* 只写 -rise 或只写 -fall 时返回 'r' / 'f'，否则（都没写或都写了）返回 0。 */
char Msta_SdcCmdEdge( const MstaSdcCmd *pCmd )
{
    int fRise = Msta_SdcHasFlag( pCmd, "-rise" ), fFall = Msta_SdcHasFlag( pCmd, "-fall" );
    return ( fRise && !fFall ) ? 'r' : ( fFall && !fRise ) ? 'f' : 0;
}

/* 对象列表 -> 网络。每个对象可以是端口、引脚、网络名或总线基名；找不到的对象
   当场告警并跳过。一个网络都没找到时告警作废并返回 -1。数组挂在临时内存上。 */
int Msta_SdcObjectNets( MstaSdc *p, MstaDesign *pDes, const MstaSdcCmd *pCmd,
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
        Msta_SdcReject( p, pCmd, "一个对象都没有找到" );
        return -1;
    }
    return vNets.nSize;
}

/* 对象列表 -> 时钟（vClocks 里的下标）。不认识的时钟当场告警并跳过；
   一个都不认识时告警作废并返回 -1。 */
int Msta_SdcObjectClocks( MstaSdc *p, const MstaSdcCmd *pCmd,
                          char **ppNames, int nNames, int **ppClocks )
{
    int *pClocks = (int *)Msta_SdcArenaZeros( nNames, sizeof(int) );
    int i, n = 0;
    for ( i = 0; i < nNames; i++ )
    {
        int j = Msta_SdcClockIndexOf( p, Msta_NameId( ppNames[i] ) );
        if ( j < 0 )
            Msta_SdcNote( pCmd, "找不到时钟 \"%s\"", ppNames[i] );
        else
            pClocks[n++] = j;
    }
    *ppClocks = pClocks;
    if ( n == 0 )
    {
        Msta_SdcReject( p, pCmd, "一个时钟都没有找到" );
        return -1;
    }
    return n;
}

/* 对象列表 -> 实例。fCellNames=1 时找不到的名字再当库单元名，取用到这个单元的
   所有实例。找不到的对象当场告警并跳过；一个实例都没得到时告警作废并返回 -1。 */
int Msta_SdcObjectInsts( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, const MstaSdcCmd *pCmd,
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
            Msta_SdcNote( pCmd, fCellNames ? "找不到实例或单元 \"%s\"" : "找不到实例 \"%s\"",
                          ppNames[i] );
    }
    *ppInsts = (int *)Msta_SdcArenaKeep( vInsts.pData );
    if ( vInsts.nSize == 0 )
    {
        Msta_SdcReject( p, pCmd, "一个对象都没有找到" );
        return -1;
    }
    return vInsts.nSize;
}
