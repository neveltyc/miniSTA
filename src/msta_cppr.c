/**CFile***************************************************************

  FileName    [msta_cppr.c]

  Synopsis    [公共时钟路径 pessimism 消除（CPPR）、AOCV 图深度与起点签名分类。]

***********************************************************************/

#include "msta_timing_int.h"

/* =====================================================================
   AOCV 图深度（原 msta_aocv_depth.inc 合并入本模块）
   ---------------------------------------------------------------------
   F 是任意起点到当前网络的最短级数，B 是当前网络到任意端点的最短
   级数；一条弧的完整上下文是 F[In] + 1 + B[Out]。
   ===================================================================== */

/* 返回 -1=不可遍历，0=未建模的零延迟连接，1=普通单元弧。 */
static int AocvCombStep( MstaTiming *p, int nInst, int nInPin, int nOutPin, int fClock )
{
    MstaInst *pInst = Msta_TiInstAt( p, nInst );
    MstaPin *pIn = MstaPinArrayAt( &pInst->pCell->vPins, nInPin );
    MstaPin *pOut = MstaPinArrayAt( &pInst->pCell->vPins, nOutPin );
    MstaArc *pArc;

    if ( pIn->Dir == MSTA_DIR_OUTPUT || Msta_SdcTimingDisabled( p->pSdc, nInst, pIn->Name, pOut->Name ) )
        return -1;
    pArc = Msta_TiTimingCombArc( p, pInst->pCell, pIn->Name, pOut->Name, 1 );
    if ( !pArc )
        pArc = Msta_TiTimingCombArc( p, pInst->pCell, pIn->Name, pOut->Name, 0 );
    if ( !pArc && ( fClock || Msta_CellHasAsyncArc( pInst->pCell, pIn->Name, pOut->Name ) ) )
        return -1;
    return pArc ? 1 : 0;
}

static int AocvNetMatchesGraph( MstaTiming *p, int nClock, int nNet )
{
    return nClock >= 0 ? Msta_TiNetHasClock( p, nClock, nNet ) : !p->pfClockNet[nNet];
}

static int AocvEffectiveStep( MstaTiming *p, MstaClock *pClock, int nNet, int nInst,
                              int nInPin, int nOutPin )
{
    int nStep = AocvCombStep( p, nInst, nInPin, nOutPin, pClock != NULL );
    if ( nStep >= 0 && ( p->pfIdealNet[nNet] || ( pClock && !pClock->fPropagated ) ) )
        return 0;
    return nStep;
}

static void MarkAocvIoEndpoints( MstaTiming *p, int nClock, MstaClock *pClock,
                                MstaIoDelayArray *pDelays, int *pBackward )
{
    int i;
    for ( i = 0; i < pDelays->nSize; i++ )
    {
        MstaIoDelay *pDelay = MstaIoDelayArrayAt( pDelays, i );
        if ( pDelay->Clock != MSTA_NO_ID && pDelay->Clock != pClock->Name )
            continue;
        if ( Msta_IsSet( pDelay->Max ) && Msta_TiNetHasClock( p, nClock, pDelay->RefNetMax ) )
            pBackward[pDelay->RefNetMax] = 0;
        if ( Msta_IsSet( pDelay->Min ) && Msta_TiNetHasClock( p, nClock, pDelay->RefNetMin ) )
            pBackward[pDelay->RefNetMin] = 0;
    }
}

static void MarkAocvClockEndpoints( MstaTiming *p, int nClock, MstaClock *pClock, int *pBackward )
{
    int i, k;
    for ( i = 0; i < p->pDes->vInsts.nSize; i++ )
    {
        MstaCell *pCell = Msta_TiInstAt( p, i )->pCell;
        for ( k = 0; k < pCell->vRegs.nSize; k++ )
        {
            int nNet = Msta_TiInstPinNet( p, i, MstaRegCheckArrayAt( &pCell->vRegs, k )->ClkPin );
            if ( Msta_TiNetHasClock( p, nClock, nNet ) )
                pBackward[nNet] = 0;
        }
        for ( k = 0; k < pCell->vAsync.nSize; k++ )
        {
            int nNet = Msta_TiInstPinNet( p, i, MstaAsyncCheckArrayAt( &pCell->vAsync, k )->ClkPin );
            if ( Msta_TiNetHasClock( p, nClock, nNet ) )
                pBackward[nNet] = 0;
        }
        for ( k = 0; k < pCell->vGates.nSize; k++ )
        {
            int nNet = Msta_TiInstPinNet( p, i, MstaGateCheckArrayAt( &pCell->vGates, k )->ClkPin );
            if ( Msta_TiNetHasClock( p, nClock, nNet ) )
                pBackward[nNet] = 0;
        }
    }
    MarkAocvIoEndpoints( p, nClock, pClock, &p->pSdc->vInputDelays, pBackward );
    MarkAocvIoEndpoints( p, nClock, pClock, &p->pSdc->vOutputDelays, pBackward );
    for ( i = 0; i < Msta_SdcClockCount( p->pSdc ); i++ )
    {
        MstaClock *pChild = Msta_SdcClockByIndex( p->pSdc, i );
        if ( pChild->MasterClock != pClock->Name || pChild->SourceNet < 0 )
            continue;
        if ( Msta_TiNetHasClock( p, nClock, pChild->SourceNet ) )
            pBackward[pChild->SourceNet] = 0;
        else
        {
            MstaNet *pSrcNet = MstaNetArrayAt( &p->pDes->vNets, pChild->SourceNet );
            if ( pSrcNet->Driver.InstId >= 0 )
            {
                MstaInst *pSrcInst = Msta_TiInstAt( p, pSrcNet->Driver.InstId );
                MstaCell *pSrcCell = pSrcInst->pCell;
                if ( pSrcCell->fSequential )
                {
                    for ( k = 0; k < pSrcCell->vRegs.nSize; k++ )
                    {
                        int nClkNet = Msta_TiInstPinNet( p, pSrcNet->Driver.InstId,
                                                         MstaRegCheckArrayAt( &pSrcCell->vRegs, k )->ClkPin );
                        if ( Msta_TiNetHasClock( p, nClock, nClkNet ) )
                        {
                            pBackward[pChild->SourceNet] = 0;
                            break;
                        }
                    }
                }
            }
        }
    }
}

static void MarkAocvDataEndpoints( MstaTiming *p, int *pBackward )
{
    int i, k;
    for ( i = 0; i < p->vChecks.nSize; i++ )
        pBackward[MstaCheckArrayAt( &p->vChecks, i )->nEndNet] = 0;
    for ( i = 0; i < Msta_SdcDataCheckCount( p->pSdc ); i++ )
    {
        MstaDataCheck *pCheck = Msta_SdcDataCheckByIndex( p->pSdc, i );
        if ( !pCheck->fSetup )
            continue;
        if ( pCheck->FromNet >= 0 )
            pBackward[pCheck->FromNet] = 0;
        if ( pCheck->ToNet >= 0 )
            pBackward[pCheck->ToNet] = 0;
    }
    for ( i = 0; i < p->pDes->vInsts.nSize; i++ )
    {
        MstaCell *pCell = Msta_TiInstAt( p, i )->pCell;
        for ( k = 0; k < pCell->vGates.nSize; k++ )
        {
            int nNet = Msta_TiInstPinNet( p, i, MstaGateCheckArrayAt( &pCell->vGates, k )->EnablePin );
            if ( nNet >= 0 )
                pBackward[nNet] = 0;
        }
    }
}

static int AocvSequentialStartDepth( MstaTiming *p, MstaInst *pInst, MstaNet *pNet, int nNet )
{
    MstaId OutPin = MstaPinArrayAt( &pInst->pCell->vPins, pNet->Driver.PinId )->Name;
    int k, fClockRise, nDepth = MSTA_AOCV_DEPTH_INF;
    for ( k = 0; k < pInst->pCell->vRegs.nSize; k++ )
    {
        MstaRegCheck *pReg = MstaRegCheckArrayAt( &pInst->pCell->vRegs, k );
        if ( Msta_TiRegClkToQArc( p, pInst->pCell, pReg, OutPin, &fClockRise, 1, pNet->Driver.InstId ) ||
             Msta_TiRegClkToQArc( p, pInst->pCell, pReg, OutPin, &fClockRise, 0, pNet->Driver.InstId ) )
            nDepth = p->pfIdealNet[nNet] ? 0 : 1;
    }
    return nDepth;
}

static void PropagateAocvForwardDepth( MstaTiming *p, int nClock, MstaClock *pClock, int *pForward )
{
    int q, k;
    for ( q = 0; q < p->nTopoOrder; q++ )
    {
        int nNet = p->pTopoOrder[q];
        MstaNet *pNet = MstaNetArrayAt( &p->pDes->vNets, nNet );
        MstaInst *pInst;
        if ( pNet->fCaseValue || pNet->fConst || !AocvNetMatchesGraph( p, nClock, nNet ) )
            continue;
        if ( !pClock && pNet->Driver.InstId < 0 )
        {
            pForward[nNet] = 0;
            continue;
        }
        if ( pClock && nNet == pClock->SourceNet )
            continue;
        if ( pNet->Driver.InstId < 0 )
            continue;
        pInst = Msta_TiInstAt( p, pNet->Driver.InstId );
        if ( pInst->pCell->fSequential )
        {
            if ( !pClock )
                pForward[nNet] = AocvSequentialStartDepth( p, pInst, pNet, nNet );
            continue;
        }
        for ( k = 0; k < pInst->nPins && k < pInst->pCell->vPins.nSize; k++ )
        {
            int nInNet = pInst->pNets[k];
            int nStep = AocvEffectiveStep( p, pClock, nNet, pNet->Driver.InstId, k, pNet->Driver.PinId );
            if ( nInNet < 0 || nStep < 0 || !AocvNetMatchesGraph( p, nClock, nInNet ) )
                continue;
            if ( pForward[nInNet] + nStep < pForward[nNet] )
                pForward[nNet] = pForward[nInNet] + nStep;
        }
    }
}

static void PropagateAocvBackwardDepth( MstaTiming *p, int nClock, MstaClock *pClock, int *pBackward )
{
    int q, k;
    for ( q = p->nTopoOrder - 1; q >= 0; q-- )
    {
        int nNet = p->pTopoOrder[q];
        MstaNet *pNet = MstaNetArrayAt( &p->pDes->vNets, nNet );
        MstaInst *pInst;
        if ( pBackward[nNet] == MSTA_AOCV_DEPTH_INF || pNet->Driver.InstId < 0 ||
             pNet->fCaseValue || pNet->fConst || !AocvNetMatchesGraph( p, nClock, nNet ) )
            continue;
        pInst = Msta_TiInstAt( p, pNet->Driver.InstId );
        if ( pInst->pCell->fSequential || ( pClock && nNet == pClock->SourceNet ) )
            continue;
        for ( k = 0; k < pInst->nPins && k < pInst->pCell->vPins.nSize; k++ )
        {
            int nInNet = pInst->pNets[k];
            int nStep = AocvEffectiveStep( p, pClock, nNet, pNet->Driver.InstId, k, pNet->Driver.PinId );
            if ( nInNet < 0 || nStep < 0 || !AocvNetMatchesGraph( p, nClock, nInNet ) )
                continue;
            if ( pBackward[nNet] + nStep < pBackward[nInNet] )
                pBackward[nInNet] = pBackward[nNet] + nStep;
        }
    }
}

static void BuildAocvGraphDepth( MstaTiming *p, int nClock, int *pForward, int *pBackward )
{
    MstaClock *pClock = nClock >= 0 ? Msta_SdcClockByIndex( p->pSdc, nClock ) : NULL;
    int n;
    for ( n = 0; n < p->pDes->vNets.nSize; n++ )
        pForward[n] = pBackward[n] = MSTA_AOCV_DEPTH_INF;
    if ( pClock )
    {
        if ( pClock->SourceNet >= 0 )
            pForward[pClock->SourceNet] = 0;
        MarkAocvClockEndpoints( p, nClock, pClock, pBackward );
    }
    else
        MarkAocvDataEndpoints( p, pBackward );
    PropagateAocvForwardDepth( p, nClock, pClock, pForward );
    PropagateAocvBackwardDepth( p, nClock, pClock, pBackward );
}

void Msta_TiBuildAocvDepths( MstaTiming *p )
{
    int nNets = p->pDes->vNets.nSize;
    int c, nClocks = Msta_SdcClockCount( p->pSdc );
    if ( !p->pSdc->fAocv || !p->pSdc->vAocv.nSize )
        return;
    p->pAocvDataForward  = (int *)malloc( (size_t)nNets * sizeof(int) );
    p->pAocvDataBackward = (int *)malloc( (size_t)nNets * sizeof(int) );
    p->pAocvClockForward  = (int *)malloc( (size_t)nNets * nClocks * sizeof(int) );
    p->pAocvClockBackward = (int *)malloc( (size_t)nNets * nClocks * sizeof(int) );
    assert( p->pAocvDataForward && p->pAocvDataBackward &&
            p->pAocvClockForward && p->pAocvClockBackward );
    BuildAocvGraphDepth( p, -1, p->pAocvDataForward, p->pAocvDataBackward );
    for ( c = 0; c < nClocks; c++ )
        BuildAocvGraphDepth( p, c, p->pAocvClockForward + (size_t)c * nNets,
                             p->pAocvClockBackward + (size_t)c * nNets );
}

static int AocvDepthThroughArc( const int *pForward, const int *pBackward, int nInNet, int nOutNet )
{
    if ( !pForward || !pBackward || nOutNet < 0 || pBackward[nOutNet] >= MSTA_AOCV_DEPTH_INF ||
         ( nInNet >= 0 && pForward[nInNet] >= MSTA_AOCV_DEPTH_INF ) )
        return 1;
    return ( nInNet < 0 ? 0 : pForward[nInNet] ) + 1 + pBackward[nOutNet];
}

int Msta_TimingAocvDataDepth( MstaTiming *p, int nInNet, int nOutNet )
{
    return AocvDepthThroughArc( p->pAocvDataForward, p->pAocvDataBackward, nInNet, nOutNet );
}

int Msta_TimingAocvClockDepth( MstaTiming *p, int nClock, int nInNet, int nOutNet )
{
    int nNets = p->pDes->vNets.nSize;
    if ( p == NULL || nClock < 0 || nClock >= Msta_SdcClockCount(p->pSdc) || !p->pAocvClockForward )
        return 1;
    return AocvDepthThroughArc( p->pAocvClockForward + (size_t)nClock * nNets,
                                p->pAocvClockBackward + (size_t)nClock * nNets,
                                nInNet, nOutNet );
}

double Msta_TiAocvDelayFactor( MstaTiming *p, int nInst, int nClock, int nInNet, int nOutNet,
                               int fMax, int fRise, int fClock )
{
    int nNets = p->pDes->vNets.nSize;
    const int *pForward = p->pAocvDataForward;
    const int *pBackward = p->pAocvDataBackward;
    int nDepth;
    if ( fClock && nClock >= 0 && p->pAocvClockForward )
    {
        pForward = p->pAocvClockForward + (size_t)nClock * nNets;
        pBackward = p->pAocvClockBackward + (size_t)nClock * nNets;
    }
    nDepth = AocvDepthThroughArc( pForward, pBackward, nInNet, nOutNet );
    return Msta_AocvFactor( p->pSdc, Msta_TiInstAt(p, nInst), fClock, fRise, fMax, nDepth );
}

double Msta_TimingAocvArcFactor( MstaTiming *p, int nInst, int nClock, int nInNet, int nOutNet,
                                 int fMax, int fInRise, int fClock )
{
    if ( p == NULL || nInst < 0 || nInst >= p->pDes->vInsts.nSize )
        return 1.0;
    return Msta_TiAocvDelayFactor( p, nInst, nClock, nInNet, nOutNet, fMax, fInRise, fClock );
}

/* =====================================================================
   CPPR（Common Path Pessimism Removal）与时钟路径回溯
   ===================================================================== */

/* 时钟路径从叶到根保存实际前驱；生成时钟直接接回主时钟路径。 */
static int TraceClockPath( MstaTiming *p, MstaClockPrev Point, int fMax,
                           MstaClockPath *pPath, int nGuard )
{
    pPath->nSize = 0;
    while ( Point.Clock >= 0 && Point.Net >= 0 && pPath->nSize < nGuard )
    {
        MstaClockArr *pArr;
        MstaClockPrev Prev;
        if ( Point.Clock >= Msta_SdcClockCount( p->pSdc ) || Point.Net >= p->pDes->vNets.nSize )
            break;
        pArr = Msta_TiClockArrAt( p, Point.Clock, Point.Net );
        if ( !Msta_TiClockTagValid( pArr, fMax, Point.Edge, Point.Source ) )
            break;
        Prev = pArr->Prev[fMax][Point.Edge][Point.Source];
        Point.Inst = Prev.Inst;
        Point.Pin = Prev.Pin;
        *MstaClockPathAppend( pPath ) = Point;
        Point = Prev;
    }
    if ( Point.Net >= 0 )
        pPath->nSize = 0;
    return pPath->nSize;
}

int Msta_TimingTraceClock( MstaTiming *p, int nClock, int nClkNet, int fMax,
                           int fLocalRise, int fSourceRise,
                           MstaClockPrev *pOut, int nCap )
{
    MstaClockPath Path;
    MstaClockPrev Point;
    int nGuard, nCount, i, nCopy;
    if ( p == NULL || pOut == NULL || nCap <= 0 ||
         nClock < 0 || nClock >= Msta_SdcClockCount(p->pSdc) ||
         nClkNet < 0 || nClkNet >= p->pDes->vNets.nSize )
        return 0;
    if ( fSourceRise < 0 )
        fSourceRise = Msta_TiClockTagSelect( Msta_TiClockArrAt(p, nClock, nClkNet),
                                             fMax, fLocalRise, -1 );
    if ( fSourceRise < 0 )
        return 0;
    MstaClockPathInit( &Path );
    nGuard = p->pDes->vNets.nSize + Msta_SdcClockCount(p->pSdc) + 1;
    Point = (MstaClockPrev){ nClock, nClkNet, fLocalRise, fSourceRise, -1, -1 };
    nCount = TraceClockPath( p, Point, fMax, &Path, nGuard );
    nCopy = nCount < nCap ? nCount : nCap;
    /* 反转为 source -> ... -> sink 顺序方便报告层直接顺着打印。 */
    for ( i = 0; i < nCopy; i++ )
        pOut[i] = Path.pData[nCount - 1 - i];
    MstaClockPathFree( &Path );
    return nCopy;
}

static int SameClockPoint( MstaClockPrev Left, MstaClockPrev Right )
{
    return Left.Clock == Right.Clock && Left.Net == Right.Net &&
           Left.Inst == Right.Inst && Left.Pin == Right.Pin &&
           Left.Edge == Right.Edge && Left.Source == Right.Source;
}

static int SameCpprArc( MstaClockPrev Launch, MstaClockPrev Capture, int fSamePin )
{
    return Launch.Net == Capture.Net && Launch.Inst == Capture.Inst && Launch.Pin == Capture.Pin &&
           ( fSamePin || Launch.Edge == Capture.Edge );
}

static double CpprCornerDifference( MstaTiming *p, MstaClockPrev Point )
{
    MstaClockArr *pArr = Msta_TiClockArrAt( p, Point.Clock, Point.Net );
    return fabs( Msta_TiClockArrival( pArr, 1, Point.Edge, Point.Source )
               - Msta_TiClockArrival( pArr, 0, Point.Edge, Point.Source ) );
}

static double ClockCppr( MstaTiming *p, MstaClockPrev Launch, MstaClockPrev Capture, int fSetup )
{
    int nGuard = p->pDes->vNets.nSize + Msta_SdcClockCount( p->pSdc ) + 1;
    MstaClockPath *pLaunch = &p->vCpprPaths[MSTA_CPPR_LAUNCH_PATH];
    MstaClockPath *pCapture = &p->vCpprPaths[MSTA_CPPR_CAPTURE_PATH];
    MstaClockPath *pOppositeLaunch = &p->vCpprPaths[MSTA_CPPR_OPPOSITE_LAUNCH_PATH];
    MstaClockPath *pOppositeCapture = &p->vCpprPaths[MSTA_CPPR_OPPOSITE_CAPTURE_PATH];
    int i;
    double Credit = 0;

    if ( !p->pSdc->fCppr || p->pSdc->AnalysisType != MSTA_ANALYSIS_OCV ||
         Launch.Clock < 0 || Capture.Clock < 0 || Launch.Net < 0 || Capture.Net < 0 )
        return 0;
    if ( !Msta_SdcClockByIndex( p->pSdc, Launch.Clock )->fPropagated ||
         !Msta_SdcClockByIndex( p->pSdc, Capture.Clock )->fPropagated )
        return 0;

    TraceClockPath( p, Launch, fSetup, pLaunch, nGuard );
    TraceClockPath( p, Capture, !fSetup, pCapture, nGuard );
    TraceClockPath( p, Launch, !fSetup, pOppositeLaunch, nGuard );
    TraceClockPath( p, Capture, fSetup, pOppositeCapture, nGuard );
    for ( i = 0; i < pLaunch->nSize && i < pCapture->nSize &&
                 i < pOppositeLaunch->nSize && i < pOppositeCapture->nSize; i++ )
    {
        MstaClockPrev LaunchPoint = pLaunch->pData[pLaunch->nSize - 1 - i];
        MstaClockPrev CapturePoint = pCapture->pData[pCapture->nSize - 1 - i];
        MstaClockPrev OppositeLaunch = pOppositeLaunch->pData[pOppositeLaunch->nSize - 1 - i];
        MstaClockPrev OppositeCapture = pOppositeCapture->pData[pOppositeCapture->nSize - 1 - i];
        double LaunchDelta, CaptureDelta;

        if ( !SameCpprArc( LaunchPoint, CapturePoint, p->pSdc->fCpprSamePin ) ||
             !SameClockPoint( LaunchPoint, OppositeLaunch ) ||
             !SameClockPoint( CapturePoint, OppositeCapture ) )
            break;
        LaunchDelta = CpprCornerDifference( p, LaunchPoint );
        CaptureDelta = CpprCornerDifference( p, CapturePoint );
        Credit = fmin( LaunchDelta, CaptureDelta );
    }
    return Credit;
}

MstaClockPrev Msta_TiLaunchClockPoint( MstaTiming *p, int nStart, int nClock, int fMax, int nSourceTag )
{
    MstaNet *pNet = MstaNetArrayAt( &p->pDes->vNets, nStart );
    MstaClockPrev Point = { nClock, -1, nSourceTag, nSourceTag, -1, -1 };
    if ( pNet->Driver.InstId < 0 )
    {
        Point.Net = Msta_SdcPortReferenceNet( p->pSdc, nStart, nClock, 0, fMax );
        Point.Edge = !Msta_SdcPortClockFall( p->pSdc, nStart, nClock, 0, fMax );
        if ( Point.Net < 0 && !Msta_SdcPortSourceLatencyIncluded( p->pSdc, nStart, nClock, 0, fMax ) )
            Point.Net = Msta_SdcClockByIndex( p->pSdc, nClock )->SourceNet;
    }
    else
    {
        MstaInst *pInst = Msta_TiInstAt( p, pNet->Driver.InstId );
        MstaPin *pOut = MstaPinArrayAt( &pInst->pCell->vPins, pNet->Driver.PinId );
        int k;
        for ( k = 0; k < pInst->pCell->vRegs.nSize; k++ )
        {
            MstaRegCheck *pReg = MstaRegCheckArrayAt( &pInst->pCell->vRegs, k );
            int fClockRise, nClockNet;
            if ( !Msta_TiRegClkToQArc( p, pInst->pCell, pReg, pOut->Name, &fClockRise, fMax, pNet->Driver.InstId ) )
                continue;
            nClockNet = Msta_TiInstPinNet( p, pNet->Driver.InstId, pReg->ClkPin );
            if ( Msta_TiNetHasClock( p, nClock, nClockNet ) &&
                 Msta_TiClockTagValid( Msta_TiClockArrAt(p, nClock, nClockNet), fMax, fClockRise, nSourceTag ) )
            {
                Point.Net = nClockNet;
                Point.Edge = fClockRise;
                break;
            }
        }
    }
    return Point;
}

double Msta_TiPathCppr( MstaTiming *p, int nStart, int nLaunchClock, int nCaptureClock,
                        int nCaptureNet, int fSetup, int nLaunchSource,
                        int nCaptureLocal, int nCaptureSource )
{
    MstaClockPrev Launch = Msta_TiLaunchClockPoint( p, nStart, nLaunchClock, fSetup, nLaunchSource );
    MstaClockPrev Capture = { nCaptureClock, nCaptureNet, nCaptureLocal, nCaptureSource, -1, -1 };
    return ClockCppr( p, Launch, Capture, fSetup );
}

/* =====================================================================
   起点分类（使拥有不同 launch 相位 / CPPR 上下文 / -from 例外的起点隔离传播）
   ===================================================================== */

static const int *s_pSortSig, *s_pSortSigBeg, *s_pSortSigLen, *s_pSortPhase;
static int s_nSortClocks;
typedef struct {
    int Net, LocalEdge;
} MstaCpprStartContext;

enum {
    MSTA_CPPR_CORNERS = 2,
    MSTA_CPPR_SOURCE_TAGS = 2,
    MSTA_CPPR_CONTEXTS_PER_CLOCK = MSTA_CPPR_CORNERS * MSTA_CPPR_SOURCE_TAGS
};

static const MstaCpprStartContext *s_pCpprContexts;
static const int *s_pCpprIndex;
static int s_nCpprContexts;

static MstaCpprStartContext *BuildCpprStartContexts( MstaTiming *p, const int *pOrder,
                                                   int nStarts, int nClocks, int **ppIndex )
{
    MstaCpprStartContext *pContexts;
    int i, c, fMax, nSource;
    *ppIndex = NULL;
    if ( !p->pSdc->fCppr || p->pSdc->AnalysisType != MSTA_ANALYSIS_OCV || !nStarts )
        return NULL;
    pContexts = (MstaCpprStartContext *)malloc( (size_t)nStarts * nClocks *
                                                MSTA_CPPR_CONTEXTS_PER_CLOCK * sizeof(*pContexts) );
    *ppIndex = (int *)calloc( (size_t)p->pDes->vNets.nSize, sizeof(int) );
    assert( pContexts && *ppIndex );
    for ( i = 0; i < nStarts; i++ )
    {
        int nStart = pOrder[i];
        (*ppIndex)[nStart] = i;
        for ( c = 0; c < nClocks; c++ )
        for ( fMax = 0; fMax < MSTA_CPPR_CORNERS; fMax++ )
        for ( nSource = 0; nSource < MSTA_CPPR_SOURCE_TAGS; nSource++ )
        {
            MstaClockPrev Point = Msta_TiLaunchClockPoint( p, nStart, c, fMax, nSource );
            size_t nContext = ( (size_t)i * nClocks + c ) * MSTA_CPPR_CONTEXTS_PER_CLOCK
                            + fMax * MSTA_CPPR_SOURCE_TAGS + nSource;
            if ( !Msta_SdcClockByIndex( p->pSdc, c )->fPropagated || Point.Net < 0 ||
                 !Msta_TiClockTagValid( Msta_TiClockArrAt(p, c, Point.Net), fMax, Point.Edge, nSource ) )
                Point.Net = Point.Edge = -1;
            pContexts[nContext].Net = Point.Net;
            pContexts[nContext].LocalEdge = Point.Edge;
        }
    }
    return pContexts;
}

static int CompareStartSignature( const void *pA, const void *pB )
{
    int a = *(const int *)pA, b = *(const int *)pB, k;
    for ( k = 0; k < s_nSortClocks; k++ )
    {
        int x = s_pSortPhase[(size_t)a * s_nSortClocks + k];
        int y = s_pSortPhase[(size_t)b * s_nSortClocks + k];
        if ( x != y ) return x < y ? -1 : 1;
    }
    if ( s_pCpprContexts )
        for ( k = 0; k < s_nCpprContexts; k++ )
        {
            const MstaCpprStartContext *pLeft = &s_pCpprContexts[(size_t)s_pCpprIndex[a] * s_nCpprContexts + k];
            const MstaCpprStartContext *pRight = &s_pCpprContexts[(size_t)s_pCpprIndex[b] * s_nCpprContexts + k];
            if ( pLeft->Net != pRight->Net )
                return pLeft->Net < pRight->Net ? -1 : 1;
            if ( pLeft->LocalEdge != pRight->LocalEdge )
                return pLeft->LocalEdge < pRight->LocalEdge ? -1 : 1;
        }
    if ( s_pSortSigLen[a] != s_pSortSigLen[b] )
        return s_pSortSigLen[a] < s_pSortSigLen[b] ? -1 : 1;
    for ( k = 0; k < s_pSortSigLen[a]; k++ )
    {
        int x = s_pSortSig[s_pSortSigBeg[a] + k], y = s_pSortSig[s_pSortSigBeg[b] + k];
        if ( x != y )
            return x < y ? -1 : 1;
    }
    return 0;
}

int Msta_TiBuildStartClasses( MstaTiming *p )
{
    int nNets = p->pDes->vNets.nSize;
    int nClocks = Msta_SdcClockCount( p->pSdc );
    int nEx = Msta_SdcExceptionCount( p->pSdc );
    int *pGroups = (int *)malloc( (size_t)(nEx + 1) * sizeof(int) );
    int *pBeg = (int *)calloc( (size_t)(nNets + 1), sizeof(int) );
    int *pLen = (int *)calloc( (size_t)(nNets + 1), sizeof(int) );
    int *pOrder = (int *)malloc( (size_t)(nNets + 1) * sizeof(int) );
    int *pSig = NULL;
    int *pPhase = (int *)calloc( (size_t)nNets * nClocks, sizeof(int) );
    MstaCpprStartContext *pCpprContexts;
    int *pCpprIndex;
    const int *pMatches;
    int nSig = 0, nSigCap = 0, nStarts = 0, nClasses = 0, nMatches, n, i, c;
    assert( pGroups && pBeg && pLen && pOrder && pPhase );
    Msta_SdcFromExceptionGroups( p->pSdc, pGroups );
    for ( n = 0; n < nNets; n++ )
    {
        MstaSdcObject Obj;
        int fStart = 0;
        p->pStartClass[n] = -1;
        for ( c = 0; c < nClocks && !fStart; c++ )
            fStart = Msta_TiIsStartForClock( p, n, c );
        if ( !fStart )
            continue;
        Msta_TiStartEndpoint( p, n, &Obj );
        pBeg[n] = nSig;
        nMatches = Msta_SdcFromExceptionMatches( p->pSdc, p->pDes, &Obj, &pMatches );
        for ( i = 0; i < nMatches; i++ )
        {
            if ( pGroups[pMatches[i]] < 0 )
                continue;
            if ( nSig == nSigCap )
            {
                nSigCap = nSigCap ? 2 * nSigCap : 1024;
                pSig = (int *)realloc( pSig, (size_t)nSigCap * sizeof(int) );
                assert( pSig );
            }
            pSig[nSig++] = pGroups[pMatches[i]];
        }
        pLen[n] = nSig - pBeg[n];
        pOrder[nStarts++] = n;
    }
    for ( c = 0; c < nClocks; c++ )
    {
        Msta_TiPropagateData( p, 1, c, -1 );
        Msta_TiPropagateData( p, 0, c, -1 );
        for ( i = 0; i < nStarts; i++ )
        {
            n = pOrder[i];
            pPhase[(size_t)n * nClocks + c] =
                (p->CornerMax.pnLaunchClock[n] == c ? 1 + p->CornerMax.pfLaunchRises[n] : 0)
                + 3 * (p->CornerMin.pnLaunchClock[n] == c ? 1 + p->CornerMin.pfLaunchRises[n] : 0);
        }
    }
    pCpprContexts = BuildCpprStartContexts( p, pOrder, nStarts, nClocks, &pCpprIndex );
    s_nCpprContexts = nClocks * MSTA_CPPR_CONTEXTS_PER_CLOCK;
    s_pCpprContexts = pCpprContexts;
    s_pCpprIndex = pCpprIndex;
    s_pSortPhase = pPhase;
    s_nSortClocks = nClocks;
    s_pSortSig = pSig;
    s_pSortSigBeg = pBeg;
    s_pSortSigLen = pLen;
    qsort( pOrder, (size_t)nStarts, sizeof(int), CompareStartSignature );
    for ( i = 0; i < nStarts; i++ )
    {
        if ( i == 0 || CompareStartSignature( &pOrder[i-1], &pOrder[i] ) != 0 )
            nClasses++;
        p->pStartClass[pOrder[i]] = nClasses - 1;
    }
    free( pCpprContexts );
    free( pCpprIndex );
    s_pCpprContexts = NULL;
    s_pCpprIndex = NULL;
    free( pPhase );
    free( pSig );
    free( pOrder );
    free( pLen );
    free( pBeg );
    free( pGroups );
    return nClasses;
}
