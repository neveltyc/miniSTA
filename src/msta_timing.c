/**CFile***************************************************************

  FileName    [msta_timing.c]

  Synopsis    [时序引擎总入口：生命周期管理、Analyze 调度、结果汇总与查询接口。]

  各算法步骤分布在同目录的独立模块中，按分析顺序阅读：
    1. msta_graph.c : 网表与库工艺角映射、负载、理想网络、拓扑序与逻辑级数
    2. msta_clock.c : 时钟树标记、极性推导与时钟到达/摆率传播
    3. msta_prop.c  : 数据 AT 前向传播与教科书式 RAT 后向传播
    4. msta_cppr.c  : AOCV 结构深度、CPPR 公共路径补偿与起点分类
    5. msta_check.c : 端点收集、捕获沿对齐、路径例外与次优搜索、数据/门控检查
    6. msta_drc.c   : max_transition / max_capacitance / min_capacitance / max_fanout

***********************************************************************/

#include "msta_timing_int.h"
#include <time.h>

static void AllocCorner( MstaCorner *pC, int n, int fMax )
{
    int i, n4 = n > 0 ? n : 1;
    double UnsetReq = fMax ? MSTA_NO_TIME : -MSTA_NO_TIME;
    pC->pArr          = (double *)calloc( (size_t)n4, sizeof(double) );
    pC->pArrRise      = (double *)calloc( (size_t)n4, sizeof(double) );
    pC->pArrFall      = (double *)calloc( (size_t)n4, sizeof(double) );
    pC->pReq          = (double *)malloc( (size_t)n4 * sizeof(double) );
    pC->pReqRise      = (double *)malloc( (size_t)n4 * sizeof(double) );
    pC->pReqFall      = (double *)malloc( (size_t)n4 * sizeof(double) );
    pC->pAuxReqRise   = (double *)malloc( (size_t)n4 * sizeof(double) );
    pC->pAuxReqFall   = (double *)malloc( (size_t)n4 * sizeof(double) );
    pC->pSlew         = (double *)calloc( (size_t)n4, sizeof(double) );
    pC->pSlewRise     = (double *)calloc( (size_t)n4, sizeof(double) );
    pC->pSlewFall     = (double *)calloc( (size_t)n4, sizeof(double) );
    pC->pnLaunchClock = (int *)malloc( (size_t)n4 * sizeof(int) );
    pC->pdLaunchEdge  = (double *)calloc( (size_t)n4, sizeof(double) );
    pC->pfLaunchRises = (char *)calloc( (size_t)n4, sizeof(char) );
    pC->pPrevNet      = (int *)malloc( (size_t)n4 * sizeof(int) );
    pC->pPrevRise     = (MstaPrev *)malloc( (size_t)n4 * sizeof(MstaPrev) );
    pC->pPrevFall     = (MstaPrev *)malloc( (size_t)n4 * sizeof(MstaPrev) );
    assert( pC->pReq && pC->pReqRise && pC->pReqFall && pC->pAuxReqRise && pC->pAuxReqFall );
    for ( i = 0; i < n4; i++ )
    {
        pC->pReq[i] = pC->pReqRise[i] = pC->pReqFall[i] = UnsetReq;
        pC->pAuxReqRise[i] = pC->pAuxReqFall[i] = UnsetReq;
    }
}

static void FreeCorner( MstaCorner *pC )
{
    free( pC->pArr );
    free( pC->pArrRise );
    free( pC->pArrFall );
    free( pC->pReq );
    free( pC->pReqRise );
    free( pC->pReqFall );
    free( pC->pAuxReqRise );
    free( pC->pAuxReqFall );
    free( pC->pSlew );
    free( pC->pSlewRise );
    free( pC->pSlewFall );
    free( pC->pnLaunchClock );
    free( pC->pdLaunchEdge );
    free( pC->pfLaunchRises );
    free( pC->pPrevNet );
    free( pC->pPrevRise );
    free( pC->pPrevFall );
    memset( pC, 0, sizeof(MstaCorner) );
}

static void AllocNetArrays( MstaTiming *p )
{
    int n = p->pDes->vNets.nSize;
    int nClocks = Msta_SdcClockCount( p->pSdc );
    int n4 = n > 0 ? n : 1;
    p->pfClockNet        = (char *)calloc( (size_t)n4, sizeof(char) );
    p->pfIdealNet        = (char *)calloc( (size_t)n4, sizeof(char) );
    p->pClockArr         = (MstaClockArr *)calloc( (size_t)n4 * (size_t)(nClocks > 0 ? nClocks : 1),
                                                   sizeof(MstaClockArr) );
    p->pfClockOfNet      = (char *)calloc( (size_t)n4 * (size_t)(nClocks > 0 ? nClocks : 1),
                                           sizeof(char) );
    AllocCorner( &p->CornerMax, n, 1 );
    AllocCorner( &p->CornerMin, n, 0 );
    p->nPathExclude      = 0;
    p->nClockPairs       = 0;
    p->pTopoOrder        = (int *)malloc( (size_t)n4 * sizeof(int) );
    p->pLevel            = (int *)calloc( (size_t)n4, sizeof(int) );
    p->pState            = (int *)calloc( (size_t)n4, sizeof(int) );
    p->pStartClass       = (int *)malloc( (size_t)n4 * sizeof(int) );
    p->pFirstCheck       = (int *)malloc( (size_t)n4 * sizeof(int) );
    assert( p->pfClockNet && p->pClockArr && p->pfClockOfNet &&
            p->pTopoOrder && p->pLevel && p->pFirstCheck );
    for ( int k = 0; k < n4; k++ )
        p->pFirstCheck[k] = -1;
}

static void FreeNetArrays( MstaTiming *p )
{
    int i;
    free( p->pfClockNet );         p->pfClockNet = NULL;
    free( p->pfIdealNet );         p->pfIdealNet = NULL;
    free( p->pClockArr );          p->pClockArr = NULL;
    free( p->pfClockOfNet );       p->pfClockOfNet = NULL;
    FreeCorner( &p->CornerMax );
    FreeCorner( &p->CornerMin );
    free( p->pTopoOrder );         p->pTopoOrder = NULL;
    free( p->pLevel );             p->pLevel = NULL;
    free( p->pState );             p->pState = NULL;
    free( p->pStartClass );        p->pStartClass = NULL;
    free( p->pFirstCheck );        p->pFirstCheck = NULL;
    free( p->pAocvDataForward );   p->pAocvDataForward = NULL;
    free( p->pAocvDataBackward );  p->pAocvDataBackward = NULL;
    free( p->pAocvClockForward );  p->pAocvClockForward = NULL;
    free( p->pAocvClockBackward ); p->pAocvClockBackward = NULL;
    for ( i = 0; i < MSTA_CPPR_PATH_COUNT; i++ )
        MstaClockPathFree( &p->vCpprPaths[i] );
}

static void FreeChecks( MstaTiming *p )
{
    int i;
    for ( i = 0; i < p->vChecks.nSize; i++ )
        Msta_TiFreeCheckPaths( MstaCheckArrayAt(&p->vChecks, i) );
    MstaCheckArrayFree( &p->vChecks );
    free( p->pNextCheck );
    p->pNextCheck = NULL;
}

static void BuildCheckIndex( MstaTiming *p )
{
    int nNets = p->pDes->vNets.nSize, i;
    if ( p->pFirstCheck == NULL )
    {
        p->pFirstCheck = (int *)malloc( (size_t)(nNets > 0 ? nNets : 1) * sizeof(int) );
        assert( p->pFirstCheck );
    }
    for ( i = 0; i < nNets; i++ )
        p->pFirstCheck[i] = -1;
    free( p->pNextCheck );
    p->pNextCheck = (int *)malloc( (size_t)(p->vChecks.nSize > 0 ? p->vChecks.nSize : 1) * sizeof(int) );
    assert( p->pNextCheck );
    for ( i = p->vChecks.nSize - 1; i >= 0; i-- )
    {
        int nEnd = MstaCheckArrayAt( &p->vChecks, i )->nEndNet;
        if ( nEnd >= 0 && nEnd < nNets )
        {
            p->pNextCheck[i] = p->pFirstCheck[nEnd];
            p->pFirstCheck[nEnd] = i;
        }
        else
            p->pNextCheck[i] = -1;
    }
}

MstaTiming *Msta_TimingStart( MstaDesign *pDes, MstaLib *pLib, MstaSdc *pSdc )
{
    MstaTiming *p = (MstaTiming *)calloc( 1, sizeof(MstaTiming) );
    assert( p );
    p->pDes = pDes;
    p->pLib = pLib;
    p->pSdc = pSdc;
    Msta_PluginSetInitDefaults( &p->Plugins );
    p->LaunchClockTag = p->CaptureClockTag = -1;
    p->fAnalyzed = 0;
    MstaCheckArrayInit( &p->vChecks );
    return p;
}

void Msta_TimingSetPlugins( MstaTiming *p, const MstaPluginSet *pSet )
{
    MstaPluginSet Defaults;
    if ( p == NULL )
        return;
    Msta_PluginSetInitDefaults( &Defaults );
    if ( pSet == NULL )
    {
        p->Plugins = Defaults;
        p->fAnalyzed = 0;
        return;
    }
    p->Plugins.pDelay = ( pSet->pDelay && pSet->pDelay->AbiVersion == MSTA_PLUGIN_ABI_VERSION )
                      ? pSet->pDelay : Defaults.pDelay;
    p->Plugins.pWire = ( pSet->pWire && pSet->pWire->AbiVersion == MSTA_PLUGIN_ABI_VERSION )
                     ? pSet->pWire : Defaults.pWire;
    p->Plugins.pVariation = ( pSet->pVariation && pSet->pVariation->AbiVersion == MSTA_PLUGIN_ABI_VERSION )
                          ? pSet->pVariation : Defaults.pVariation;
    p->Plugins.pRefiner = ( pSet->pRefiner && pSet->pRefiner->AbiVersion == MSTA_PLUGIN_ABI_VERSION )
                        ? pSet->pRefiner : Defaults.pRefiner;
    p->fAnalyzed = 0;
}

void Msta_TimingFree( MstaTiming *p )
{
    if ( p == NULL )
        return;
    FreeNetArrays( p );
    FreeChecks( p );
    free( p->pPathObjects );
    free( p );
}

static void AddCheckSlack( double Slack, double *pWorst, double *pTotal, int *pnViolations )
{
    if ( Slack < *pWorst )
        *pWorst = Slack;
    if ( Slack < 0.0 )
    {
        (*pnViolations)++;
        *pTotal += Slack;
    }
}

static void RefreshTimingSummary( MstaTiming *p )
{
    int i;
    p->WorstSetupSlack = p->WorstHoldSlack = MSTA_NO_TIME;
    p->TotalSetupSlack = p->TotalHoldSlack = 0.0;
    p->nSetupViolations = p->nHoldViolations = 0;
    p->nUnconstrainedEnds = p->nExcludedEnds = 0;
    if ( Msta_IsSet(p->WorstDataCheckSetupSlack) &&
         p->WorstDataCheckSetupSlack < p->WorstSetupSlack )
        AddCheckSlack( p->WorstDataCheckSetupSlack, &p->WorstSetupSlack,
                       &p->TotalSetupSlack, &p->nSetupViolations );
    if ( Msta_IsSet(p->WorstClkGatingSetupSlack) &&
         p->WorstClkGatingSetupSlack < p->WorstSetupSlack )
        AddCheckSlack( p->WorstClkGatingSetupSlack, &p->WorstSetupSlack,
                       &p->TotalSetupSlack, &p->nSetupViolations );
    if ( Msta_IsSet(p->WorstClkGatingHoldSlack) &&
         p->WorstClkGatingHoldSlack < p->WorstHoldSlack )
        AddCheckSlack( p->WorstClkGatingHoldSlack, &p->WorstHoldSlack,
                       &p->TotalHoldSlack, &p->nHoldViolations );

    for ( i = 0; i < p->vChecks.nSize; i++ )
    {
        MstaCheck *pCheck = MstaCheckArrayAt( &p->vChecks, i );
        if ( !pCheck->Setup.fChecked && !pCheck->Hold.fChecked )
        {
            if ( pCheck->fCutByException ) p->nExcludedEnds++;
            else p->nUnconstrainedEnds++;
        }
        if ( pCheck->Setup.fChecked )
            AddCheckSlack( pCheck->Setup.Slack, &p->WorstSetupSlack,
                           &p->TotalSetupSlack, &p->nSetupViolations );
        if ( pCheck->Hold.fChecked )
            AddCheckSlack( pCheck->Hold.Slack, &p->WorstHoldSlack,
                           &p->TotalHoldSlack, &p->nHoldViolations );
    }
    p->DesignArea = 0.0;
    for ( i = 0; i < p->pDes->vInsts.nSize; i++ )
        p->DesignArea += MstaInstArrayAt( &p->pDes->vInsts, i )->pCell->Area;
    Msta_TiCheckDesignRules( p );
}

int Msta_TimingAnalyze( MstaTiming *p, int fVerbose )
{
    int i, nStartClasses, fTagActive[2] = { 0, 0 };
    char *pfClassUsed = NULL, *pfDataCheckDone = NULL;

    p->fAnalyzed = 0;
    p->fIncrEligible = 0;
    if ( p->pDes->vNets.nSize == 0 )
    {
        Msta_Error( "analyze：设计还没有展平（请先执行 current_design）\n" );
        return 0;
    }
    if ( p->pSdc->fAocv && !p->pSdc->vAocv.nSize )
        Msta_WarnOnce( "AOCV 已开启但尚未读取深度表；未标注弧使用系数 1.0" );
    if ( Msta_SdcClockCount( p->pSdc ) == 0 )
    {
        Msta_Error( "analyze：没有定义时钟，请先用 create_clock 定义\n" );
        return 0;
    }

    FreeNetArrays( p );
    AllocNetArrays( p );
    FreeChecks( p );
    MstaCheckArrayInit( &p->vChecks );
    p->nRegisters = 0;
    p->nLatches = 0;
    p->nCombLoops = 0;

    Msta_TiBuildChecks( p );            /* 1. 端点 */
    BuildCheckIndex( p );
    Msta_TiMarkClockNets( p );          /* 2. 时钟树 */
    Msta_TiAttachClockIds( p );
    Msta_TiWarnUntimedLatches( p );
    Msta_TiMarkIdealNets( p );          /* 3. 理想网络 */
    Msta_TiBuildTopoOrder( p );         /* 4. 拓扑序与逻辑级数 */
    Msta_TiComputeLogicLevels( p );
    p->MaxLogicLevel = 0;
    for ( i = 0; i < p->pDes->vNets.nSize; i++ )
        if ( p->pLevel[i] > p->MaxLogicLevel )
            p->MaxLogicLevel = p->pLevel[i];
    Msta_TiBuildAocvDepths( p );        /* AOCV 结构深度上下文 */
    Msta_TiPropagateClocks( p );        /* 5. 时钟传播 */

    /* 6. 数据到达与检查 */
    p->nDataChecks = 0;
    p->WorstDataCheckSetupSlack = MSTA_NO_TIME;
    p->nClkGatingChecks = 0;
    p->WorstClkGatingSetupSlack = p->WorstClkGatingHoldSlack = MSTA_NO_TIME;
    p->ClkGatingSetupInst = p->ClkGatingHoldInst = -1;
    p->LaunchClockTag = p->CaptureClockTag = -1;

    nStartClasses = Msta_TiBuildStartClasses( p );
    p->nStartClasses = nStartClasses;
    pfClassUsed = (char *)malloc( (size_t)(nStartClasses + 1) );
    assert( pfClassUsed );
    pfDataCheckDone = (char *)calloc( (size_t)Msta_SdcDataCheckCount(p->pSdc) + 1, 1 );
    assert( pfDataCheckDone );
    for ( i = 0; i < Msta_SdcClockCount(p->pSdc); i++ )
    {
        int n, c;
        memset( pfClassUsed, 0, (size_t)(nStartClasses + 1) );
        for ( n = 0; n < p->pDes->vNets.nSize; n++ )
            if ( p->pStartClass[n] >= 0 && !pfClassUsed[p->pStartClass[n]] &&
                 Msta_TiIsStartForClock(p, n, i) )
                pfClassUsed[p->pStartClass[n]] = 1;
        for ( c = 0; c < nStartClasses; c++ )
            if ( pfClassUsed[c] )
            {
                int Tag;
                for ( Tag = 0; Tag < 2; Tag++ )
                {
                    p->LaunchClockTag = Tag;
                    Msta_TiEvaluateCandidatePaths( p, i, c );
                    if ( Msta_SdcClockCount(p->pSdc) == 1 && nStartClasses <= 1 )
                    {
                        for ( n = 0; n < p->pDes->vNets.nSize; n++ )
                            if ( p->pStartClass[n] >= 0 &&
                                 ( p->CornerMax.pnLaunchClock[n] >= 0 ||
                                   p->CornerMin.pnLaunchClock[n] >= 0 ) )
                            {   fTagActive[Tag] = 1; break;   }
                    }
                }
            }
        p->LaunchClockTag = -1;
        Msta_TiCheckDataChecks( p, i, pfDataCheckDone );
        Msta_TiCheckClockGating( p, i, nStartClasses );
    }
    free( pfClassUsed );
    for ( i = 0; i < Msta_SdcDataCheckCount(p->pSdc); i++ )
        if ( !pfDataCheckDone[i] )
        {
            MstaDataCheck *pCheck = Msta_SdcDataCheckByIndex( p->pSdc, i );
            Msta_WarnOnce( "set_data_check -from %s -to %s 没有检查："
                           "两条路径都必须能算出时序，并由同一个时钟发出"
                           "（跨时钟的数据检查未建模）",
                           Msta_NameStr(pCheck->FromText), Msta_NameStr(pCheck->ToText) );
        }
    free( pfDataCheckDone );

    /* 全设计合并前向 AT；随后调用路径精修插件（如有）并逆拓扑传播后向 RAT。 */
    p->LaunchClockTag = -1;
    p->CaptureClockTag = -1;
    p->nPathExclude = 0;
    Msta_TiPropagateData( p, 1, -1, -1 );
    Msta_TiPropagateData( p, 0, -1, -1 );
    if ( p->Plugins.pRefiner && p->Plugins.pRefiner->RefineCheck &&
         p->Plugins.pRefiner != Msta_PluginRefinerNone() )
    {
        for ( i = 0; i < p->vChecks.nSize; i++ )
        {
            MstaCheck *pCheck = MstaCheckArrayAt( &p->vChecks, i );
            if ( pCheck->Setup.fChecked )
                p->Plugins.pRefiner->RefineCheck( p, pCheck, 1 );
            if ( pCheck->Hold.fChecked )
                p->Plugins.pRefiner->RefineCheck( p, pCheck, 0 );
        }
    }
    Msta_TimingPropagateRequired( p, 1 );
    Msta_TimingPropagateRequired( p, 0 );

    /* 7. 汇总：WNS/TNS（含数据检查、门控检查）、面积、DRC */
    RefreshTimingSummary( p );
    {
        int nActiveTags = fTagActive[0] + fTagActive[1];
        p->fIncrEligible = ( Msta_SdcClockCount(p->pSdc) == 1 &&
                             p->nStartClasses <= 1 &&
                             Msta_SdcExceptionCount(p->pSdc) == 0 &&
                             !p->pSdc->fAocv &&
                             p->nCombLoops == 0 &&
                             Msta_SdcDataCheckCount(p->pSdc) == 0 &&
                             p->nClkGatingChecks == 0 &&
                             p->Plugins.pRefiner == Msta_PluginRefinerNone() &&
                             nActiveTags <= 1 );
    }
    if ( fVerbose )
        Msta_Info( "analyze：寄存器 %d 个，端点 %d 个，拓扑序网络 %d 根，时钟网络 %d 根，组合环路 %d 处\n",
                   p->nRegisters, p->vChecks.nSize, p->nTopoOrder,
                   Msta_TimingClockNetCount(p), p->nCombLoops );
    if ( fVerbose && Msta_SdcHasDrcLimits(p->pSdc) )
        Msta_Info( "drc：max_transition 违例 %d 处，max_capacitance 违例 %d 处，max_fanout 违例 %d 处\n",
                   p->nDrcTransitionViol, p->nDrcCapacitanceViol, p->nDrcFanoutViol );
    p->fAnalyzed = 1;
    return 1;
}

static int s_fSetupSort = 1;

static int CompareSlack( const void *pA, const void *pB )
{
    const MstaCheck *pX = (const MstaCheck *)pA;
    const MstaCheck *pY = (const MstaCheck *)pB;
    double a = s_fSetupSort ? (pX->Setup.fChecked ? pX->Setup.Slack : MSTA_NO_TIME)
                            : (pX->Hold.fChecked  ? pX->Hold.Slack  : MSTA_NO_TIME);
    double b = s_fSetupSort ? (pY->Setup.fChecked ? pY->Setup.Slack : MSTA_NO_TIME)
                            : (pY->Hold.fChecked  ? pY->Hold.Slack  : MSTA_NO_TIME);
    if ( a < b ) return -1;
    if ( a > b ) return 1;
    return 0;
}

void Msta_TimingSortChecks( MstaTiming *p, int fSetup )
{
    s_fSetupSort = fSetup;
    if ( p->vChecks.nSize > 1 )
    {
        qsort( p->vChecks.pData, (size_t)p->vChecks.nSize, sizeof(MstaCheck), CompareSlack );
        BuildCheckIndex( p );
    }
}

static int TimingStartNet( MstaTiming *p, int nEndNet, int fMax )
{
    int *pPrev = Msta_TiCornerOf(p, fMax)->pPrevNet;
    int n = nEndNet, nGuard = 0;
    while ( n >= 0 && pPrev[n] >= 0 && nGuard++ < p->pDes->vNets.nSize )
        n = pPrev[n];
    return n;
}

static const char *TimingStartName( MstaTiming *p, int nNet )
{
    MstaNet *pNet = MstaNetArrayAt( &p->pDes->vNets, nNet );
    return pNet->Driver.InstId == MSTA_NO_ID
         ? Msta_NetName( p->pDes, nNet )
         : Msta_InstName( p->pDes, pNet->Driver.InstId );
}

const char *Msta_TimingEndpointName( MstaTiming *p, MstaCheck *pCheck,
                                     int fStart, int fMax )
{
    const MstaCheckCorner *pCorner = Msta_CheckCorner( pCheck, fMax );
    if ( !fStart )
        return pCheck->fToRegister ? Msta_InstName(p->pDes, pCheck->InstId)
                                   : Msta_NetName(p->pDes, pCheck->nEndNet);
    if ( pCorner->pPath && pCorner->nPath > 0 )
        return TimingStartName( p, pCorner->pPath[pCorner->nPath - 1] );
    return TimingStartName( p, TimingStartNet(p, pCheck->nEndNet, fMax) );
}

int Msta_TimingTracePath( MstaTiming *p, MstaCheck *pCheck, int fMax, int *pnNets, int nCap )
{
    const MstaCheckCorner *pCorner = Msta_CheckCorner( pCheck, fMax );
    int *pSaved = pCorner->pPath;
    int nSaved = pCorner->nPath;
    int n = pCheck->nEndNet, k = 0, nGuard = 0;
    int *pPrev = Msta_TiCornerOf(p, fMax)->pPrevNet;
    if ( pSaved )
    {
        int nCopy = nSaved < nCap ? nSaved : nCap;
        memcpy( pnNets, pSaved, (size_t)nCopy * sizeof(int) );
        return nCopy;
    }
    while ( n >= 0 && k < nCap )
    {
        pnNets[k++] = n;
        if ( nGuard++ > p->pDes->vNets.nSize )
        {
            Msta_WarnOnce( "路径回溯超过了保护上限（有环路？）" );
            break;
        }
        n = pPrev[n];
    }
    return k;
}

MstaPinRef *Msta_TimingNetDriver( MstaTiming *p, int nNet )
{
    MstaNet *pNet = MstaNetArrayAt( &p->pDes->vNets, nNet );
    if ( pNet->Driver.InstId == MSTA_NO_ID )
        return NULL;
    return &pNet->Driver;
}

double Msta_TimingNetArrival( MstaTiming *p, int nNet, int fMax )
{
    return Msta_TiCornerOf(p, fMax)->pArr[nNet];
}

int Msta_TimingTimeValid( double Value )
{
    return isfinite( Value ) && Value > -MSTA_NO_TIME * 0.5 && Value < MSTA_NO_TIME * 0.5;
}

double Msta_TimingNetSlewEdge( MstaTiming *p, int nNet, int fMax, int fRise )
{
    int e = fRise ? 1 : 0;
    if ( p == NULL || nNet < 0 || nNet >= p->pDes->vNets.nSize )
        return MSTA_DEFAULT_SLEW;
    if ( p->pfClockNet && p->pfClockNet[nNet] )
    {
        int c, s, fFound = 0;
        double Best = 0.0;
        for ( c = 0; c < Msta_SdcClockCount( p->pSdc ); c++ )
        {
            const MstaClockArr *pArr;
            if ( !Msta_TiNetHasClock(p, c, nNet) )
                continue;
            pArr = Msta_TiClockArrAt( p, c, nNet );
            for ( s = 0; s < 2; s++ )
                if ( Msta_TiClockTagValid(pArr, fMax, e, s) )
                {
                    double Sl = pArr->Slew[fMax][e][s];
                    if ( !fFound || (fMax ? (Sl > Best) : (Sl < Best)) )
                    {   Best = Sl; fFound = 1;   }
                }
        }
        if ( fFound )
            return Best;
    }
    return fRise ? Msta_TiCornerOf(p, fMax)->pSlewRise[nNet]
                 : Msta_TiCornerOf(p, fMax)->pSlewFall[nNet];
}

double Msta_TimingNetArrivalEdge( MstaTiming *p, int nNet, int fMax, int fRise )
{
    if ( p == NULL || nNet < 0 || nNet >= p->pDes->vNets.nSize )
        return fMax ? -MSTA_NO_TIME : MSTA_NO_TIME;
    return fRise ? Msta_TiCornerOf(p, fMax)->pArrRise[nNet]
                 : Msta_TiCornerOf(p, fMax)->pArrFall[nNet];
}

double Msta_TimingNetRequiredEdge( MstaTiming *p, int nNet, int fMax, int fRise )
{
    if ( p == NULL || nNet < 0 || nNet >= p->pDes->vNets.nSize )
        return fMax ? MSTA_NO_TIME : -MSTA_NO_TIME;
    return fRise ? Msta_TiCornerOf(p, fMax)->pReqRise[nNet]
                 : Msta_TiCornerOf(p, fMax)->pReqFall[nNet];
}

/* =====================================================================
   8. 换单元（swap_cell）后的分级增量 STA 与 -verify 全量自检
   ===================================================================== */

typedef struct {
    int  *pHead;
    int  *pNext;
    char *pInQueue;
    int   nNets;
    int   nMaxLevel;
} MstaLevelQueue;

static void LevelQueueInit( MstaLevelQueue *pQ, int nNets, int nMaxLevel )
{
    int l;
    pQ->nNets = nNets;
    pQ->nMaxLevel = nMaxLevel >= 0 ? nMaxLevel : 0;
    pQ->pHead = (int *)malloc( (size_t)(pQ->nMaxLevel + 1) * sizeof(int) );
    pQ->pNext = (int *)malloc( (size_t)(nNets > 0 ? nNets : 1) * sizeof(int) );
    pQ->pInQueue = (char *)calloc( (size_t)(nNets > 0 ? nNets : 1), 1 );
    assert( pQ->pHead && pQ->pNext && pQ->pInQueue );
    for ( l = 0; l <= pQ->nMaxLevel; l++ )
        pQ->pHead[l] = -1;
}

static void LevelQueueFree( MstaLevelQueue *pQ )
{
    free( pQ->pHead );
    free( pQ->pNext );
    free( pQ->pInQueue );
    memset( pQ, 0, sizeof(MstaLevelQueue) );
}

static void LevelQueuePush( MstaLevelQueue *pQ, int nNet, int nLevel )
{
    if ( nNet < 0 || nNet >= pQ->nNets || pQ->pInQueue[nNet] )
        return;
    if ( nLevel < 0 ) nLevel = 0;
    if ( nLevel > pQ->nMaxLevel ) nLevel = pQ->nMaxLevel;
    pQ->pInQueue[nNet] = 1;
    pQ->pNext[nNet] = pQ->pHead[nLevel];
    pQ->pHead[nLevel] = nNet;
}

static int LevelQueuePop( MstaLevelQueue *pQ, int nLevel )
{
    int nNet = pQ->pHead[nLevel];
    if ( nNet < 0 )
        return -1;
    pQ->pHead[nLevel] = pQ->pNext[nNet];
    pQ->pNext[nNet] = -1;
    pQ->pInQueue[nNet] = 0;
    return nNet;
}

static int HasEffectiveDataEdge( MstaTiming *p, int nInst, MstaInst *pInst, MstaCell *pCell,
                                 MstaId InName, MstaId OutName, int fMax )
{
    MstaCell *pCombCell;
    int a, fAnyComb = 0, fActive = 0;
    if ( Msta_SdcTimingDisabled(p->pSdc, nInst, InName, OutName) )
        return 0;
    pCombCell = Msta_TiTimingCombCell( p, pCell, InName, OutName, fMax );
    for ( a = 0; a < pCombCell->vArcs.nSize; a++ )
    {
        MstaArc *pArc = MstaArcArrayAt( &pCombCell->vArcs, a );
        if ( pArc->InPin != InName || pArc->OutPin != OutName ||
             !Msta_ArcIsComb( pCombCell, pArc ) )
            continue;
        fAnyComb = 1;
        if ( Msta_TiDataArcWhenActive( p, nInst, pInst, pCombCell, pArc ) )
        {
            fActive = 1;
            break;
        }
    }
    if ( fAnyComb )
        return fActive;
    return !Msta_CellHasAsyncArc( pCell, InName, OutName );
}

static int CanSwapIncremental( MstaTiming *p, int nInst, MstaCell *pNewCell )
{
    MstaInst *pInst;
    MstaCell *pOldCell;
    int nPins, k, o, m;
    if ( !p->fAnalyzed || !p->fIncrEligible ||
         nInst < 0 || nInst >= p->pDes->vInsts.nSize || pNewCell == NULL )
        return 0;
    pInst = Msta_TiInstAt( p, nInst );
    pOldCell = pInst->pCell;
    if ( pInst->fBlackBox || pOldCell == NULL || pNewCell->fBlackBox ||
         pOldCell->fIgnore || pNewCell->fIgnore ||
         pOldCell->fSequential || pNewCell->fSequential ||
         pOldCell->fLatch || pNewCell->fLatch )
        return 0;
    nPins = pOldCell->vPins.nSize;
    if ( pInst->nPins != nPins || pNewCell->vPins.nSize != nPins )
        return 0;
    for ( k = 0; k < pInst->nPins; k++ )
    {
        int nNet = pInst->pNets[k];
        if ( nNet >= 0 && nNet < p->pDes->vNets.nSize &&
             ( p->pfClockNet[nNet] || p->pfIdealNet[nNet] ) )
            return 0;
    }
    for ( k = 0; k < nPins; k++ )
    {
        MstaPin *pPin = MstaPinArrayAt( &pOldCell->vPins, k );
        int nDst = Msta_CellPinIndexOf( pNewCell, pPin->Name );
        if ( nDst < 0 || nDst >= nPins ||
             MstaPinArrayAt(&pNewCell->vPins, nDst)->Dir != pPin->Dir )
            return 0;
    }
    /* 确保替换前后有效数据依赖弧集合完全一致（保证既有拓扑序与逻辑级数严格不变）。 */
    for ( k = 0; k < nPins; k++ )
    {
        MstaPin *pIn = MstaPinArrayAt( &pOldCell->vPins, k );
        if ( pIn->Dir == MSTA_DIR_OUTPUT || pInst->pNets[k] < 0 )
            continue;
        for ( o = 0; o < nPins; o++ )
        {
            MstaPin *pOut = MstaPinArrayAt( &pOldCell->vPins, o );
            if ( pOut->Dir != MSTA_DIR_OUTPUT || pInst->pNets[o] < 0 )
                continue;
            for ( m = 0; m < 2; m++ )
                if ( HasEffectiveDataEdge(p, nInst, pInst, pOldCell, pIn->Name, pOut->Name, m) !=
                     HasEffectiveDataEdge(p, nInst, pInst, pNewCell, pIn->Name, pOut->Name, m) )
                    return 0;
        }
    }
    return 1;
}

static void RemapSavedCheckPins( MstaTiming *p, int nInst, const int *pPinMap, int nPins )
{
    int fChanged = 0, i, j, corner;
    for ( i = 0; i < nPins; i++ )
        if ( pPinMap[i] != i ) { fChanged = 1; break; }
    if ( !fChanged )
        return;
    for ( i = 0; i < p->vChecks.nSize; i++ )
    {
        MstaCheck *pCheck = MstaCheckArrayAt( &p->vChecks, i );
        for ( corner = 0; corner < 2; corner++ )
        {
            MstaCheckCorner *pC = Msta_CheckCorner( pCheck, corner );
            if ( !pC->fChecked || pC->nPath <= 0 || pC->pPathLoadPin == NULL )
                continue;
            for ( j = 0; j < pC->nPath; j++ )
            {
                int nNet = pC->pPath[j];
                if ( nNet < 0 || nNet >= p->pDes->vNets.nSize )
                    continue;
                if ( MstaNetArrayAt(&p->pDes->vNets, nNet)->Driver.InstId == nInst )
                {
                    int nOldPin = pC->pPathLoadPin[j];
                    if ( nOldPin >= 0 && nOldPin < nPins )
                        pC->pPathLoadPin[j] = pPinMap[nOldPin];
                }
            }
        }
    }
}

typedef struct {
    double   Arr, ArrR, ArrF;
    double   Slew, SlewR, SlewF;
    double   LaunchEdge;
    int      PrevNet, LaunchClock;
    char     LaunchRises;
    MstaPrev PrevR, PrevF;
} MstaNetFwdSnap;

static void CaptureNetFwd( const MstaCorner *pC, int nNet, MstaNetFwdSnap *pS )
{
    pS->Arr         = pC->pArr[nNet];
    pS->ArrR        = pC->pArrRise[nNet];
    pS->ArrF        = pC->pArrFall[nNet];
    pS->Slew        = pC->pSlew[nNet];
    pS->SlewR       = pC->pSlewRise[nNet];
    pS->SlewF       = pC->pSlewFall[nNet];
    pS->LaunchEdge  = pC->pdLaunchEdge[nNet];
    pS->PrevNet     = pC->pPrevNet[nNet];
    pS->LaunchClock = pC->pnLaunchClock[nNet];
    pS->LaunchRises = pC->pfLaunchRises[nNet];
    pS->PrevR       = pC->pPrevRise[nNet];
    pS->PrevF       = pC->pPrevFall[nNet];
}

static int NetFwdChanged( const MstaCorner *pC, int nNet, const MstaNetFwdSnap *pOld )
{
    return pC->pArr[nNet]         != pOld->Arr ||
           pC->pArrRise[nNet]     != pOld->ArrR ||
           pC->pArrFall[nNet]     != pOld->ArrF ||
           pC->pSlew[nNet]        != pOld->Slew ||
           pC->pSlewRise[nNet]    != pOld->SlewR ||
           pC->pSlewFall[nNet]    != pOld->SlewF ||
           pC->pdLaunchEdge[nNet] != pOld->LaunchEdge ||
           pC->pPrevNet[nNet]     != pOld->PrevNet ||
           pC->pnLaunchClock[nNet]!= pOld->LaunchClock ||
           pC->pfLaunchRises[nNet]!= pOld->LaunchRises ||
           pC->pPrevRise[nNet].Net     != pOld->PrevR.Net ||
           pC->pPrevRise[nNet].Edge    != pOld->PrevR.Edge ||
           pC->pPrevRise[nNet].LoadPin != pOld->PrevR.LoadPin ||
           pC->pPrevFall[nNet].Net     != pOld->PrevF.Net ||
           pC->pPrevFall[nNet].Edge    != pOld->PrevF.Edge ||
           pC->pPrevFall[nNet].LoadPin != pOld->PrevF.LoadPin;
}

static void PushCombFanoutNets( MstaTiming *p, MstaLevelQueue *pQ, int nNet )
{
    MstaNet *pNet = MstaNetArrayAt( &p->pDes->vNets, nNet );
    int i, o;
    for ( i = 0; i < pNet->vLoads.nSize; i++ )
    {
        MstaPinRef *pRef = MstaPinRefArrayAt( &pNet->vLoads, i );
        MstaInst *pLoad = Msta_TiInstAt( p, pRef->InstId );
        MstaCell *pCell = pLoad->pCell;
        if ( pCell->fSequential )
            continue;
        for ( o = 0; o < pLoad->nPins && o < pCell->vPins.nSize; o++ )
        {
            if ( MstaPinArrayAt(&pCell->vPins, o)->Dir != MSTA_DIR_OUTPUT )
                continue;
            {
                int nOut = pLoad->pNets[o];
                if ( nOut >= 0 && nOut < p->pDes->vNets.nSize &&
                     !MstaNetArrayAt(&p->pDes->vNets, nOut)->fConst &&
                     !p->pfClockNet[nOut] )
                    LevelQueuePush( pQ, nOut, p->pLevel[nOut] );
            }
        }
    }
}

static void PushCombFaninNets( MstaTiming *p, MstaLevelQueue *pQ, int nNet )
{
    MstaNet *pNet = MstaNetArrayAt( &p->pDes->vNets, nNet );
    MstaInst *pDriver;
    MstaCell *pCell;
    int k;
    if ( pNet->Driver.InstId == MSTA_NO_ID )
        return;
    pDriver = Msta_TiInstAt( p, pNet->Driver.InstId );
    pCell = pDriver->pCell;
    if ( pCell->fSequential )
        return;
    for ( k = 0; k < pDriver->nPins && k < pCell->vPins.nSize; k++ )
    {
        if ( MstaPinArrayAt(&pCell->vPins, k)->Dir == MSTA_DIR_OUTPUT )
            continue;
        {
            int nIn = pDriver->pNets[k];
            if ( nIn >= 0 && nIn < p->pDes->vNets.nSize &&
                 !MstaNetArrayAt(&p->pDes->vNets, nIn)->fConst &&
                 !p->pfClockNet[nIn] )
                LevelQueuePush( pQ, nIn, p->pLevel[nIn] );
        }
    }
}

typedef struct {
    MstaId InstId;
    int    nCheck, nDataPin, nEndNet;
    int    fToRegister, fAsync, fRecovery, fRemoval, nAsyncCheck;
    int    fSetupChecked, fHoldChecked;
    double SetupArr, SetupReq, SetupSlk;
    double HoldArr,  HoldReq,  HoldSlk;
} MstaCheckSnap;

typedef struct {
    int       nNets, nChecks;
    double   *pCornerDoubles; /* [2 corners * 9 arrays * nNets] */
    MstaCheckSnap *pChecks;
    double    WorstSetupSlack, TotalSetupSlack, WorstHoldSlack, TotalHoldSlack, DesignArea;
    int       nSetupViolations, nHoldViolations, nUnconstrainedEnds, nExcludedEnds;
    int       nDrcTrans, nDrcCap, nDrcMinCap, nDrcFan, WorstDrcFanout;
    double    WorstDrcTrans, WorstDrcCap, WorstDrcMinCap;
} MstaFullVerifySnap;

static void CopyCornerArraysOut( const MstaCorner *pC, int nNets, double *pDst )
{
    size_t sz = (size_t)nNets * sizeof(double);
    if ( nNets <= 0 ) return;
    memcpy( pDst + (size_t)0 * nNets, pC->pArr,      sz );
    memcpy( pDst + (size_t)1 * nNets, pC->pArrRise,  sz );
    memcpy( pDst + (size_t)2 * nNets, pC->pArrFall,  sz );
    memcpy( pDst + (size_t)3 * nNets, pC->pReq,      sz );
    memcpy( pDst + (size_t)4 * nNets, pC->pReqRise,  sz );
    memcpy( pDst + (size_t)5 * nNets, pC->pReqFall,  sz );
    memcpy( pDst + (size_t)6 * nNets, pC->pSlew,     sz );
    memcpy( pDst + (size_t)7 * nNets, pC->pSlewRise, sz );
    memcpy( pDst + (size_t)8 * nNets, pC->pSlewFall, sz );
}

static void TakeVerifySnapshot( MstaTiming *p, MstaFullVerifySnap *pSnap )
{
    int nNets = p->pDes->vNets.nSize, nChks = p->vChecks.nSize, i;
    pSnap->nNets = nNets;
    pSnap->nChecks = nChks;
    pSnap->pCornerDoubles = (double *)malloc( (size_t)(nNets > 0 ? nNets : 1) * 18 * sizeof(double) );
    pSnap->pChecks = (MstaCheckSnap *)malloc( (size_t)(nChks > 0 ? nChks : 1) * sizeof(MstaCheckSnap) );
    assert( pSnap->pCornerDoubles && pSnap->pChecks );
    CopyCornerArraysOut( &p->CornerMax, nNets, pSnap->pCornerDoubles );
    CopyCornerArraysOut( &p->CornerMin, nNets, pSnap->pCornerDoubles + (size_t)9 * nNets );
    for ( i = 0; i < nChks; i++ )
    {
        const MstaCheck *pChk = MstaCheckArrayAt( &p->vChecks, i );
        MstaCheckSnap *pOut = &pSnap->pChecks[i];
        pOut->InstId        = pChk->InstId;
        pOut->nCheck        = pChk->nCheck;
        pOut->nDataPin      = pChk->nDataPin;
        pOut->nEndNet       = pChk->nEndNet;
        pOut->fToRegister   = pChk->fToRegister;
        pOut->fAsync        = pChk->fAsync;
        pOut->fRecovery     = pChk->fRecovery;
        pOut->fRemoval      = pChk->fRemoval;
        pOut->nAsyncCheck   = pChk->nAsyncCheck;
        pOut->fSetupChecked = pChk->Setup.fChecked;
        pOut->SetupArr      = pChk->Setup.Arrival;
        pOut->SetupReq      = pChk->Setup.Required;
        pOut->SetupSlk      = pChk->Setup.Slack;
        pOut->fHoldChecked  = pChk->Hold.fChecked;
        pOut->HoldArr       = pChk->Hold.Arrival;
        pOut->HoldReq       = pChk->Hold.Required;
        pOut->HoldSlk       = pChk->Hold.Slack;
    }
    pSnap->WorstSetupSlack      = p->WorstSetupSlack;
    pSnap->TotalSetupSlack      = p->TotalSetupSlack;
    pSnap->WorstHoldSlack       = p->WorstHoldSlack;
    pSnap->TotalHoldSlack       = p->TotalHoldSlack;
    pSnap->DesignArea           = p->DesignArea;
    pSnap->nSetupViolations     = p->nSetupViolations;
    pSnap->nHoldViolations      = p->nHoldViolations;
    pSnap->nUnconstrainedEnds   = p->nUnconstrainedEnds;
    pSnap->nExcludedEnds        = p->nExcludedEnds;
    pSnap->nDrcTrans            = p->nDrcTransitionViol;
    pSnap->nDrcCap              = p->nDrcCapacitanceViol;
    pSnap->nDrcMinCap           = p->nDrcMinCapacitanceViol;
    pSnap->nDrcFan              = p->nDrcFanoutViol;
    pSnap->WorstDrcFanout       = p->WorstDrcFanout;
    pSnap->WorstDrcTrans        = p->WorstDrcTransition;
    pSnap->WorstDrcCap          = p->WorstDrcCapacitance;
    pSnap->WorstDrcMinCap       = p->WorstDrcMinCapacitance;
}

static void FreeVerifySnapshot( MstaFullVerifySnap *pSnap )
{
    free( pSnap->pCornerDoubles );
    free( pSnap->pChecks );
    memset( pSnap, 0, sizeof(MstaFullVerifySnap) );
}

static int CompareIntVal( int Incr, int Full, const char *pKind )
{
    if ( Incr != Full )
    {
        Msta_Error( "swap_cell -verify 失败：%s 不一致 incr=%d vs full=%d\n",
                    pKind, Incr, Full );
        return 0;
    }
    return 1;
}

static int CompareOptVal( double Incr, int fIncrValid, double Full, int fFullValid,
                          double *pMaxDiff, const char *pKind, const char *pCtx )
{
    double Diff;
    if ( fIncrValid != fFullValid )
    {
        Msta_Error( "swap_cell -verify 失败：%s（%s）有效位不一致 incr=%d vs full=%d\n",
                    pKind, pCtx, fIncrValid, fFullValid );
        return 0;
    }
    if ( !fFullValid )
        return 1;
    Diff = fabs( Incr - Full );
    if ( Diff > *pMaxDiff )
        *pMaxDiff = Diff;
    return 1;
}

static int VerifyIncrAgainstFull( MstaTiming *p, const MstaFullVerifySnap *pSnap, double *pMaxDiff )
{
    int nNets = p->pDes->vNets.nSize, c, n, i;
    *pMaxDiff = 0.0;
    if ( nNets != pSnap->nNets || p->vChecks.nSize != pSnap->nChecks )
    {
        Msta_Error( "swap_cell -verify 失败：规模不一致 nets %d vs %d, checks %d vs %d\n",
                    pSnap->nNets, nNets, pSnap->nChecks, p->vChecks.nSize );
        return 0;
    }
    for ( c = 0; c < 2; c++ )
    {
        int fMax = ( c == 0 ) ? 1 : 0;
        const MstaCorner *pC = Msta_TiCornerOf( p, fMax );
        const double *pBase = pSnap->pCornerDoubles + (size_t)c * 9 * nNets;
        for ( n = 0; n < nNets; n++ )
        {
            const char *pNetName = Msta_NetName( p->pDes, n );
            double iaR = pBase[(size_t)1 * nNets + n], faR = pC->pArrRise[n];
            double iaF = pBase[(size_t)2 * nNets + n], faF = pC->pArrFall[n];
            double irM = pBase[(size_t)3 * nNets + n], frM = pC->pReq[n];
            double irR = pBase[(size_t)4 * nNets + n], frR = pC->pReqRise[n];
            double irF = pBase[(size_t)5 * nNets + n], frF = pC->pReqFall[n];
            if ( !CompareOptVal(iaR, Msta_TiArrivalSet(iaR, fMax), faR, Msta_TiArrivalSet(faR, fMax),
                                pMaxDiff, fMax ? "max ArrRise" : "min ArrRise", pNetName) ) return 0;
            if ( !CompareOptVal(iaF, Msta_TiArrivalSet(iaF, fMax), faF, Msta_TiArrivalSet(faF, fMax),
                                pMaxDiff, fMax ? "max ArrFall" : "min ArrFall", pNetName) ) return 0;
            if ( !CompareOptVal(pBase[(size_t)0 * nNets + n], 1, pC->pArr[n], 1,
                                pMaxDiff, fMax ? "max Arr" : "min Arr", pNetName) ) return 0;
            if ( !CompareOptVal(irM, Msta_TimingTimeValid(irM), frM, Msta_TimingTimeValid(frM),
                                pMaxDiff, fMax ? "max Req" : "min Req", pNetName) ) return 0;
            if ( !CompareOptVal(irR, Msta_TimingTimeValid(irR), frR, Msta_TimingTimeValid(frR),
                                pMaxDiff, fMax ? "max ReqRise" : "min ReqRise", pNetName) ) return 0;
            if ( !CompareOptVal(irF, Msta_TimingTimeValid(irF), frF, Msta_TimingTimeValid(frF),
                                pMaxDiff, fMax ? "max ReqFall" : "min ReqFall", pNetName) ) return 0;
            if ( !CompareOptVal(pBase[(size_t)6 * nNets + n], 1, pC->pSlew[n], 1,
                                pMaxDiff, fMax ? "max Slew" : "min Slew", pNetName) ) return 0;
            if ( !CompareOptVal(pBase[(size_t)7 * nNets + n], 1, pC->pSlewRise[n], 1,
                                pMaxDiff, fMax ? "max SlewRise" : "min SlewRise", pNetName) ) return 0;
            if ( !CompareOptVal(pBase[(size_t)8 * nNets + n], 1, pC->pSlewFall[n], 1,
                                pMaxDiff, fMax ? "max SlewFall" : "min SlewFall", pNetName) ) return 0;
        }
    }
    for ( i = 0; i < pSnap->nChecks; i++ )
    {
        const MstaCheckSnap *pS = &pSnap->pChecks[i];
        const MstaCheck *pMatch = NULL;
        const char *pEndName = Msta_NetName( p->pDes, pS->nEndNet );
        int idx;
        for ( idx = p->pFirstCheck[pS->nEndNet]; idx >= 0; idx = p->pNextCheck[idx] )
        {
            const MstaCheck *pCand = MstaCheckArrayAt( &p->vChecks, idx );
            if ( pCand->InstId == pS->InstId && pCand->nCheck == pS->nCheck &&
                 pCand->nDataPin == pS->nDataPin && pCand->fToRegister == pS->fToRegister &&
                 pCand->fAsync == pS->fAsync && pCand->fRecovery == pS->fRecovery &&
                 pCand->fRemoval == pS->fRemoval && pCand->nAsyncCheck == pS->nAsyncCheck )
            {
                pMatch = pCand;
                break;
            }
        }
        if ( pMatch == NULL )
        {
            Msta_Error( "swap_cell -verify 失败：找不到端点 \"%s\" 对应的检查项\n", pEndName );
            return 0;
        }
        if ( !CompareOptVal(pS->SetupArr, pS->fSetupChecked, pMatch->Setup.Arrival, pMatch->Setup.fChecked,
                            pMaxDiff, "check Setup.Arrival", pEndName) ||
             !CompareOptVal(pS->SetupReq, pS->fSetupChecked, pMatch->Setup.Required, pMatch->Setup.fChecked,
                            pMaxDiff, "check Setup.Required", pEndName) ||
             !CompareOptVal(pS->SetupSlk, pS->fSetupChecked, pMatch->Setup.Slack, pMatch->Setup.fChecked,
                            pMaxDiff, "check Setup.Slack", pEndName) ||
             !CompareOptVal(pS->HoldArr, pS->fHoldChecked, pMatch->Hold.Arrival, pMatch->Hold.fChecked,
                            pMaxDiff, "check Hold.Arrival", pEndName) ||
             !CompareOptVal(pS->HoldReq, pS->fHoldChecked, pMatch->Hold.Required, pMatch->Hold.fChecked,
                            pMaxDiff, "check Hold.Required", pEndName) ||
             !CompareOptVal(pS->HoldSlk, pS->fHoldChecked, pMatch->Hold.Slack, pMatch->Hold.fChecked,
                            pMaxDiff, "check Hold.Slack", pEndName) )
            return 0;
    }
    if ( !CompareIntVal(pSnap->nSetupViolations, p->nSetupViolations, "nSetupViolations") ||
         !CompareIntVal(pSnap->nHoldViolations, p->nHoldViolations, "nHoldViolations") ||
         !CompareIntVal(pSnap->nUnconstrainedEnds, p->nUnconstrainedEnds, "nUnconstrainedEnds") ||
         !CompareIntVal(pSnap->nExcludedEnds, p->nExcludedEnds, "nExcludedEnds") ||
         !CompareIntVal(pSnap->nDrcTrans, p->nDrcTransitionViol, "nDrcTrans") ||
         !CompareIntVal(pSnap->nDrcCap, p->nDrcCapacitanceViol, "nDrcCap") ||
         !CompareIntVal(pSnap->nDrcMinCap, p->nDrcMinCapacitanceViol, "nDrcMinCap") ||
         !CompareIntVal(pSnap->nDrcFan, p->nDrcFanoutViol, "nDrcFan") ||
         !CompareIntVal(pSnap->WorstDrcFanout, p->WorstDrcFanout, "WorstDrcFanout") ||
         !CompareOptVal(pSnap->DesignArea, 1, p->DesignArea, 1, pMaxDiff, "DesignArea", "summary") ||
         !CompareOptVal(pSnap->WorstSetupSlack, Msta_TimingTimeValid(pSnap->WorstSetupSlack),
                        p->WorstSetupSlack, Msta_TimingTimeValid(p->WorstSetupSlack),
                        pMaxDiff, "WorstSetupSlack", "summary") ||
         !CompareOptVal(pSnap->TotalSetupSlack, 1, p->TotalSetupSlack, 1, pMaxDiff, "TotalSetupSlack", "summary") ||
         !CompareOptVal(pSnap->WorstHoldSlack, Msta_TimingTimeValid(pSnap->WorstHoldSlack),
                        p->WorstHoldSlack, Msta_TimingTimeValid(p->WorstHoldSlack),
                        pMaxDiff, "WorstHoldSlack", "summary") ||
         !CompareOptVal(pSnap->TotalHoldSlack, 1, p->TotalHoldSlack, 1, pMaxDiff, "TotalHoldSlack", "summary") ||
         !CompareOptVal(pSnap->WorstDrcTrans, 1, p->WorstDrcTransition, 1, pMaxDiff, "WorstDrcTrans", "summary") ||
         !CompareOptVal(pSnap->WorstDrcCap, 1, p->WorstDrcCapacitance, 1, pMaxDiff, "WorstDrcCap", "summary") ||
         !CompareOptVal(pSnap->WorstDrcMinCap, 1, p->WorstDrcMinCapacitance, 1, pMaxDiff, "WorstDrcMinCap", "summary") )
        return 0;
    if ( *pMaxDiff > 1e-9 )
    {
        Msta_Error( "swap_cell -verify 失败：增量与全量最大偏差 %.6e ps > 1e-9 ps\n", *pMaxDiff );
        return 0;
    }
    return 1;
}

int Msta_TimingSwapCell( MstaTiming *p, int nInst, MstaCell *pNewCell,
                         int fVerify, MstaSwapStats *pStats )
{
    MstaSwapStats LocalStats;
    MstaInst *pInst;
    int nNets, nPins, bCanIncr, k;
    int vPinMapStack[64];
    int *pPinMap = vPinMapStack;
    clock_t t0 = clock();

    memset( &LocalStats, 0, sizeof(LocalStats) );
    if ( p == NULL || p->pDes == NULL || nInst < 0 ||
         nInst >= p->pDes->vInsts.nSize || pNewCell == NULL )
        return 0;
    nNets = p->pDes->vNets.nSize;
    LocalStats.nTotalNets = nNets;
    pInst = Msta_TiInstAt( p, nInst );
    nPins = pInst->nPins;
    if ( nPins > 64 )
    {
        pPinMap = (int *)malloc( (size_t)nPins * sizeof(int) );
        assert( pPinMap );
    }

    bCanIncr = CanSwapIncremental( p, nInst, pNewCell );
    if ( !Msta_DesignSwapInstCell(p->pDes, p->pLib, nInst, pNewCell, pPinMap, nPins) )
    {
        if ( nPins > 64 ) free( pPinMap );
        return 0;
    }

    if ( !bCanIncr )
    {
        if ( nPins > 64 ) free( pPinMap );
        if ( !Msta_TimingAnalyze(p, 0) )
            return 0;
        LocalStats.fIncremental = 0;
        LocalStats.nDirtyNets = nNets;
        LocalStats.nChecksUpdated = p->vChecks.nSize;
    }
    else
    {
        MstaLevelQueue Q;
        char *pfFwdChanged = (char *)calloc( (size_t)(nNets > 0 ? nNets : 1), 1 );
        char *pfDirtySeen  = (char *)calloc( (size_t)(nNets > 0 ? nNets : 1), 1 );
        char *pfChkDone    = (char *)calloc( (size_t)(p->vChecks.nSize > 0 ? p->vChecks.nSize : 1), 1 );
        int Lvl, nNet;
        assert( pfFwdChanged && pfDirtySeen && pfChkDone );

        RemapSavedCheckPins( p, nInst, pPinMap, nPins );
        if ( nPins > 64 ) free( pPinMap );

        p->LaunchClockTag = -1;
        p->CaptureClockTag = -1;
        p->nPathExclude = 0;

        /* 1. 分级前向增量传播（输入电容变动可能同时影响前驱负载与兄弟 Elmore 线长） */
        LevelQueueInit( &Q, nNets, p->MaxLogicLevel );
        for ( k = 0; k < pInst->nPins && k < pInst->pCell->vPins.nSize; k++ )
        {
            int nConn = pInst->pNets[k];
            const MstaPin *pPin = MstaPinArrayAt( &pInst->pCell->vPins, k );
            if ( nConn < 0 || nConn >= nNets || MstaNetArrayAt(&p->pDes->vNets, nConn)->fConst )
                continue;
            LevelQueuePush( &Q, nConn, p->pLevel[nConn] );
            if ( pPin->Dir != MSTA_DIR_OUTPUT )
                PushCombFanoutNets( p, &Q, nConn );
        }
        for ( Lvl = 0; Lvl <= Q.nMaxLevel; Lvl++ )
        {
            while ( (nNet = LevelQueuePop(&Q, Lvl)) >= 0 )
            {
                MstaNetFwdSnap SnapMax, SnapMin;
                int fChg;
                CaptureNetFwd( &p->CornerMax, nNet, &SnapMax );
                CaptureNetFwd( &p->CornerMin, nNet, &SnapMin );
                Msta_TiEvalNetForward( p, 1, nNet, -1, -1 );
                Msta_TiEvalNetForward( p, 0, nNet, -1, -1 );
                pfDirtySeen[nNet] = 1;
                fChg = NetFwdChanged( &p->CornerMax, nNet, &SnapMax ) ||
                       NetFwdChanged( &p->CornerMin, nNet, &SnapMin );
                if ( fChg )
                {
                    pfFwdChanged[nNet] = 1;
                    PushCombFanoutNets( p, &Q, nNet );
                }
            }
        }
        LevelQueueFree( &Q );

        /* 2. 重算与脏网络及换单元直接扇入相连的端点检查。 */
        for ( k = 0; k < pInst->nPins && k < pInst->pCell->vPins.nSize; k++ )
        {
            int nConn = pInst->pNets[k];
            if ( nConn >= 0 && nConn < nNets )
                pfFwdChanged[nConn] = 1;
        }
        if ( p->pFirstCheck != NULL )
        {
            for ( nNet = 0; nNet < nNets; nNet++ )
            {
                int chk;
                if ( !pfFwdChanged[nNet] )
                    continue;
                for ( chk = p->pFirstCheck[nNet]; chk >= 0; chk = p->pNextCheck[chk] )
                {
                    if ( pfChkDone[chk] )
                        continue;
                    pfChkDone[chk] = 1;
                    Msta_TiReevalSingleCheck( p, chk, 0, 0 );
                    LocalStats.nChecksUpdated++;
                }
            }
        }

        /* 3. 分级逆拓扑后向拉取（pull）重算 RAT。 */
        LevelQueueInit( &Q, nNets, p->MaxLogicLevel );
        for ( nNet = 0; nNet < nNets; nNet++ )
            if ( pfFwdChanged[nNet] )
                LevelQueuePush( &Q, nNet, p->pLevel[nNet] );
        for ( k = 0; k < pInst->nPins && k < pInst->pCell->vPins.nSize; k++ )
        {
            const MstaPin *pPin = MstaPinArrayAt( &pInst->pCell->vPins, k );
            int nConn = pInst->pNets[k];
            if ( pPin->Dir != MSTA_DIR_OUTPUT && nConn >= 0 && nConn < nNets )
                PushCombFaninNets( p, &Q, nConn );
        }
        for ( Lvl = Q.nMaxLevel; Lvl >= 0; Lvl-- )
        {
            while ( (nNet = LevelQueuePop(&Q, Lvl)) >= 0 )
            {
                double OldMaxR = p->CornerMax.pReqRise[nNet];
                double OldMaxF = p->CornerMax.pReqFall[nNet];
                double OldMinR = p->CornerMin.pReqRise[nNet];
                double OldMinF = p->CornerMin.pReqFall[nNet];
                Msta_TiEvalNetRequired( p, 1, nNet );
                Msta_TiEvalNetRequired( p, 0, nNet );
                pfDirtySeen[nNet] = 1;
                if ( p->CornerMax.pReqRise[nNet] != OldMaxR ||
                     p->CornerMax.pReqFall[nNet] != OldMaxF ||
                     p->CornerMin.pReqRise[nNet] != OldMinR ||
                     p->CornerMin.pReqFall[nNet] != OldMinF )
                    PushCombFaninNets( p, &Q, nNet );
            }
        }
        LevelQueueFree( &Q );

        for ( nNet = 0; nNet < nNets; nNet++ )
            if ( pfDirtySeen[nNet] )
                LocalStats.nDirtyNets++;
        free( pfFwdChanged );
        free( pfDirtySeen );
        free( pfChkDone );

        RefreshTimingSummary( p );
        p->fAnalyzed = 1;
        LocalStats.fIncremental = 1;
    }

    LocalStats.ElapsedMs = (double)(clock() - t0) * 1000.0 / (double)CLOCKS_PER_SEC;

    if ( fVerify )
    {
        MstaFullVerifySnap Snap;
        int fMatch;
        TakeVerifySnapshot( p, &Snap );
        if ( !Msta_TimingAnalyze(p, 0) )
        {
            FreeVerifySnapshot( &Snap );
            return 0;
        }
        fMatch = VerifyIncrAgainstFull( p, &Snap, &LocalStats.MaxDiffPs );
        FreeVerifySnapshot( &Snap );
        if ( !fMatch )
        {
            if ( pStats ) *pStats = LocalStats;
            return 0;
        }
    }

    if ( pStats )
        *pStats = LocalStats;
    return 1;
}
