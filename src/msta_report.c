/**CFile***************************************************************

  FileName    [msta_report.c]

  Synopsis    [报告打印与教学诊断输出。]

  路径表的排版刻意做得"看得见算式"：每一行给出这一点的到达时间和相对上一点的
  增量，末尾把 required = capture - setup - uncertainty 三项分开列出来。
  额外提供 -tran/-cap/-clock_path 扩展列与 explain_arc/report_net/dump_graph。

***********************************************************************/

#include "msta_report.h"
#include "msta_util.h"
#include <math.h>

#define MSTA_PATH_MAX 4096        /* 单条路径最多打印这么多点（更长的链是异常） */

#define MSTA_REPORT_DATA_GROUP   "**data check**"
#define MSTA_REPORT_GATING_GROUP "**clock gating**"

/* ps 数值格式化成 ns 文本（保留三位小数）。 */
static const char *Msta_NsText( double ps, char *pBuf, int nBuf )
{
    snprintf( pBuf, (size_t)nBuf, "%.3f", ps / 1000.0 );
    return pBuf;
}

static const char *Msta_SenseName( MstaSense Sense )
{
    switch ( Sense )
    {
    case MSTA_SENSE_POSITIVE: return "positive_unate";
    case MSTA_SENSE_NEGATIVE: return "negative_unate";
    case MSTA_SENSE_NONUNATE: return "non_unate";
    default:                  return "unknown";
    }
}

/* 查找驱动输出脚 OutPin 的时序单元对应时钟脚名（供 startpoint 行展示 pin <ClkPin>）。 */
static const char *Msta_SeqClkPinText( MstaCell *pCell, MstaId OutPinName )
{
    int k;
    for ( k = 0; k < pCell->vRegs.nSize; k++ )
    {
        MstaRegCheck *pReg = MstaRegCheckArrayAt( &pCell->vRegs, k );
        if ( pReg->QPin == OutPinName && pReg->ClkPin != MSTA_NO_ID )
            return Msta_NameStr( pReg->ClkPin );
    }
    if ( pCell->vRegs.nSize > 0 && pCell->vRegs.pData[0].ClkPin != MSTA_NO_ID )
        return Msta_NameStr( pCell->vRegs.pData[0].ClkPin );
    return "-";
}

/* 查找数据路径起点的时钟网络与触发器时钟脚本地沿，供 -clock_path 展开 launch 时钟树。 */
static int Msta_FindSeqLaunchClk( MstaTiming *p, int nStartNet, int nClock, int fMax,
                                  int nSourceTag, int *pnClkNet, int *pfLocalRise,
                                  int *pnLaunchInst, int *pnLaunchClkPin )
{
    MstaPinRef *pRef = Msta_TimingNetDriver( p, nStartNet );
    MstaInst *pInst;
    MstaPin *pOut;
    int k;
    if ( pRef == NULL || nClock < 0 )
        return 0;
    pInst = MstaInstArrayAt( &p->pDes->vInsts, pRef->InstId );
    if ( !pInst->pCell->fSequential )
        return 0;
    pOut = MstaPinArrayAt( &pInst->pCell->vPins, pRef->PinId );
    for ( k = 0; k < pInst->pCell->vRegs.nSize; k++ )
    {
        MstaRegCheck *pReg = MstaRegCheckArrayAt( &pInst->pCell->vRegs, k );
        int nClkPin = Msta_CellPinIndexOf( pInst->pCell, pReg->ClkPin );
        int nClkNet = (nClkPin >= 0 && nClkPin < pInst->nPins) ? pInst->pNets[nClkPin] : -1;
        const MstaClockArr *pArr;
        int fDrivesOut = 0;
        if ( nClkNet < 0 || !Msta_TimingNetHasClock(p, nClock, nClkNet) )
            continue;
        if ( Msta_SdcTimingDisabled(p->pSdc, pRef->InstId, pReg->ClkPin, pOut->Name) )
            continue;
        if ( pReg->QPin == pOut->Name && pReg->ClkToQArc != MSTA_NO_ID )
            fDrivesOut = 1;
        else
        {
            int a;
            for ( a = 0; a < pInst->pCell->vArcs.nSize; a++ )
            {
                MstaArc *pArc = MstaArcArrayAt( &pInst->pCell->vArcs, a );
                int fEdge = ( pArc->Type == MSTA_TT_RISE_EDGE || pArc->Type == MSTA_TT_FALL_EDGE ||
                              pArc->Type == MSTA_TT_RISE_BOTH || pArc->Type == MSTA_TT_FALL_BOTH );
                if ( fEdge && pArc->InPin == pReg->ClkPin && pArc->OutPin == pOut->Name )
                {   fDrivesOut = 1; break;   }
            }
        }
        if ( !fDrivesOut )
            continue;
        pArr = Msta_TimingClockArr( p, nClock, nClkNet );
        if ( !pArr->fReached )
            continue;
        if ( (pArr->Tags[pReg->fClkRises] & (1 << nSourceTag)) &&
             Msta_TimingTimeValid(pArr->Arrival[fMax][pReg->fClkRises][nSourceTag]) )
        {
            *pnClkNet = nClkNet;
            *pfLocalRise = pReg->fClkRises;
            if ( pnLaunchInst )   *pnLaunchInst = pRef->InstId;
            if ( pnLaunchClkPin ) *pnLaunchClkPin = nClkPin;
            return 1;
        }
    }
    return 0;
}

static void Msta_PrintPointCols( MstaTiming *p, FILE *pFile, const char *pName,
                                 double IncPs, double ArrPs, int fHasSlew, double SlewPs,
                                 int nCapNet, int fMax, int Flags, const char *pSuffix )
{
    char sInc[16], sArr[16], sTran[16], sCap[16];
    fprintf( pFile, "  %-42s %10s %10s",
             pName,
             Msta_NsText( IncPs, sInc, sizeof(sInc) ),
             Msta_NsText( ArrPs, sArr, sizeof(sArr) ) );
    if ( Flags & MSTA_RPT_TRAN )
    {
        if ( fHasSlew )
            Msta_NsText( SlewPs, sTran, sizeof(sTran) );
        else
            snprintf( sTran, sizeof(sTran), "-" );
        fprintf( pFile, " %10s", sTran );
    }
    if ( Flags & MSTA_RPT_CAP )
    {
        if ( nCapNet >= 0 )
            snprintf( sCap, sizeof(sCap), "%.3f", Msta_TimingNetLoad(p, nCapNet, fMax) );
        else
            snprintf( sCap, sizeof(sCap), "-" );
        fprintf( pFile, " %10s", sCap );
    }
    fprintf( pFile, "   %s\n", pSuffix );
}

/* 打印一条经过追踪的时钟树（source -> ... -> sink），并更新 *pPrevPs。 */
static void Msta_PrintTracedClockPath( MstaTiming *p, FILE *pFile,
                                       const MstaClockPrev *pPts, int nPts,
                                       int fCornerMax, double WaveformBasePs,
                                       double *pPrevPs, int Flags )
{
    MstaDesign *pDes = p->pDes;
    int j;
    for ( j = 0; j < nPts; j++ )
    {
        MstaClockPrev Pt = pPts[j];
        const MstaClockArr *pArr = Msta_TimingClockArr( p, Pt.Clock, Pt.Net );
        double ArrPs = pArr->Arrival[fCornerMax][Pt.Edge][Pt.Source] + WaveformBasePs;
        double SlewPs = pArr->Slew[fCornerMax][Pt.Edge][Pt.Source];
        if ( Pt.Inst < 0 )
        {
            const char *pTag = (j == 0) ? "(clock source)" : "(clock pin)";
            Msta_PrintPointCols( p, pFile, Msta_NetName(pDes, Pt.Net),
                                 ArrPs - *pPrevPs, ArrPs, 1, SlewPs, Pt.Net,
                                 fCornerMax, Flags, pTag );
        }
        else
        {
            MstaInst *pInst = MstaInstArrayAt( &pDes->vInsts, Pt.Inst );
            MstaPinRef *pDrv = Msta_TimingNetDriver( p, Pt.Net );
            const char *pOutPin = pDrv
                ? Msta_NameStr( MstaPinArrayAt(&pInst->pCell->vPins, pDrv->PinId)->Name )
                : "Y";
            const char *pInPin = (Pt.Pin >= 0 && Pt.Pin < pInst->pCell->vPins.nSize)
                ? Msta_NameStr( MstaPinArrayAt(&pInst->pCell->vPins, Pt.Pin)->Name )
                : "-";
            char sPt[256], sSuffix[160];
            snprintf( sPt, sizeof(sPt), "%s/%s", Msta_InstName(pDes, Pt.Inst), pOutPin );
            snprintf( sSuffix, sizeof(sSuffix), "%s  pin %s",
                      Msta_NameStr(pInst->pCell->Name), pInPin );
            Msta_PrintPointCols( p, pFile, sPt,
                                 ArrPs - *pPrevPs, ArrPs, 1, SlewPs, Pt.Net,
                                 fCornerMax, Flags, sSuffix );
        }
        *pPrevPs = ArrPs;
    }
}

/* 打印路径上的一点；Flags==0 时保持与既有端口/单元行格式完全一致。 */
static void Msta_PrintPathPoint( MstaTiming *p, FILE *pFile, int nNet,
                                 double Arrival, double PrevArrival,
                                 int fMax, int Edge, int nLoadPin, int Flags )
{
    MstaDesign *pDes = p->pDes;
    MstaNet *pNet = MstaNetArrayAt( &pDes->vNets, nNet );
    MstaPinRef *pRef = Msta_TimingNetDriver( p, nNet );
    double SlewPs = (Flags & MSTA_RPT_TRAN) ? Msta_TimingNetSlewEdge(p, nNet, fMax, Edge) : 0.0;

    if ( pRef == NULL )
    {
        char sSuffix[64];
        snprintf( sSuffix, sizeof(sSuffix), "(input port %s)", pNet->fTopPort ? "" : "[undriven]" );
        Msta_PrintPointCols( p, pFile, Msta_NetName(pDes, nNet),
                             Arrival - PrevArrival, Arrival, 1, SlewPs, nNet,
                             fMax, Flags, sSuffix );
        return;
    }
    {
        MstaInst *pInst = MstaInstArrayAt( &pDes->vInsts, pRef->InstId );
        MstaPin *pPin = MstaPinArrayAt( &pInst->pCell->vPins, pRef->PinId );
        const char *pInPinStr = "-";
        char sPoint[256], sSuffix[160];
        if ( nLoadPin >= 0 && nLoadPin < pInst->pCell->vPins.nSize )
            pInPinStr = Msta_NameStr( MstaPinArrayAt(&pInst->pCell->vPins, nLoadPin)->Name );
        else if ( pInst->pCell->fSequential )
            pInPinStr = Msta_SeqClkPinText( pInst->pCell, pPin->Name );

        snprintf( sPoint, sizeof(sPoint), "%s/%s",
                  Msta_InstName(pDes, pRef->InstId), Msta_NameStr(pPin->Name) );
        snprintf( sSuffix, sizeof(sSuffix), "%s  pin %s",
                  Msta_NameStr(pInst->pCell->Name), pInPinStr );
        Msta_PrintPointCols( p, pFile, sPoint,
                             Arrival - PrevArrival, Arrival, 1, SlewPs, nNet,
                             fMax, Flags, sSuffix );
    }
}

static void Msta_PrintOnePath( MstaTiming *p, MstaCheck *pCheck, int fSetup,
                               FILE *pFile, int Flags )
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
    double LaunchShift = pCorner->LaunchShift;
    const char *pKind = pCheck->fAsync
                      ? (pCheck->fRecovery ? "recovery" : "removal")
                      : (fSetup ? "setup" : "hold");
    char sBuf[3][16];
    const char *pSep = (Flags & (MSTA_RPT_TRAN | MSTA_RPT_CAP))
        ? "--------------------------------------------------------------------------------------"
        : "--------------------------------------------------------------";

    fprintf( pFile, "\n===== %s path (%s, %s) =====\n", pKind,
             fSetup ? "max corner" : "min corner",
             !pCheck->fToRegister ? "to output port"
             : ( MstaInstArrayAt(&pDes->vInsts, pCheck->InstId)->pCell->fLatch
                 ? "to latch" : "to register" ) );
    fprintf( pFile, "startpoint : %s\n", Msta_TimingEndpointName( p, pCheck, 1, fMax ) );
    fprintf( pFile, "endpoint   : %s%s\n", Msta_TimingEndpointName( p, pCheck, 0, fMax ),
             pCheck->fToRegister ? "" : "  (top-level output)" );
    fprintf( pFile, "data edge    : %s (%s)\n", pCorner->fDataRise ? "rise" : "fall",
             pCorner->fDataRise ? "^" : "v" );
    fprintf( pFile, "launch clock : %s @ %.3f ns\n",
             Msta_NameStr( nLaunchClock ), LaunchTime / 1000.0 );
    fprintf( pFile, "launch source edge : %s\n", pCorner->LaunchSourceRise ? "rise" : "fall" );
    fprintf( pFile, "capture source edge: %s\n", pCorner->CaptureSourceRise ? "rise" : "fall" );
    MstaId nCapClock = ( pCorner->CaptureClock != MSTA_NO_ID ) ? pCorner->CaptureClock
                                                               : pCheck->CaptureClock;
    fprintf( pFile, "capture clock: %s @ %.3f ns\n",
             Msta_NameStr( nCapClock ), CaptureTime / 1000.0 );
    if ( !pCheck->fToRegister )
    {
        int nClock = Msta_SdcClockIndexOf( p->pSdc, nCapClock );
        int nRefNet = Msta_SdcPortReferenceNet( p->pSdc, pCheck->nEndNet, nClock, 1, fSetup );
        if ( nRefNet >= 0 )
            fprintf( pFile, "reference pin: %s\n", Msta_NetName(pDes, nRefNet) );
    }
    if ( Msta_SdcPathGroupsUsed( p->pSdc ) )
    {
        MstaId nGroup = pCorner->Group;
        if ( nGroup == MSTA_NO_ID ) nGroup = nCapClock;
        if ( nGroup != MSTA_NO_ID )
            fprintf( pFile, "path group   : %s\n", Msta_NameStr( nGroup ) );
    }

    nCount = Msta_TimingTracePath( p, pCheck, fMax, nNets, MSTA_PATH_MAX );
    fprintf( pFile, "%-44s %10s %10s", "point", "inc(ns)", "arr(ns)" );
    if ( Flags & MSTA_RPT_TRAN ) fprintf( pFile, " %10s", "tran(ns)" );
    if ( Flags & MSTA_RPT_CAP )  fprintf( pFile, " %10s", "cap(fF)" );
    fprintf( pFile, "\n%s\n", pSep );

    {
        double Prev = 0.0;
        if ( (Flags & MSTA_RPT_CLOCK_PATH) && nCount > 0 )
        {
            int nLaunchIdx = Msta_SdcClockIndexOf( p->pSdc, nLaunchClock );
            MstaClock *pClkObj = nLaunchIdx >= 0 ? Msta_SdcClockByIndex(p->pSdc, nLaunchIdx) : NULL;
            int nClkNet = -1, fLocalRise = 1, fTraced = 0, nLaunchInst = -1, nLaunchClkPin = -1;
            if ( pClkObj != NULL && pClkObj->fPropagated &&
                 Msta_FindSeqLaunchClk(p, nNets[nCount - 1], nLaunchIdx, fMax,
                                       pCorner->LaunchSourceRise, &nClkNet, &fLocalRise,
                                       &nLaunchInst, &nLaunchClkPin) )
            {
                MstaClockPrev vClkPts[256];
                int nPts = Msta_TimingTraceClock( p, nLaunchIdx, nClkNet, fMax,
                                                  fLocalRise, pCorner->LaunchSourceRise,
                                                  vClkPts, 256 );
                if ( nPts > 0 )
                {
                    const MstaClockArr *pSinkArr = Msta_TimingClockArr( p, nLaunchIdx, nClkNet );
                    double SinkDelay = pSinkArr->Arrival[fMax][fLocalRise][pCorner->LaunchSourceRise];
                    double SinkDrvSlew = pSinkArr->Slew[fMax][fLocalRise][pCorner->LaunchSourceRise];
                    double ClkWireD = 0.0, ClkPinSlew = SinkDrvSlew;
                    double WaveBase;
                    char sClkOrigin[96];
                    Msta_TimingPinWireDelay( p, nClkNet, nLaunchInst, nLaunchClkPin,
                                             fMax, fLocalRise, 1, SinkDrvSlew,
                                             &ClkWireD, &ClkPinSlew );
                    WaveBase = LaunchTime - SinkDelay - ClkWireD;
                    snprintf( sClkOrigin, sizeof(sClkOrigin), "clock %s (%s edge)",
                              Msta_NameStr(nLaunchClock),
                              pCorner->LaunchSourceRise ? "rise" : "fall" );
                    Prev = WaveBase;
                    Msta_PrintPointCols( p, pFile, sClkOrigin, 0.0, WaveBase, 0, 0.0, -1,
                                         fMax, Flags, "(launch clock)" );
                    Msta_PrintTracedClockPath( p, pFile, vClkPts, nPts, fMax,
                                               WaveBase, &Prev, Flags );
                    if ( fabs(ClkWireD) > 1e-6 && nLaunchInst >= 0 && nLaunchClkPin >= 0 )
                    {
                        MstaInst *pLInst = MstaInstArrayAt( &pDes->vInsts, nLaunchInst );
                        MstaPin *pLPin = MstaPinArrayAt( &pLInst->pCell->vPins, nLaunchClkPin );
                        char sWirePt[256];
                        snprintf( sWirePt, sizeof(sWirePt), "%s/%s",
                                  Msta_InstName(pDes, nLaunchInst), Msta_NameStr(pLPin->Name) );
                        Msta_PrintPointCols( p, pFile, sWirePt, LaunchTime - Prev, LaunchTime,
                                             1, ClkPinSlew, -1, fMax, Flags, "(wire)" );
                        Prev = LaunchTime;
                    }
                    fTraced = 1;
                }
            }
            if ( !fTraced )
            {
                char sClkOrigin[96];
                snprintf( sClkOrigin, sizeof(sClkOrigin), "clock %s (%s)",
                          Msta_NameStr(nLaunchClock),
                          (pClkObj && pClkObj->fPropagated) ? "propagated" : "ideal" );
                Msta_PrintPointCols( p, pFile, sClkOrigin, LaunchTime - Prev, LaunchTime,
                                     0, 0.0, -1, fMax, Flags, "(launch clock)" );
                Prev = LaunchTime;
            }
        }
        for ( i = nCount - 1; i >= 0; i-- )
        {
            double *pSaved = pCorner->pPathArrival;
            double Arr = ( pSaved ? pSaved[i] : Msta_TimingNetArrival(p, nNets[i], fMax) )
                       + LaunchShift;
            int Edge = pCorner->pPathEdge ? pCorner->pPathEdge[i] : pCorner->fDataRise;
            int nLoadPin = pCorner->pPathLoadPin ? pCorner->pPathLoadPin[i] : -1;
            Msta_PrintPathPoint( p, pFile, nNets[i], Arr, Prev, fMax, Edge, nLoadPin, Flags );
            if ( p->pSdc->fAocv && p->pSdc->vAocv.nSize &&
                 MstaNetArrayAt( &pDes->vNets, nNets[i] )->Driver.InstId >= 0 )
            {
                int nInNet = i + 1 < nCount ? nNets[i + 1] : -1;
                fprintf( pFile, "    AOCV structural depth: %d\n",
                         Msta_TimingAocvDataDepth( p, nInNet, nNets[i] ) );
            }
            Prev = Arr;
        }
        if ( nCount > 0 && pCheck->fToRegister && fabs(Arrival - Prev) > 1e-6 )
        {
            MstaInst *pEndInst = MstaInstArrayAt( &pDes->vInsts, pCheck->InstId );
            MstaPin *pDPin = MstaPinArrayAt( &pEndInst->pCell->vPins, pCheck->nDataPin );
            double EndDrvSlew = Msta_TimingNetSlewEdge( p, pCheck->nEndNet, fMax, pCorner->fDataRise );
            double EndWireD = 0.0, EndPinSlew = EndDrvSlew;
            char sWirePt[256];
            Msta_TimingPinWireDelay( p, pCheck->nEndNet, pCheck->InstId, pCheck->nDataPin,
                                     fMax, pCorner->fDataRise, 0, EndDrvSlew,
                                     &EndWireD, &EndPinSlew );
            snprintf( sWirePt, sizeof(sWirePt), "%s/%s",
                      Msta_InstName(pDes, pCheck->InstId), Msta_NameStr(pDPin->Name) );
            Msta_PrintPointCols( p, pFile, sWirePt, Arrival - Prev, Arrival,
                                 1, EndPinSlew, -1, fMax, Flags, "(wire)" );
            Prev = Arrival;
        }
    }
    fprintf( pFile, "%s\n", pSep );
    fprintf( pFile, "data arrival time                       %10s\n",
             Msta_NsText( Arrival, sBuf[0], sizeof(sBuf[0]) ) );

    if ( (Flags & MSTA_RPT_CLOCK_PATH) && !pCorner->fPathBudget )
    {
        int nCapIdx = Msta_SdcClockIndexOf( p->pSdc, nCapClock );
        MstaClock *pCapClk = nCapIdx >= 0 ? Msta_SdcClockByIndex(p->pSdc, nCapIdx) : NULL;
        int nCapNet = -1, fLocalCap = 1, nPinIdx = -1;
        if ( pCheck->fToRegister )
        {
            MstaInst *pEndInst = MstaInstArrayAt( &pDes->vInsts, pCheck->InstId );
            MstaId ClkPin = MSTA_NO_ID;
            if ( pCheck->fAsync )
            {
                MstaAsyncCheck *pA = MstaAsyncCheckArrayAt(&pEndInst->pCell->vAsync, pCheck->nAsyncCheck);
                ClkPin = pA->ClkPin;
                fLocalCap = pA->fClkRises;
            }
            else if ( pEndInst->pCell->vRegs.nSize > pCheck->nCheck )
            {
                MstaRegCheck *pR = MstaRegCheckArrayAt(&pEndInst->pCell->vRegs, pCheck->nCheck);
                ClkPin = pR->ClkPin;
                fLocalCap = pR->fClkRises;
            }
            if ( ClkPin != MSTA_NO_ID )
            {
                nPinIdx = Msta_CellPinIndexOf( pEndInst->pCell, ClkPin );
                if ( nPinIdx >= 0 && nPinIdx < pEndInst->nPins )
                    nCapNet = pEndInst->pNets[nPinIdx];
            }
        }
        else if ( nCapIdx >= 0 )
        {
            nCapNet = Msta_SdcPortReferenceNet( p->pSdc, pCheck->nEndNet, nCapIdx, 1, fSetup );
            fLocalCap = !Msta_SdcPortClockFall( p->pSdc, pCheck->nEndNet, nCapIdx, 1, fSetup );
        }
        if ( pCapClk != NULL && pCapClk->fPropagated && nCapNet >= 0 )
        {
            int nCapCorner = Msta_SdcCaptureCorner( p->pSdc, fSetup );
            MstaClockPrev vCapPts[256];
            int nCapPts = Msta_TimingTraceClock( p, nCapIdx, nCapNet, nCapCorner,
                                                 fLocalCap, pCorner->CaptureSourceRise,
                                                 vCapPts, 256 );
            if ( nCapPts > 0 )
            {
                const MstaClockArr *pSinkArr = Msta_TimingClockArr( p, nCapIdx, nCapNet );
                double SinkDelay = pSinkArr->Arrival[nCapCorner][fLocalCap][pCorner->CaptureSourceRise];
                double SinkDrvSlew = pSinkArr->Slew[nCapCorner][fLocalCap][pCorner->CaptureSourceRise];
                double CapClkWireD = 0.0, CapClkPinSlew = SinkDrvSlew;
                double CapBase, CapPrev;
                char sCapOrigin[96];
                if ( pCheck->fToRegister && nPinIdx >= 0 )
                    Msta_TimingPinWireDelay( p, nCapNet, pCheck->InstId, nPinIdx,
                                             nCapCorner, fLocalCap, 1, SinkDrvSlew,
                                             &CapClkWireD, &CapClkPinSlew );
                CapBase = CaptureTime - SinkDelay - CapClkWireD;
                CapPrev = CapBase;
                snprintf( sCapOrigin, sizeof(sCapOrigin), "clock %s (%s edge)",
                          Msta_NameStr(nCapClock),
                          pCorner->CaptureSourceRise ? "rise" : "fall" );
                Msta_PrintPointCols( p, pFile, sCapOrigin, 0.0, CapBase, 0, 0.0, -1,
                                     nCapCorner, Flags, "(capture clock)" );
                Msta_PrintTracedClockPath( p, pFile, vCapPts, nCapPts, nCapCorner,
                                           CapBase, &CapPrev, Flags );
                if ( pCheck->fToRegister && fabs(CapClkWireD) > 1e-6 && nPinIdx >= 0 )
                {
                    MstaInst *pEndInst = MstaInstArrayAt( &pDes->vInsts, pCheck->InstId );
                    MstaPin *pCPin = MstaPinArrayAt( &pEndInst->pCell->vPins, nPinIdx );
                    char sWirePt[256];
                    snprintf( sWirePt, sizeof(sWirePt), "%s/%s",
                              Msta_InstName(pDes, pCheck->InstId), Msta_NameStr(pCPin->Name) );
                    Msta_PrintPointCols( p, pFile, sWirePt, CaptureTime - CapPrev, CaptureTime,
                                         1, CapClkPinSlew, -1, nCapCorner, Flags, "(wire)" );
                    CapPrev = CaptureTime;
                }
            }
        }
    }

    if ( pCorner->fPathBudget )
    {
        fprintf( pFile, "  launch edge                           %10s\n",
                 Msta_NsText( LaunchTime, sBuf[1], sizeof(sBuf[1]) ) );
        fprintf( pFile, "  + %-35s %10s\n", fSetup ? "max_delay" : "min_delay",
                 Msta_NsText( pCorner->CheckTime, sBuf[2], sizeof(sBuf[2]) ) );
    }
    else if ( fSetup )
    {
        fprintf( pFile, "  capture edge                          %10s\n",
                 Msta_NsText( CaptureTime, sBuf[1], sizeof(sBuf[1]) ) );
        fprintf( pFile, "  - setup check (%s)                    %10s\n",
                 pCheck->fToRegister ? "from lib" : "output delay",
                 Msta_NsText( -pCorner->CheckTime, sBuf[2], sizeof(sBuf[2]) ) );
        fprintf( pFile, "  - clock uncertainty                   %10s\n",
                 Msta_NsText( -pCorner->Uncertainty, sBuf[1], sizeof(sBuf[1]) ) );
        if ( pCheck->fToRegister && Msta_IsSet(pCorner->Borrow) &&
             MstaInstArrayAt(&pDes->vInsts, pCheck->InstId)->pCell->fLatch )
            fprintf( pFile, "  (锁存器：开沿起算，max_time_borrow %s ns 取代上面的关闭沿要求)\n",
                     Msta_NsText( pCorner->Borrow, sBuf[2], sizeof(sBuf[2]) ) );
    }
    else
    {
        fprintf( pFile, "  hold edge                             %10s\n",
                 Msta_NsText( CaptureTime, sBuf[1], sizeof(sBuf[1]) ) );
        fprintf( pFile, "  + %-35s %10s\n",
                 pCheck->fToRegister ? "hold check" : "output delay",
                 Msta_NsText( pCorner->CheckTime, sBuf[2], sizeof(sBuf[2]) ) );
        fprintf( pFile, "  + clock uncertainty                   %10s\n",
                 Msta_NsText( pCorner->Uncertainty, sBuf[1], sizeof(sBuf[1]) ) );
    }
    if ( p->pSdc->fCppr )
        fprintf( pFile, "  %s clock reconvergence pessimism       %10s\n", fSetup ? "+" : "-",
                 Msta_NsText( fSetup ? pCorner->Cppr : -pCorner->Cppr, sBuf[1], sizeof(sBuf[1]) ) );
    fprintf( pFile, "data required time                      %10s\n",
             Msta_NsText( Required, sBuf[0], sizeof(sBuf[0]) ) );
    fprintf( pFile, "slack (%s)                               %10s%s\n",
             Slack >= 0.0 ? "MET" : "VIOLATED",
             Msta_NsText( Slack, sBuf[2], sizeof(sBuf[2]) ),
             Slack >= 0.0 ? "" : "   <-- 需要修" );
}

void Msta_ReportChecksEx( MstaTiming *p, FILE *pFile, int nMaxPaths, int fSetup, int Flags )
{
    int i, n = 0;
    if ( p->vChecks.nSize == 0 )
    {
        fprintf( pFile, "没有时序端点：设计展平了吗？时钟定义了吗？\n" );
        return;
    }
    Msta_TimingSortChecks( p, fSetup );
    for ( i = 0; i < p->vChecks.nSize && n < nMaxPaths; i++ )
    {
        MstaCheck *pCheck = MstaCheckArrayAt( &p->vChecks, i );
        if ( !Msta_CheckCorner( pCheck, fSetup )->fChecked )
            continue;
        n++;
        Msta_PrintOnePath( p, pCheck, fSetup, pFile, Flags );
    }
    if ( n == 0 )
        fprintf( pFile, "没有可报告的 %s 路径（端点都没有约束？）\n",
                 fSetup ? "setup" : "hold" );
}

void Msta_ReportChecks( MstaTiming *p, FILE *pFile, int nMaxPaths, int fSetup )
{
    Msta_ReportChecksEx( p, pFile, nMaxPaths, fSetup, 0 );
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

static void Msta_ReportGroups( MstaTiming *p, FILE *pFile )
{
    MstaSdc *pSdc = p->pSdc;
    int nCap = Msta_SdcPathGroupCount(pSdc) + Msta_SdcClockCount(pSdc) + 4;
    MstaGroupRow *pRows;
    int nRows = 0, i;

    pRows = (MstaGroupRow *)calloc( (size_t)nCap, sizeof(MstaGroupRow) );
    assert( pRows );
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
            MstaId nCapClock = ( pCorner->CaptureClock != MSTA_NO_ID ) ? pCorner->CaptureClock
                                                                       : pCheck->CaptureClock;
            MstaId nName = ( pCorner->Group != MSTA_NO_ID ) ? pCorner->Group
                                                            : nCapClock;
            MstaGroupRow *pRow;
            MstaGroupStat *pStat;
            if ( !pCorner->fChecked || nName == MSTA_NO_ID )
                continue;
            pRow = Msta_GroupRow( pRows, &nRows, nCap, nName, Msta_SdcPathGroupWeight(pSdc, nName) );
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
    if ( p->pSdc->fCppr )
        fprintf( pFile, "CPPR     : enabled (%s), launch contexts %d\n",
                 p->pSdc->fCpprSamePin ? "same_pin" : "same_transition", p->nStartClasses );
    if ( p->pSdc->vAocv.nSize )
        fprintf( pFile, "AOCV     : depth GBA, %s (%d tables)\n",
                 p->pSdc->fAocv ? "enabled" : "disabled", p->pSdc->vAocv.nSize );
    if ( p->pSdc->fAnalysisSelected || p->pSdc->vObjDerate.nSize )
        fprintf( pFile, "分析模式 : %s\n", p->pSdc->AnalysisType == MSTA_ANALYSIS_OCV ? "on_chip_variation" :
                 p->pSdc->AnalysisType == MSTA_ANALYSIS_SINGLE ? "single" : "bc_wc" );
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
    int nReached = 0, nDeepest = 0;
    double dMax = 0.0;
    MstaDesign *pDes = p->pDes;

    for ( n = 0; n < pDes->vNets.nSize; n++ )
    {
        int fReached = 0;
        if ( !p->pfClockNet[n] )
            continue;
        for ( c = 0; c < nClocks; c++ )
        {
            const MstaClockArr *pArr;
            if ( !Msta_TimingNetHasClock(p, c, n) )
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
    fprintf( pFile, "时钟树：网络 %d 根，算出插入延迟的 %d 根，最深 %d 级缓冲，最大插入延迟 %.3f ns\n",
             Msta_TimingClockNetCount( p ), nReached, nDeepest, dMax / 1000.0 );
    if ( nClocks > 1 )
        for ( c = 0; c < nClocks; c++ )
        {
            MstaClock *pClock = Msta_SdcClockByIndex( p->pSdc, c );
            int nNets = 0, nClockReached = 0;
            double dClockMax = 0.0;
            for ( n = 0; n < pDes->vNets.nSize; n++ )
            {
                const MstaClockArr *pArr;
                if ( !Msta_TimingNetHasClock(p, c, n) )
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
}

/* =====================================================================
   教学诊断命令 1：explain_arc
   ===================================================================== */

static void Msta_PrintTableBreakdown( FILE *pFile, const char *pLabel,
                                      const MstaTable *pTable, double In1, double In2,
                                      int fHasScale, int fIsCheck,
                                      double Derate, double Aocv, int Depth )
{
    MstaTableExplain Exp;
    const char *pU0, *pU1;
    double Final;
    if ( !Msta_TableExplain( pTable, In1, In2, &Exp ) )
        return;
    pU0 = ( Exp.Axis[0] == MSTA_AXIS_LOAD ) ? "fF" : "ps";
    pU1 = ( Exp.Axis[1] == MSTA_AXIS_LOAD ) ? "fF" : "ps";
    fprintf( pFile, "  --- %s ---\n", pLabel );
    fprintf( pFile, "    table        : %s (nDim = %d)\n",
             pTable->sTag[0] ? pTable->sTag : "(unnamed)", Exp.nDim );
    fprintf( pFile, "    axis[0] row  : %s = %.6f %s -> bracket [%.6f, %.6f], iRow = %d, wRow = %.6f\n",
             Msta_LibAxisName(Exp.Axis[0]), Exp.RowInput, pU0,
             Exp.Row0, Exp.Row1, Exp.iRow, Exp.wRow );
    fprintf( pFile, "    axis[1] col  : %s = %.6f %s -> bracket [%.6f, %.6f], iCol = %d, wCol = %.6f\n",
             Msta_LibAxisName(Exp.Axis[1]), Exp.ColInput, pU1,
             Exp.Col0, Exp.Col1, Exp.iCol, Exp.wCol );
    fprintf( pFile, "    grid 2x2     : V00 = %.6f, V01 = %.6f, V10 = %.6f, V11 = %.6f ps\n",
             Exp.V00, Exp.V01, Exp.V10, Exp.V11 );
    fprintf( pFile, "    nldm raw     : %.6f ps\n", Exp.Result );
    if ( fHasScale )
    {
        if ( fIsCheck )
        {
            Final = Exp.Result * Derate;
            fprintf( pFile, "    scale        : ocv_check_derate = %.6f\n", Derate );
            fprintf( pFile, "    final check  : %.6f ps (%.6f ns, report %.3f ns)\n",
                     Final, Final / 1000.0, Final / 1000.0 );
        }
        else
        {
            Final = Exp.Result * Derate * Aocv;
            fprintf( pFile, "    scale        : ocv_derate = %.6f, aocv_factor = %.6f (depth %d)\n",
                     Derate, Aocv, Depth );
            fprintf( pFile, "    final delay  : %.6f ps (%.6f ns, report %.3f ns)\n",
                     Final, Final / 1000.0, Final / 1000.0 );
        }
    }
}

static int Msta_ArcMatchesTimingType( const MstaArc *pArc, int *pfIsConstraint, int *pfSetupLike )
{
    MstaTimingType t = pArc->Type;
    if ( t == MSTA_TT_SETUP_RISING || t == MSTA_TT_SETUP_FALLING ||
         t == MSTA_TT_RECOVERY_RISING || t == MSTA_TT_RECOVERY_FALLING )
    {
        *pfIsConstraint = 1;
        *pfSetupLike = 1;
        return 1;
    }
    if ( t == MSTA_TT_HOLD_RISING || t == MSTA_TT_HOLD_FALLING ||
         t == MSTA_TT_REMOVAL_RISING || t == MSTA_TT_REMOVAL_FALLING )
    {
        *pfIsConstraint = 1;
        *pfSetupLike = 0;
        return 1;
    }
    *pfIsConstraint = 0;
    *pfSetupLike = 1;
    return 1;
}

int Msta_ReportExplainArc( MstaTiming *p, FILE *pFile,
                           const char *pInstName, const char *pInPin,
                           const char *pOutPin, int fMax )
{
    MstaDesign *pDes = p->pDes;
    int nInst = Msta_DesignFindInstByName( pDes, pInstName );
    MstaInst *pInst;
    MstaId InPinId, OutPinId;
    int nInPin, nOutPin, nInNet, nOutNet, i, a, nMatched = 0;
    MstaId CornerLib;
    MstaCell *pCornerCell, *pEvalCell;
    double DrvSlewR, DrvSlewF, InWireR = 0.0, InWireF = 0.0, InSlewR, InSlewF;
    double RealLoad, LookupLoad;

    if ( nInst < 0 )
    {
        Msta_Error( "explain_arc：找不到实例 \"%s\"\n", pInstName );
        return 0;
    }
    pInst = MstaInstArrayAt( &pDes->vInsts, nInst );
    InPinId = Msta_NameId( pInPin );
    OutPinId = Msta_NameId( pOutPin );
    nInPin = Msta_CellPinIndexOf( pInst->pCell, InPinId );
    nOutPin = Msta_CellPinIndexOf( pInst->pCell, OutPinId );
    if ( nInPin < 0 )
    {
        Msta_Error( "explain_arc：实例 \"%s\" 没有输入引脚 \"%s\"\n", pInstName, pInPin );
        return 0;
    }
    if ( nOutPin < 0 )
    {
        Msta_Error( "explain_arc：实例 \"%s\" 没有输出引脚 \"%s\"\n", pInstName, pOutPin );
        return 0;
    }
    nInNet = nInPin < pInst->nPins ? pInst->pNets[nInPin] : -1;
    nOutNet = nOutPin < pInst->nPins ? pInst->pNets[nOutPin] : -1;

    CornerLib = Msta_SdcOperatingLibrary( p->pSdc, fMax );
    pCornerCell = Msta_LibCellForCorner( p->pLib, pInst->pCell, CornerLib );
    pEvalCell = pCornerCell;
    if ( pCornerCell != pInst->pCell )
    {
        int fHasCornerArc = 0;
        for ( a = 0; a < pCornerCell->vArcs.nSize; a++ )
        {
            MstaArc *pArc = MstaArcArrayAt( &pCornerCell->vArcs, a );
            if ( pArc->InPin == InPinId && pArc->OutPin == OutPinId )
            {   fHasCornerArc = 1; break;   }
        }
        if ( !fHasCornerArc )
            pEvalCell = pInst->pCell;
    }

    DrvSlewR = (nInNet >= 0) ? Msta_TimingNetSlewEdge(p, nInNet, fMax, 1) : 50.0;
    DrvSlewF = (nInNet >= 0) ? Msta_TimingNetSlewEdge(p, nInNet, fMax, 0) : 50.0;
    InSlewR = DrvSlewR;
    InSlewF = DrvSlewF;
    if ( nInNet >= 0 )
    {
        int fInClk = ( p->pfClockNet && p->pfClockNet[nInNet] );
        Msta_TimingPinWireDelay( p, nInNet, nInst, nInPin, fMax, 1, fInClk,
                                 DrvSlewR, &InWireR, &InSlewR );
        Msta_TimingPinWireDelay( p, nInNet, nInst, nInPin, fMax, 0, fInClk,
                                 DrvSlewF, &InWireF, &InSlewF );
    }
    RealLoad = (nOutNet >= 0) ? Msta_TimingNetLoad(p, nOutNet, fMax) : 0.0;
    LookupLoad = (RealLoad > 0.0) ? RealLoad : 5.0;

    fprintf( pFile, "\n===== explain_arc: %s (%s) %s -> %s [%s corner] =====\n",
             pInstName, Msta_NameStr(pEvalCell->Name), pInPin, pOutPin,
             fMax ? "max" : "min" );
    if ( pEvalCell->LibName != MSTA_NO_ID )
        fprintf( pFile, "  library      : %s\n", Msta_NameStr(pEvalCell->LibName) );
    fprintf( pFile, "  input net    : %s (slew rise = %.6f ps [%.3f ns], fall = %.6f ps [%.3f ns])\n",
             nInNet >= 0 ? Msta_NetName(pDes, nInNet) : "(unconnected)",
             DrvSlewR, DrvSlewR / 1000.0, DrvSlewF, DrvSlewF / 1000.0 );
    if ( fabs(InWireR) > 1e-9 || fabs(InWireF) > 1e-9 ||
         fabs(InSlewR - DrvSlewR) > 1e-9 || fabs(InSlewF - DrvSlewF) > 1e-9 )
        fprintf( pFile, "  input wire   : delay rise = %.6f ps (%.3f ns), fall = %.6f ps (%.3f ns); pin slew rise = %.6f ps [%.3f ns], fall = %.6f ps [%.3f ns]\n",
                 InWireR, InWireR / 1000.0, InWireF, InWireF / 1000.0,
                 InSlewR, InSlewR / 1000.0, InSlewF, InSlewF / 1000.0 );
    if ( nOutNet >= 0 )
    {
        MstaNet *pOutNetObj = MstaNetArrayAt( &pDes->vNets, nOutNet );
        MstaNetCons *pCons = Msta_SdcNetCons( p->pSdc, nOutNet );
        fprintf( pFile, "  output net   : %s (loads = %d, total = %.6f fF, lookup_load = %.6f fF%s)\n",
                 Msta_NetName(pDes, nOutNet), pOutNetObj->vLoads.nSize, RealLoad, LookupLoad,
                 RealLoad > 0.0 ? "" : " [default fallback]" );
        for ( i = 0; i < pOutNetObj->vLoads.nSize && i < 16; i++ )
        {
            MstaPinRef *pRef = MstaPinRefArrayAt( &pOutNetObj->vLoads, i );
            MstaInst *pLoad = MstaInstArrayAt( &pDes->vInsts, pRef->InstId );
            MstaPin *pLPin = Msta_LibPinForCorner( p->pLib, pLoad->pCell, pRef->PinId, CornerLib );
            char sPinPath[192];
            snprintf( sPinPath, sizeof(sPinPath), "%s/%s",
                      Msta_InstName(pDes, pRef->InstId), Msta_NameStr(pLPin->Name) );
            fprintf( pFile, "    fanout %-24s : %.6f fF\n", sPinPath, pLPin->Cap );
        }
        if ( pOutNetObj->vLoads.nSize > 16 )
            fprintf( pFile, "    ... (%d more fanout loads)\n", pOutNetObj->vLoads.nSize - 16 );
        if ( pCons && Msta_IsSet(fMax ? pCons->LoadMax : pCons->LoadMin) )
            fprintf( pFile, "    sdc set_load%s        : %.6f fF\n",
                     pCons->fSubtractPinLoad ? " (-subtract_pin_load)" : "",
                     fMax ? pCons->LoadMax : pCons->LoadMin );
    }
    else
        fprintf( pFile, "  output net   : (unconnected)\n" );

    for ( a = 0; a < pEvalCell->vArcs.nSize; a++ )
    {
        MstaArc *pArc = MstaArcArrayAt( &pEvalCell->vArcs, a );
        int fIsConstraint = 0, fSetupLike = 1;
        if ( pArc->InPin != InPinId || pArc->OutPin != OutPinId )
            continue;
        nMatched++;
        Msta_ArcMatchesTimingType( pArc, &fIsConstraint, &fSetupLike );
        fprintf( pFile, "  arc #%d       : type = %s, timing_sense = %s, when = %s, active = %s\n",
                 nMatched, Msta_TimingTypeName(pArc->Type), Msta_SenseName(pArc->Sense),
                 pArc->pWhen ? pArc->pWhen : "(none)",
                 Msta_TimingDataArcActive(p, nInst, pEvalCell, pArc) ? "yes" : "no" );
        if ( fIsConstraint )
        {
            int nChkCorner = fSetupLike ? 1 : 0;
            int nCapCorner = Msta_SdcCaptureCorner( p->pSdc, nChkCorner );
            MstaId ChkLib = Msta_SdcOperatingLibrary( p->pSdc, nChkCorner );
            MstaArc *pChkArc = Msta_LibArcForCorner( p->pLib, pEvalCell, a, ChkLib );
            int fClkRise = ( pArc->Type == MSTA_TT_SETUP_RISING || pArc->Type == MSTA_TT_HOLD_RISING ||
                             pArc->Type == MSTA_TT_RECOVERY_RISING || pArc->Type == MSTA_TT_REMOVAL_RISING );
            double ClkDrvSl = (nInNet >= 0) ? Msta_TimingNetSlewEdge(p, nInNet, nCapCorner, fClkRise) : 50.0;
            double DatDrvSlR = (nOutNet >= 0) ? Msta_TimingNetSlewEdge(p, nOutNet, nChkCorner, 1) : 50.0;
            double DatDrvSlF = (nOutNet >= 0) ? Msta_TimingNetSlewEdge(p, nOutNet, nChkCorner, 0) : 50.0;
            double UnusedW = 0.0, ClkSl = ClkDrvSl, DatSlR = DatDrvSlR, DatSlF = DatDrvSlF;
            if ( nInNet >= 0 )
                Msta_TimingPinWireDelay( p, nInNet, nInst, nInPin, nCapCorner, fClkRise, 1,
                                         ClkDrvSl, &UnusedW, &ClkSl );
            if ( nOutNet >= 0 )
            {
                Msta_TimingPinWireDelay( p, nOutNet, nInst, nOutPin, nChkCorner, 1, 0,
                                         DatDrvSlR, &UnusedW, &DatSlR );
                Msta_TimingPinWireDelay( p, nOutNet, nInst, nOutPin, nChkCorner, 0, 0,
                                         DatDrvSlF, &UnusedW, &DatSlF );
            }
            if ( pChkArc == NULL )
                pChkArc = pArc;
            Msta_PrintTableBreakdown( pFile, "constraint rise (data rise)", &pChkArc->ConstraintRise,
                                      ClkSl, DatSlR, 1, 1,
                                      Msta_SdcCheckDerateInst(p->pSdc, nInst, nChkCorner, 1),
                                      1.0, 1 );
            Msta_PrintTableBreakdown( pFile, "constraint fall (data fall)", &pChkArc->ConstraintFall,
                                      ClkSl, DatSlF, 1, 1,
                                      Msta_SdcCheckDerateInst(p->pSdc, nInst, nChkCorner, 0),
                                      1.0, 1 );
        }
        else
        {
            int fIsClockPath = (nOutNet >= 0 && p->pfClockNet[nOutNet]);
            int nClkIdx = -1, c, Depth;
            int nAocvIn = pInst->pCell->fSequential ? -1 : nInNet;
            if ( fIsClockPath )
                for ( c = 0; c < Msta_TimingClockCount(p); c++ )
                    if ( Msta_TimingNetHasClock(p, c, nOutNet) )
                    {   nClkIdx = c; break;   }
            Depth = ( fIsClockPath && nClkIdx >= 0 )
                  ? Msta_TimingAocvClockDepth( p, nClkIdx, nAocvIn, nOutNet )
                  : Msta_TimingAocvDataDepth( p, nAocvIn, nOutNet );
            int eOut;
            for ( eOut = 1; eOut >= 0; eOut-- )
            {
                const MstaTable *pDelayTbl = eOut ? &pArc->DelayRise : &pArc->DelayFall;
                const MstaTable *pTransTbl = eOut ? &pArc->TransRise : &pArc->TransFall;
                int eInBeg = 1, eInEnd = 0, eIn;
                if ( pArc->Sense == MSTA_SENSE_POSITIVE )      eInBeg = eInEnd = eOut;
                else if ( pArc->Sense == MSTA_SENSE_NEGATIVE ) eInBeg = eInEnd = !eOut;
                else if ( !Msta_ArcIsComb(pEvalCell, pArc) )
                {
                    int fClkRise = (pArc->Type != MSTA_TT_FALL_EDGE);
                    eInBeg = eInEnd = fClkRise;
                }
                for ( eIn = eInBeg; eIn >= eInEnd; eIn-- )
                {
                    double SlewIn = eIn ? InSlewR : InSlewF;
                    double Derate = ( fIsClockPath && nClkIdx >= 0 )
                        ? Msta_SdcClockDerateClock( p->pSdc, nClkIdx, nInst, fMax, eIn )
                        : Msta_SdcDataDerateInst( p->pSdc, nInst, fMax, eIn );
                    double Aocv = Msta_TimingAocvArcFactor( p, nInst, nClkIdx, nAocvIn, nOutNet,
                                                            fMax, eIn, fIsClockPath );
                    char sLbl[96];
                    snprintf( sLbl, sizeof(sLbl), "output %s delay (input %s)",
                              eOut ? "rise" : "fall", eIn ? "rise" : "fall" );
                    Msta_PrintTableBreakdown( pFile, sLbl, pDelayTbl, SlewIn, LookupLoad,
                                              1, 0, Derate, Aocv, Depth );
                    snprintf( sLbl, sizeof(sLbl), "output %s slew (input %s)",
                              eOut ? "rise" : "fall", eIn ? "rise" : "fall" );
                    Msta_PrintTableBreakdown( pFile, sLbl, pTransTbl, SlewIn, LookupLoad,
                                              0, 0, 1.0, 1.0, Depth );
                }
            }
        }
    }
    if ( nMatched == 0 )
    {
        Msta_Error( "explain_arc：单元 \"%s\" 上没有从 \"%s\" 到 \"%s\" 的时序弧\n",
                    Msta_NameStr(pEvalCell->Name), pInPin, pOutPin );
        return 0;
    }
    return 1;
}

/* =====================================================================
   教学诊断命令 2：report_net
   ===================================================================== */

static void Msta_FmtOptNs( double Ps, int fValid, char *pBuf, int nBuf )
{
    if ( !fValid )
        snprintf( pBuf, (size_t)nBuf, "N/A" );
    else
        snprintf( pBuf, (size_t)nBuf, "%.3f", Ps / 1000.0 );
}

int Msta_ReportNet( MstaTiming *p, FILE *pFile, const char *pNetName )
{
    MstaDesign *pDes = p->pDes;
    int nNet = Msta_DesignNetByName( pDes, pNetName );
    MstaNet *pNet;
    int i, fCorner, fEdge;
    double WorstSetup = 0.0, WorstHold = 0.0;
    int fHasSetup = 0, fHasHold = 0;

    if ( nNet < 0 )
    {
        Msta_Error( "report_net：找不到网络 \"%s\"\n", pNetName );
        return 0;
    }
    pNet = MstaNetArrayAt( &pDes->vNets, nNet );

    fprintf( pFile, "\n===== report_net: %s (id %d) =====\n", Msta_NetName(pDes, nNet), nNet );
    fprintf( pFile, "  flags        :" );
    if ( pNet->fTopPort )          fprintf( pFile, " top_port(%s)",
                                            pNet->Dir == MSTA_PORT_IN ? "in"
                                            : pNet->Dir == MSTA_PORT_OUT ? "out" : "inout" );
    if ( p->pfClockNet[nNet] )     fprintf( pFile, " clock_net" );
    if ( p->pfIdealNet[nNet] )     fprintf( pFile, " ideal_net" );
    if ( pNet->fConst )            fprintf( pFile, " const(%d)", pNet->fConst - 1 );
    if ( pNet->fCaseValue )        fprintf( pFile, " case_value(%d)", pNet->fCaseValue - 1 );
    if ( !pNet->fTopPort && !p->pfClockNet[nNet] && !p->pfIdealNet[nNet] &&
         !pNet->fConst && !pNet->fCaseValue )
        fprintf( pFile, " normal" );
    fprintf( pFile, "\n" );
    fprintf( pFile, "  logic level  : %d\n", p->pLevel ? p->pLevel[nNet] : 0 );

    if ( pNet->Driver.InstId == MSTA_NO_ID )
        fprintf( pFile, "  driver       : %s\n", pNet->fTopPort ? "(primary input)" : "(undriven)" );
    else
    {
        MstaInst *pInst = MstaInstArrayAt( &pDes->vInsts, pNet->Driver.InstId );
        MstaPin *pPin = MstaPinArrayAt( &pInst->pCell->vPins, pNet->Driver.PinId );
        fprintf( pFile, "  driver       : %s/%s (%s)\n",
                 Msta_InstName(pDes, pNet->Driver.InstId),
                 Msta_NameStr(pPin->Name), Msta_NameStr(pInst->pCell->Name) );
    }
    fprintf( pFile, "  fanout       : %d load(s)\n", pNet->vLoads.nSize );
    for ( i = 0; i < pNet->vLoads.nSize && i < 16; i++ )
    {
        MstaPinRef *pRef = MstaPinRefArrayAt( &pNet->vLoads, i );
        MstaInst *pLoad = MstaInstArrayAt( &pDes->vInsts, pRef->InstId );
        MstaPin *pPinMax = Msta_LibPinForCorner( p->pLib, pLoad->pCell, pRef->PinId,
                                                 Msta_SdcOperatingLibrary(p->pSdc, 1) );
        MstaPin *pPinMin = Msta_LibPinForCorner( p->pLib, pLoad->pCell, pRef->PinId,
                                                 Msta_SdcOperatingLibrary(p->pSdc, 0) );
        fprintf( pFile, "    -> %s/%s (%s, cap max %.3f / min %.3f fF)\n",
                 Msta_InstName(pDes, pRef->InstId), Msta_NameStr(pPinMax->Name),
                 Msta_NameStr(pLoad->pCell->Name), pPinMax->Cap, pPinMin->Cap );
    }
    if ( pNet->vLoads.nSize > 16 )
        fprintf( pFile, "    ... (%d more loads)\n", pNet->vLoads.nSize - 16 );
    fprintf( pFile, "  total load   : max %.3f fF, min %.3f fF\n",
             Msta_TimingNetLoad(p, nNet, 1), Msta_TimingNetLoad(p, nNet, 0) );

    if ( p->pfClockNet[nNet] )
    {
        int c;
        for ( c = 0; c < Msta_TimingClockCount(p); c++ )
            if ( Msta_TimingNetHasClock(p, c, nNet) )
            {
                const MstaClockArr *pCA = Msta_TimingClockArr( p, c, nNet );
                MstaClock *pClk = Msta_SdcClockByIndex( p->pSdc, c );
                fprintf( pFile, "  clock %-8s: max_arr %.3f ns, max_slew %.3f ns, polarity %+d, gates %d\n",
                         Msta_NameStr(pClk->Name), pCA->MaxArrival / 1000.0,
                         pCA->MaxSlew / 1000.0, pCA->Polarity, pCA->nThroughGates );
            }
    }

    fprintf( pFile, "  corner  edge        AT(ns)     RAT(ns)   Slack(ns)    Slew(ns)\n" );
    fprintf( pFile, "  ----------------------------------------------------------------\n" );
    for ( fCorner = 1; fCorner >= 0; fCorner-- )
        for ( fEdge = 1; fEdge >= 0; fEdge-- )
        {
            double AT  = Msta_TimingNetArrivalEdge( p, nNet, fCorner, fEdge );
            double RAT = Msta_TimingNetRequiredEdge( p, nNet, fCorner, fEdge );
            double Sl  = Msta_TimingNetSlewEdge( p, nNet, fCorner, fEdge );
            int fAT  = Msta_TimingTimeValid( AT );
            int fRAT = Msta_TimingTimeValid( RAT );
            int fSlk = fAT && fRAT;
            double Slack = fCorner ? (RAT - AT) : (AT - RAT);
            char sAT[16], sRAT[16], sSlk[16], sSl[16];
            if ( fSlk )
            {
                if ( fCorner )
                {
                    if ( !fHasSetup || Slack < WorstSetup ) { WorstSetup = Slack; fHasSetup = 1; }
                }
                else
                {
                    if ( !fHasHold || Slack < WorstHold ) { WorstHold = Slack; fHasHold = 1; }
                }
            }
            Msta_FmtOptNs( AT, fAT, sAT, sizeof(sAT) );
            Msta_FmtOptNs( RAT, fRAT, sRAT, sizeof(sRAT) );
            Msta_FmtOptNs( Slack, fSlk, sSlk, sizeof(sSlk) );
            Msta_FmtOptNs( Sl, fAT || p->pfClockNet[nNet], sSl, sizeof(sSl) );
            fprintf( pFile, "  %-6s  %-6s  %10s  %10s  %10s  %10s\n",
                     fCorner ? "max" : "min", fEdge ? "rise" : "fall",
                     sAT, sRAT, sSlk, sSl );
        }
    fprintf( pFile, "  ----------------------------------------------------------------\n" );
    {
        char sS[16], sH[16];
        Msta_FmtOptNs( WorstSetup, fHasSetup, sS, sizeof(sS) );
        Msta_FmtOptNs( WorstHold, fHasHold, sH, sizeof(sH) );
        fprintf( pFile, "  setup slack : %s%s\n", sS, fHasSetup ? " ns" : "" );
        fprintf( pFile, "  hold  slack : %s%s\n", sH, fHasHold  ? " ns" : "" );
        fprintf( pFile, "  note        : GBA network RAT merges all clocks; under path exceptions/CPPR compare with report_checks\n" );
    }
    return 1;
}

/* =====================================================================
   教学诊断命令 3：dump_graph（Graphviz DOT 导出）
   ===================================================================== */

static void Msta_PrintDotEscaped( FILE *pFile, const char *pText )
{
    const char *p;
    for ( p = pText; *p; p++ )
    {
        if ( *p == '"' || *p == '\\' )
            fputc( '\\', pFile );
        fputc( *p, pFile );
    }
}

static int Msta_NetWorstSetupSlack( MstaTiming *p, int nNet,
                                    double *pAT, double *pRAT, double *pSlack )
{
    int e, fHas = 0;
    double Best = 0.0, BestAT = 0.0, BestRAT = 0.0;
    for ( e = 1; e >= 0; e-- )
    {
        double AT  = Msta_TimingNetArrivalEdge( p, nNet, 1, e );
        double RAT = Msta_TimingNetRequiredEdge( p, nNet, 1, e );
        if ( Msta_TimingTimeValid(AT) && Msta_TimingTimeValid(RAT) )
        {
            double S = RAT - AT;
            if ( !fHas || S < Best )
            {
                Best = S;
                BestAT = AT;
                BestRAT = RAT;
                fHas = 1;
            }
        }
    }
    if ( pAT && fHas )  *pAT = BestAT;
    if ( pRAT && fHas ) *pRAT = BestRAT;
    *pSlack = Best;
    return fHas;
}

int Msta_ReportDumpGraph( MstaTiming *p, FILE *pFile, int nMaxNodes )
{
    MstaDesign *pDes = p->pDes;
    int nNets = pDes->vNets.nSize;
    int nLimit = nMaxNodes > 0 ? nMaxNodes : 200;
    char *pfEmit = (char *)calloc( (size_t)(nNets > 0 ? nNets : 1), 1 );
    char *pfEnd = (char *)calloc( (size_t)(nNets > 0 ? nNets : 1), 1 );
    int i, q, nShown = 0, nTotalNonConst = 0;
    assert( pfEmit && pfEnd );

    for ( i = 0; i < p->vChecks.nSize; i++ )
    {
        int nEnd = MstaCheckArrayAt( &p->vChecks, i )->nEndNet;
        if ( nEnd >= 0 && nEnd < nNets )
            pfEnd[nEnd] = 1;
    }

    fprintf( pFile, "digraph msta_timing_graph {\n" );
    fprintf( pFile, "  rankdir=LR;\n" );
    fprintf( pFile, "  node [fontname=\"Helvetica\", fontsize=10];\n" );
    fprintf( pFile, "  edge [fontname=\"Helvetica\", fontsize=9];\n" );

    for ( q = 0; q < p->nTopoOrder; q++ )
    {
        int nNet = p->pTopoOrder[q];
        MstaNet *pNet = MstaNetArrayAt( &pDes->vNets, nNet );
        if ( pNet->fConst )
            continue;
        nTotalNonConst++;
        if ( nShown < nLimit )
        {
            pfEmit[nNet] = 1;
            nShown++;
        }
    }
    if ( nShown < nTotalNonConst )
        fprintf( pFile, "  /* truncated: showing %d of %d nets (limit -max_nodes %d) */\n",
                 nShown, nTotalNonConst, nLimit );

    for ( q = 0; q < p->nTopoOrder; q++ )
    {
        int nNet = p->pTopoOrder[q];
        MstaNet *pNet;
        double Slack = 0.0, AT;
        int fSlack, fAT;
        const char *pShape = "ellipse";
        const char *pStyle = "";
        char sAT[16], sRAT[16], sSlk[16];
        double RAT;
        int fRAT;

        if ( !pfEmit[nNet] )
            continue;
        pNet = MstaNetArrayAt( &pDes->vNets, nNet );
        fAT = Msta_TimingTimeValid( p->CornerMax.pArrRise[nNet] ) ||
              Msta_TimingTimeValid( p->CornerMax.pArrFall[nNet] );
        AT = Msta_TimingNetArrival( p, nNet, 1 );
        if ( !fAT && p->pfClockNet[nNet] )
        {
            int c;
            for ( c = 0; c < Msta_TimingClockCount(p); c++ )
                if ( Msta_TimingNetHasClock(p, c, nNet) && Msta_TimingClockArr(p, c, nNet)->fReached )
                {
                    double CA = Msta_TimingClockArr(p, c, nNet)->MaxArrival;
                    if ( !fAT || CA > AT ) { AT = CA; fAT = 1; }
                }
        }
        RAT = p->CornerMax.pReq ? p->CornerMax.pReq[nNet] : 1e30;
        fRAT = Msta_TimingTimeValid( RAT );
        fSlack = Msta_NetWorstSetupSlack( p, nNet, &AT, &RAT, &Slack );
        if ( fSlack )
            fAT = fRAT = 1;

        if ( fSlack && Slack < 0.0 )
            pStyle = ", color=red, penwidth=2.0";
        else if ( p->pfClockNet[nNet] )
        {   pShape = "diamond"; pStyle = ", color=blue";   }
        else if ( pNet->Driver.InstId == MSTA_NO_ID ||
                  MstaInstArrayAt(&pDes->vInsts, pNet->Driver.InstId)->pCell->fSequential )
        {   pShape = "box"; pStyle = ", color=green4";   }
        else if ( pfEnd[nNet] )
        {   pShape = "doubleoctagon"; pStyle = ", color=orange3";   }

        Msta_FmtOptNs( AT, fAT, sAT, sizeof(sAT) );
        Msta_FmtOptNs( RAT, fRAT, sRAT, sizeof(sRAT) );
        Msta_FmtOptNs( Slack, fSlack, sSlk, sizeof(sSlk) );

        fprintf( pFile, "  n%d [shape=%s%s, label=\"", nNet, pShape, pStyle );
        Msta_PrintDotEscaped( pFile, Msta_NetName(pDes, nNet) );
        fprintf( pFile, "\\nL%d AT:%s RAT:%s SLK:%s\"];\n",
                 p->pLevel ? p->pLevel[nNet] : 0, sAT, sRAT, sSlk );
    }

    for ( q = 0; q < p->nTopoOrder; q++ )
    {
        int nOut = p->pTopoOrder[q];
        MstaNet *pNet;
        MstaInst *pDriver;
        MstaCell *pCell;
        MstaPin *pOutPin;
        int k;
        MstaId CornerLib = Msta_SdcOperatingLibrary( p->pSdc, 1 );
        if ( !pfEmit[nOut] )
            continue;
        pNet = MstaNetArrayAt( &pDes->vNets, nOut );
        if ( pNet->fCaseValue || pNet->Driver.InstId == MSTA_NO_ID )
            continue;
        pDriver = MstaInstArrayAt( &pDes->vInsts, pNet->Driver.InstId );
        pCell = pDriver->pCell;
        if ( pCell->fSequential )
            continue;
        pOutPin = MstaPinArrayAt( &pCell->vPins, pNet->Driver.PinId );
        for ( k = 0; k < pDriver->nPins && k < pCell->vPins.nSize; k++ )
        {
            MstaPin *pIn = MstaPinArrayAt( &pCell->vPins, k );
            int nIn = pDriver->pNets[k], a, nCombArcs = 0, fActive = 0;
            MstaCell *pCombCell;
            if ( pIn->Dir == MSTA_DIR_OUTPUT || nIn < 0 || !pfEmit[nIn] )
                continue;
            if ( p->pfClockNet[nIn] && !p->pfClockNet[nOut] )
                continue;
            if ( Msta_SdcTimingDisabled(p->pSdc, pNet->Driver.InstId, pIn->Name, pOutPin->Name) )
                continue;
            pCombCell = Msta_LibCellForCorner( p->pLib, pCell, CornerLib );
            if ( pCombCell != pCell && Msta_CellCombArc(pCombCell, pIn->Name, pOutPin->Name) == NULL )
                pCombCell = pCell;
            for ( a = 0; a < pCombCell->vArcs.nSize; a++ )
            {
                MstaArc *pArc = MstaArcArrayAt( &pCombCell->vArcs, a );
                if ( pArc->InPin != pIn->Name || pArc->OutPin != pOutPin->Name ||
                     !Msta_ArcIsComb(pCombCell, pArc) )
                    continue;
                nCombArcs++;
                if ( Msta_TimingDataArcActive(p, pNet->Driver.InstId, pCombCell, pArc) )
                    fActive = 1;
            }
            if ( nCombArcs == 0 )
            {
                if ( Msta_CellHasAsyncArc(pCell, pIn->Name, pOutPin->Name) )
                    continue;
            }
            else if ( !fActive )
                continue;
            fprintf( pFile, "  n%d -> n%d [label=\"", nIn, nOut );
            Msta_PrintDotEscaped( pFile, Msta_InstName(pDes, pNet->Driver.InstId) );
            fprintf( pFile, ":%s->%s\"];\n",
                     Msta_NameStr(pIn->Name), Msta_NameStr(pOutPin->Name) );
        }
    }

    fprintf( pFile, "}\n" );
    free( pfEmit );
    free( pfEnd );
    return 1;
}
