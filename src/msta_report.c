/**CFile***************************************************************

  FileName    [msta_report.c]

  Synopsis    [报告打印。]

  路径表的排版刻意做得"看得见算式"：每一行给出这一点的到达时间和相对上一点的
  增量，末尾把 required = capture - setup - uncertainty 三项分开列出来。
  学 STA 的人最需要的就是能逐行核对自己手算的结果。

***********************************************************************/

#include "msta_report.h"
#include "msta_util.h"

#define MSTA_PATH_MAX 4096        /* 单条路径最多打印这么多点（更长的链是异常） */

/* set_data_check 的结果不在路径分组里，报告里单列一行，免得和按组的和差不上。 */
#define MSTA_REPORT_DATA_GROUP "**data check**"
#define MSTA_REPORT_GATING_GROUP "**clock gating**"

static const char *Msta_PeriodText( double ps, char *pBuf, int nBuf )
{
    snprintf( pBuf, (size_t)nBuf, "%.3f", ps / 1000.0 );   /* ps -> ns */
    return pBuf;
}

/* 打印路径上的一点：单元.脚 / 网络名 / 增量 / 到达。 */
static void Msta_PrintPathPoint( MstaTiming *p, FILE *pFile, int nNet, int fMax,
                                 double Arrival, double PrevArrival, double PrevSlew )
{
    MstaDesign *pDes = p->pDes;
    MstaNet *pNet = MstaNetArrayAt( &pDes->vNets, nNet );
    MstaPinRef *pRef = Msta_TimingNetDriver( p, nNet );
    char sInc[16], sArr[16];

    if ( pRef == NULL )
    {
        /* 没有驱动：顶层输入端口，或者是被常量的网络 */
        fprintf( pFile, "  %-42s %10s %10s   (input port %s)\n",
                 Msta_NetName( pDes, nNet ),
                 Msta_PeriodText( Arrival - PrevArrival, sInc, sizeof(sInc) ),
                 Msta_PeriodText( Arrival, sArr, sizeof(sArr) ),
                 pNet->fTopPort ? "" : "[undriven]" );
        return;
    }
    {
        MstaInst *pInst = MstaInstArrayAt( &pDes->vInsts, pRef->InstId );
        MstaPin *pPin = MstaPinArrayAt( &pInst->pCell->vPins, pRef->PinId );
        char sPoint[256];
        snprintf( sPoint, sizeof(sPoint), "%s/%s",
                  Msta_InstName( pDes, pRef->InstId ), Msta_NameStr( pPin->Name ) );
        fprintf( pFile, "  %-42s %10s %10s   %s  pin %s\n",
                 sPoint,
                 Msta_PeriodText( Arrival - PrevArrival, sInc, sizeof(sInc) ),
                 Msta_PeriodText( Arrival, sArr, sizeof(sArr) ),
                 Msta_NameStr( pInst->pCell->Name ),
                 ( PrevSlew > 0.0 ) ? "driving" : "-" );
    }
}

static void Msta_PrintOnePath( MstaTiming *p, MstaCheck *pCheck, int fSetup, FILE *pFile )
{
    MstaDesign *pDes = p->pDes;
    int nNets[MSTA_PATH_MAX];
    int nCount, i;
    int fMax = fSetup;
    const MstaCheckCorner *pCorner = Msta_CheckCorner( pCheck, fSetup );
    double Arrival = pCorner->Arrival, Required = pCorner->Required, Slack = pCorner->Slack;
    MstaId nLaunchClock = pCorner->LaunchClock;
    double LaunchTime = pCorner->LaunchTime;
    double CaptureTime = pCorner->CaptureTime;
    /* 跨时钟搜索把出发沿推到第 k 拍时，路径上的时间整体平移（见 LaunchCycleShift）。 */
    double LaunchShift = pCorner->LaunchShift;
    const char *pKind = pCheck->fAsync
                      ? (pCheck->fRecovery ? "recovery" : "removal")
                      : (fSetup ? "setup" : "hold");
    char sBuf[3][16];

    fprintf( pFile, "\n===== %s path (%s, %s) =====\n", pKind,
             fSetup ? "max corner" : "min corner",
             !pCheck->fToRegister ? "to output port"
             : ( MstaInstArrayAt(&pDes->vInsts,pCheck->InstId)->pCell->fLatch
                 ? "to latch" : "to register" ) );
    fprintf( pFile, "startpoint : %s\n", Msta_TimingEndpointName( p, pCheck, 1, fMax ) );
    fprintf( pFile, "endpoint   : %s%s\n", Msta_TimingEndpointName( p, pCheck, 0, fMax ),
             pCheck->fToRegister ? "" : "  (top-level output)" );
    fprintf( pFile, "launch clock : %s @ %.3f ns\n",
             Msta_NameStr( nLaunchClock ), LaunchTime / 1000.0 );
    fprintf( pFile, "capture clock: %s @ %.3f ns\n",
             Msta_NameStr( pCheck->CaptureClock ), CaptureTime / 1000.0 );
    if ( !pCheck->fToRegister )
    {
        int nClock = Msta_SdcClockIndexOf(p->pSdc,pCheck->CaptureClock);
        int nRefNet = Msta_SdcPortReferenceNet(p->pSdc,pCheck->nEndNet,nClock,1,fSetup);
        if ( nRefNet >= 0 )
            fprintf( pFile, "reference pin: %s\n", Msta_NetName(pDes,nRefNet) );
    }
    if ( Msta_SdcPathGroupsUsed( p->pSdc ) )
    {
        MstaId nGroup = pCorner->Group;
        if ( nGroup == MSTA_NO_ID ) nGroup = pCheck->CaptureClock;
        if ( nGroup != MSTA_NO_ID )
            fprintf( pFile, "path group   : %s\n", Msta_NameStr( nGroup ) );
    }

    nCount = Msta_TimingTracePath( p, pCheck, fMax, nNets, MSTA_PATH_MAX );
    fprintf( pFile, "%-44s %10s %10s\n", "point", "inc(ns)", "arr(ns)" );
    fprintf( pFile, "--------------------------------------------------------------\n" );
    /* nNets 里是 终点 -> 起点，打印要反过来 */
    {
        double Prev = 0.0;
        for ( i = nCount - 1; i >= 0; i-- )
        {
            double *pSaved = pCorner->pPathArrival;
            double Arr = ( pSaved ? pSaved[i] : Msta_TimingNetArrival( p, nNets[i], fMax ) )
                       + LaunchShift;
            Msta_PrintPathPoint( p, pFile, nNets[i], fMax, Arr, Prev, -1.0 );
            Prev = Arr;
        }
    }
    fprintf( pFile, "--------------------------------------------------------------\n" );
    fprintf( pFile, "data arrival time                       %10s\n",
             Msta_PeriodText( Arrival, sBuf[0], sizeof(sBuf[0]) ) );
    if ( fSetup )
    {
        fprintf( pFile, "  capture edge                          %10s\n",
                 Msta_PeriodText( CaptureTime, sBuf[1], sizeof(sBuf[1]) ) );
        fprintf( pFile, "  - setup check (%s)                    %10s\n",
                 pCheck->fToRegister ? "from lib" : "output delay",
                 Msta_PeriodText( -pCorner->CheckTime, sBuf[2], sizeof(sBuf[2]) ) );
        fprintf( pFile, "  - clock uncertainty                   %10s\n",
                 Msta_PeriodText( -pCorner->Uncertainty, sBuf[1], sizeof(sBuf[1]) ) );
        if ( pCheck->fToRegister && Msta_IsSet(pCorner->Borrow) &&
             MstaInstArrayAt(&pDes->vInsts,pCheck->InstId)->pCell->fLatch )
            fprintf( pFile, "  (锁存器：开沿起算，max_time_borrow %s ns 取代上面的关闭沿要求)\n",
                     Msta_PeriodText( pCorner->Borrow, sBuf[2], sizeof(sBuf[2]) ) );
    }
    else
    {
        fprintf( pFile, "  hold edge                             %10s\n",
                 Msta_PeriodText( CaptureTime, sBuf[1], sizeof(sBuf[1]) ) );
        fprintf( pFile, "  + %-35s %10s\n",
                 pCheck->fToRegister ? "hold check" : "output delay",
                 Msta_PeriodText( pCorner->CheckTime, sBuf[2], sizeof(sBuf[2]) ) );
        fprintf( pFile, "  + clock uncertainty                   %10s\n",
                 Msta_PeriodText( pCorner->Uncertainty, sBuf[1], sizeof(sBuf[1]) ) );
    }
    fprintf( pFile, "data required time                      %10s\n",
             Msta_PeriodText( Required, sBuf[0], sizeof(sBuf[0]) ) );
    fprintf( pFile, "slack (%s)                               %10s%s\n",
             Slack >= 0.0 ? "MET" : "VIOLATED",
             Msta_PeriodText( Slack, sBuf[2], sizeof(sBuf[2]) ),
             Slack >= 0.0 ? "" : "   <-- 需要修" );
    (void)pDes;
}

void Msta_ReportChecks( MstaTiming *p, FILE *pFile, int nMaxPaths, int fSetup )
{
    int i, n = 0;
    if ( p->vChecks.nSize == 0 )
    {
        fprintf( pFile, "no timing endpoints: is the design flattened and clocks defined?\n" );
        return;
    }
    Msta_TimingSortChecks( p, fSetup );
    for ( i = 0; i < p->vChecks.nSize && n < nMaxPaths; i++ )
    {
        MstaCheck *pCheck = MstaCheckArrayAt( &p->vChecks, i );
        if ( !Msta_CheckCorner( pCheck, fSetup )->fChecked )
            continue;
        n++;
        Msta_PrintOnePath( p, pCheck, fSetup, pFile );
    }
    if ( n == 0 )
        fprintf( pFile, "no %s path can be reported (endpoints without constraint?)\n",
                 fSetup ? "setup" : "hold" );
}

/* 一个路径分组的累计结果。分组只影响报告的组织方式，WNS/TNS 的算法和整体一致。 */
typedef struct {
    int    nChecks;
    double Worst, Total;
} MstaGroupStat;

typedef struct {
    MstaId Name;
    double Weight;
    MstaGroupStat Setup, Hold;
} MstaGroupRow;

/* 找到分组行，没有就按给定权重新建一条；表满了返回 NULL。 */
static MstaGroupRow *Msta_GroupRow( MstaGroupRow *pRows, int *pnRows, int nCap,
                                    MstaId Name, double Weight )
{
    int i;
    MstaGroupRow *pRow;
    for ( i = 0; i < *pnRows; i++ )
        if ( pRows[i].Name == Name )
            return &pRows[i];
    if ( *pnRows >= nCap )
        return NULL;
    pRow = &pRows[(*pnRows)++];
    pRow->Name = Name;
    pRow->Weight = Weight;
    pRow->Setup.nChecks = pRow->Hold.nChecks = 0;
    pRow->Setup.Worst = pRow->Hold.Worst = MSTA_UNSET;
    pRow->Setup.Total = pRow->Hold.Total = 0.0;
    return pRow;
}

static void Msta_PrintGroupStat( FILE *pFile, const MstaGroupStat *pStat )
{
    fprintf( pFile, " %7d", pStat->nChecks );
    if ( Msta_IsSet(pStat->Worst) )
        fprintf( pFile, " %11.3f %11.3f", pStat->Worst/1000.0, pStat->Total/1000.0 );
    else
        fprintf( pFile, " %11s %11s", "-", "-" );
}

/* 按 group_path 分组的 WNS/TNS：没写 group_path 时不打这一节。 */
static void Msta_ReportGroups( MstaTiming *p, FILE *pFile )
{
    MstaSdc *pSdc = p->pSdc;
    int nCap = Msta_SdcPathGroupCount(pSdc) + Msta_SdcClockCount(pSdc) + 4;
    MstaGroupRow *pRows;
    int nRows = 0, i;

    pRows = (MstaGroupRow *)calloc( (size_t)nCap, sizeof(MstaGroupRow) );
    assert( pRows );
    /* 用户写的命名组先按约束里的顺序占好位置，组名在表里就按这个顺序出现。 */
    for ( i = 0; i < Msta_SdcPathGroupCount(pSdc); i++ )
    {
        MstaPathGroup *pGroup = Msta_SdcPathGroupByIndex( pSdc, i );
        MstaId nName = pGroup->fDefault ? Msta_NameId( MSTA_SDC_GROUP_DEFAULT )
                                        : pGroup->Name;
        if ( nName != MSTA_NO_ID )
            Msta_GroupRow( pRows, &nRows, nCap, nName, pGroup->Weight );
    }
    for ( i = 0; i < p->vChecks.nSize; i++ )
    {
        MstaCheck *pCheck = MstaCheckArrayAt( &p->vChecks, i );
        int fSetup;
        for ( fSetup = 1; fSetup >= 0; fSetup-- )
        {
            const MstaCheckCorner *pCorner = Msta_CheckCorner( pCheck, fSetup );
            MstaId nName = ( pCorner->Group != MSTA_NO_ID ) ? pCorner->Group
                                                            : pCheck->CaptureClock;
            MstaGroupRow *pRow;
            MstaGroupStat *pStat;
            if ( !pCorner->fChecked || nName == MSTA_NO_ID )
                continue;
            pRow = Msta_GroupRow( pRows, &nRows, nCap, nName, Msta_SdcPathGroupWeight(pSdc,nName) );
            if ( pRow == NULL )
                continue;
            pStat = fSetup ? &pRow->Setup : &pRow->Hold;
            pStat->nChecks++;
            if ( pCorner->Slack < pStat->Worst ) pStat->Worst = pCorner->Slack;
            if ( pCorner->Slack < 0.0 ) pStat->Total += pCorner->Slack;
        }
    }
    if ( p->nDataChecks > 0 && Msta_IsSet(p->WorstDataCheckSetupSlack) )
    {
        MstaGroupRow *pRow = Msta_GroupRow( pRows, &nRows, nCap,
                                            Msta_NameId(MSTA_REPORT_DATA_GROUP), 1.0 );
        if ( pRow != NULL )
        {
            pRow->Setup.nChecks = p->nDataChecks;
            pRow->Setup.Worst = p->WorstDataCheckSetupSlack;
            if ( p->WorstDataCheckSetupSlack < 0.0 )
                pRow->Setup.Total = p->WorstDataCheckSetupSlack;
        }
    }
    if ( p->nClkGatingChecks > 0 )
    {
        MstaGroupRow *pRow = Msta_GroupRow( pRows, &nRows, nCap,
                                            Msta_NameId(MSTA_REPORT_GATING_GROUP), 1.0 );
        if ( pRow != NULL )
        {
            pRow->Setup.nChecks = pRow->Hold.nChecks = p->nClkGatingChecks;
            pRow->Setup.Worst = p->WorstClkGatingSetupSlack;
            pRow->Hold.Worst  = p->WorstClkGatingHoldSlack;
            if ( Msta_IsSet(p->WorstClkGatingSetupSlack) && p->WorstClkGatingSetupSlack < 0.0 )
                pRow->Setup.Total = p->WorstClkGatingSetupSlack;
            if ( Msta_IsSet(p->WorstClkGatingHoldSlack) && p->WorstClkGatingHoldSlack < 0.0 )
                pRow->Hold.Total = p->WorstClkGatingHoldSlack;
        }
    }

    fprintf( pFile, "---------------- 路径分组 ----------------\n" );
    fprintf( pFile, "%-18s %5s %7s %11s %11s %7s %11s %11s\n", "group", "weight",
             "setup#", "setup WNS", "setup TNS", "hold#", "hold WNS", "hold TNS" );
    for ( i = 0; i < nRows; i++ )
    {
        MstaGroupRow *pRow = &pRows[i];
        fprintf( pFile, "%-18s %5.2f", Msta_NameStr(pRow->Name), pRow->Weight );
        Msta_PrintGroupStat( pFile, &pRow->Setup );
        Msta_PrintGroupStat( pFile, &pRow->Hold );
        fprintf( pFile, "\n" );
    }
    free( pRows );
}

void Msta_ReportSummary( MstaTiming *p, FILE *pFile )
{
    fprintf( pFile, "---------------- 时序汇总 ----------------\n" );
    fprintf( pFile, "寄存器 %d 个   ", p->nRegisters );
    if ( p->nLatches > 0 )
        fprintf( pFile, "锁存器 %d 个   ", p->nLatches );
    fprintf( pFile, "端点 %d 个   未约束 %d 个   路径例外排除 %d 个\n",
             p->nEndpoints, p->nUnconstrainedEnds, p->nExcludedEnds );
    fprintf( pFile, "组合环路 %d 处   时钟网络 %d 根\n",
             p->nCombLoops, Msta_TimingClockNetCount( p ) );
    if ( Msta_SdcHasDrcLimits(p->pSdc) )
    {
        fprintf( pFile, "DRC   : max_transition 违例 %d 处", p->nDrcTransitionViol );
        if ( p->nDrcTransitionViol > 0 )
            fprintf( pFile, "（最差超出 %.3f ns）", p->WorstDrcTransition / 1000.0 );
        fprintf( pFile, "   max_capacitance 违例 %d 处", p->nDrcCapacitanceViol );
        if ( p->nDrcCapacitanceViol > 0 )
            fprintf( pFile, "（最差超出 %.3f fF）", p->WorstDrcCapacitance );
        fprintf( pFile, "   max_fanout 违例 %d 处", p->nDrcFanoutViol );
        if ( p->nDrcFanoutViol > 0 )
            fprintf( pFile, "（最大扇出 %d）", p->WorstDrcFanout );
        fprintf( pFile, "   min_capacitance 违例 %d 处", p->nDrcMinCapacitanceViol );
        if ( p->nDrcMinCapacitanceViol > 0 )
            fprintf( pFile, "（最差低了 %.3f fF）", p->WorstDrcMinCapacitance );
        fprintf( pFile, "\n" );
    }
    if ( Msta_IsSet(p->pSdc->MaxArea) )
    {
        fprintf( pFile, "面积  : %.1f（目标 %.1f，%s %.1f）\n", p->DesignArea,
                 p->pSdc->MaxArea,
                 p->DesignArea > p->pSdc->MaxArea ? "超出" : "余量",
                 p->DesignArea > p->pSdc->MaxArea ? p->DesignArea - p->pSdc->MaxArea
                                                  : p->pSdc->MaxArea - p->DesignArea );
    }
    if ( p->nDataChecks > 0 )
    {
        fprintf( pFile, "数据检查 : %d 条", p->nDataChecks );
        if ( Msta_IsSet(p->WorstDataCheckSetupSlack) )
            fprintf( pFile, "   setup 最差 %.3f ns（%s -> %s，margin %.3f）",
                     p->WorstDataCheckSetupSlack / 1000.0,
                     Msta_NameStr(p->DataCheckSetupFrom), Msta_NameStr(p->DataCheckSetupTo),
                     p->DataCheckSetupValue / 1000.0 );
        fprintf( pFile, "\n" );
    }
    if ( p->nClkGatingChecks > 0 )
    {
        fprintf( pFile, "时钟门控 : %d 条检查", p->nClkGatingChecks );
        if ( Msta_IsSet(p->WorstClkGatingSetupSlack) && p->ClkGatingSetupInst >= 0 )
            fprintf( pFile, "   setup 最差 %.3f ns（%s/%s，检查值 %.3f）",
                     p->WorstClkGatingSetupSlack / 1000.0,
                     Msta_InstName(p->pDes, p->ClkGatingSetupInst),
                     Msta_NameStr(p->ClkGatingSetupPin), p->ClkGatingSetupValue / 1000.0 );
        if ( Msta_IsSet(p->WorstClkGatingHoldSlack) && p->ClkGatingHoldInst >= 0 )
            fprintf( pFile, "   hold 最差 %.3f ns（%s/%s，检查值 %.3f）",
                     p->WorstClkGatingHoldSlack / 1000.0,
                     Msta_InstName(p->pDes, p->ClkGatingHoldInst),
                     Msta_NameStr(p->ClkGatingHoldPin), p->ClkGatingHoldValue / 1000.0 );
        fprintf( pFile, "\n" );
    }
    if ( Msta_SdcOpCondSelected(p->pSdc) )
    {
        const char *pName;
        double vMax = MSTA_UNSET, vMin = MSTA_UNSET, tMax = MSTA_UNSET, tMin = MSTA_UNSET;
        fprintf( pFile, "工艺角   :" );
        Msta_SdcOpCondInfo( p->pSdc, p->pLib, 1, &pName, &vMax, &tMax );
        fprintf( pFile, " late %s", pName ? pName : "(library default)" );
        if ( p->pSdc->OpCondLibraryMax != MSTA_NO_ID )
            fprintf( pFile, " [%s]", Msta_NameStr(p->pSdc->OpCondLibraryMax) );
        if ( Msta_IsSet(vMax) ) fprintf( pFile, "（%.3f V", vMax );
        else                    fprintf( pFile, "（电压未声明" );
        if ( Msta_IsSet(tMax) ) fprintf( pFile, " / %.1f C）", tMax );
        else                    fprintf( pFile, "）" );
        Msta_SdcOpCondInfo( p->pSdc, p->pLib, 0, &pName, &vMin, &tMin );
        fprintf( pFile, "   early %s", pName ? pName : "(library default)" );
        if ( p->pSdc->OpCondLibraryMin != MSTA_NO_ID )
            fprintf( pFile, " [%s]", Msta_NameStr(p->pSdc->OpCondLibraryMin) );
        if ( Msta_IsSet(vMin) ) fprintf( pFile, "（%.3f V", vMin );
        else                    fprintf( pFile, "（电压未声明" );
        if ( Msta_IsSet(tMin) ) fprintf( pFile, " / %.1f C）", tMin );
        else                    fprintf( pFile, "）" );
        fprintf( pFile, "\n" );
    }
    if ( p->WorstSetupSlack > 1e29 )
        fprintf( pFile, "setup : 没有一条可分析的路径\n" );
    else
        fprintf( pFile, "setup : WNS %8.3f ns   TNS %9.3f ns   违例端点 %d / %d\n",
                 p->WorstSetupSlack / 1000.0, p->TotalSetupSlack / 1000.0,
                 p->nSetupViolations, p->nEndpoints );
    if ( p->WorstHoldSlack > 1e29 )
        fprintf( pFile, "hold  : 没有一条可分析的路径\n" );
    else
        fprintf( pFile, "hold  : WNS %8.3f ns   TNS %9.3f ns   违例端点 %d / %d\n",
                 p->WorstHoldSlack / 1000.0, p->TotalHoldSlack / 1000.0,
                 p->nHoldViolations, p->nEndpoints );
    if ( Msta_SdcPathGroupsUsed( p->pSdc ) )
        Msta_ReportGroups( p, pFile );
}

void Msta_ReportClockTree( MstaTiming *p, FILE *pFile )
{
    int n, c, nClocks = Msta_TimingClockCount( p );
    int nReached = 0, nDeepest = 0, nFF = 0;
    double dMax = 0.0;
    MstaDesign *pDes = p->pDes;

    for ( n = 0; n < pDes->vNets.nSize; n++ )
    {
        int fReached = 0;
        if ( !p->pfClockNet[n] )
            continue;
        /* 一根网络可能同时属于多个时钟（create_clock -add），这里取最差的那个。 */
        for ( c = 0; c < nClocks; c++ )
        {
            const MstaClockArr *pArr;
            if ( !Msta_TimingNetHasClock(p,c,n) )
                continue;
            pArr = Msta_TimingClockArr( p, c, n );
            if ( !pArr->fReached )
                continue;
            fReached = 1;
            if ( pArr->nThroughGates > nDeepest )
                nDeepest = pArr->nThroughGates;
            if ( pArr->MaxArrival > dMax )
                dMax = pArr->MaxArrival;
        }
        if ( fReached )
            nReached++;
    }
    fprintf( pFile, "时钟树：网络 %d 根，算出插入延迟的 %d 根，最深 %d 级缓冲，最大插入延迟 %.3f ns%s\n",
             Msta_TimingClockNetCount( p ), nReached, nDeepest, dMax / 1000.0,
             p->fIdealClocks ? "  [理想时钟模式]" : "" );
    /* 同一根网络挂多个时钟时，再按每个时钟各打一行，便于区分。 */
    if ( nClocks > 1 )
        for ( c = 0; c < nClocks; c++ )
        {
            MstaClock *pClock = Msta_SdcClockByIndex( p->pSdc, c );
            int nNets = 0, nClockReached = 0;
            double dClockMax = 0.0;
            for ( n = 0; n < pDes->vNets.nSize; n++ )
            {
                const MstaClockArr *pArr;
                if ( !Msta_TimingNetHasClock(p,c,n) )
                    continue;
                nNets++;
                pArr = Msta_TimingClockArr( p, c, n );
                if ( !pArr->fReached )
                    continue;
                nClockReached++;
                if ( pArr->MaxArrival > dClockMax )
                    dClockMax = pArr->MaxArrival;
            }
            fprintf( pFile, "  时钟 %-10s 网络 %d 根，算出插入延迟的 %d 根，最大插入延迟 %.3f ns\n",
                     Msta_NameStr(pClock->Name), nNets, nClockReached, dClockMax / 1000.0 );
        }
    fprintf( pFile, "说明：时钟延迟按 Liberty timing arc 传播；本工具不检查 ICG 使能是否吞沿。\n" );
    (void)nFF;
}
