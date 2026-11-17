/**CFile***************************************************************

  FileName    [msta_check.c]

  Synopsis    [端点构建、约束查表、捕获沿对齐、路径例外与次优路径、数据/门控检查。]

***********************************************************************/

#include "msta_timing_int.h"

/* =====================================================================
   1. 端点收集与检查值查表
   ===================================================================== */

void Msta_TiBuildChecks( MstaTiming *p )
{
    MstaDesign *pDes = p->pDes;
    int i, k;

    for ( i = 0; i < pDes->vInsts.nSize; i++ )
    {
        MstaInst *pInst = Msta_TiInstAt( p, i );
        MstaCell *pCell = pInst->pCell;
        MstaCheck *pNew;
        if ( !pCell->fSequential )
            continue;
        if ( pCell->fLatch ) p->nLatches++;
        else                 p->nRegisters++;
        for ( k = 0; k < pCell->vRegs.nSize; k++ )
        {
            MstaRegCheck *pReg = MstaRegCheckArrayAt( &pCell->vRegs, k );
            int nDataPin = Msta_CellPinIndexOf( pCell, pReg->DataPin );
            int nClkPin  = Msta_CellPinIndexOf( pCell, pReg->ClkPin );
            int nDataNet;

            if ( nDataPin < 0 || nClkPin < 0 )
                continue;
            nDataNet = pInst->pNets[nDataPin];
            if ( nDataNet == MSTA_NO_ID || pInst->pNets[nClkPin] == MSTA_NO_ID )
                continue;
            if ( MstaNetArrayAt( &pDes->vNets, nDataNet )->fConst )
                continue;

            pNew = MstaCheckArrayAppend( &p->vChecks );
            pNew->CaptureClock       = MSTA_NO_ID;
            pNew->Setup.CaptureClock = MSTA_NO_ID;
            pNew->Hold.CaptureClock  = MSTA_NO_ID;
            pNew->Setup.Group        = MSTA_NO_ID;
            pNew->Hold.Group         = MSTA_NO_ID;
            pNew->InstId             = (MstaId)i;
            pNew->nCheck             = k;
            pNew->nDataPin           = nDataPin;
            pNew->nEndNet            = nDataNet;
            pNew->fToRegister        = 1;
        }
        for ( k = 0; k < pCell->vAsync.nSize; k++ )
        {
            MstaAsyncCheck *pAsync = MstaAsyncCheckArrayAt( &pCell->vAsync, k );
            int nAsyncPin = Msta_CellPinIndexOf( pCell, pAsync->AsyncPin );
            int nClkPin   = Msta_CellPinIndexOf( pCell, pAsync->ClkPin );
            int nAsyncNet;
            if ( nAsyncPin < 0 || nClkPin < 0 )
                continue;
            nAsyncNet = pInst->pNets[nAsyncPin];
            if ( nAsyncNet == MSTA_NO_ID || pInst->pNets[nClkPin] == MSTA_NO_ID )
                continue;
            if ( pAsync->RecoveryArc != MSTA_NO_ID || pAsync->RecoveryFallArc != MSTA_NO_ID )
            {
                pNew = MstaCheckArrayAppend( &p->vChecks );
                pNew->CaptureClock       = MSTA_NO_ID;
                pNew->Setup.CaptureClock = MSTA_NO_ID;
                pNew->Hold.CaptureClock  = MSTA_NO_ID;
                pNew->Setup.Group        = MSTA_NO_ID;
                pNew->Hold.Group         = MSTA_NO_ID;
                pNew->InstId = (MstaId)i;
                pNew->nAsyncCheck = k;
                pNew->nEndNet = nAsyncNet;
                pNew->fToRegister = 1;
                pNew->fAsync = 1;
                pNew->fRecovery = 1;
            }
            if ( pAsync->RemovalArc != MSTA_NO_ID || pAsync->RemovalFallArc != MSTA_NO_ID )
            {
                pNew = MstaCheckArrayAppend( &p->vChecks );
                pNew->CaptureClock       = MSTA_NO_ID;
                pNew->Setup.CaptureClock = MSTA_NO_ID;
                pNew->Hold.CaptureClock  = MSTA_NO_ID;
                pNew->Setup.Group        = MSTA_NO_ID;
                pNew->Hold.Group         = MSTA_NO_ID;
                pNew->InstId = (MstaId)i;
                pNew->nAsyncCheck = k;
                pNew->nEndNet = nAsyncNet;
                pNew->fToRegister = 1;
                pNew->fAsync = 1;
                pNew->fRemoval = 1;
            }
        }
    }

    for ( i = 0; i < pDes->vNets.nSize; i++ )
    {
        MstaNet *pNet = MstaNetArrayAt( &pDes->vNets, i );
        MstaCheck *pNew;
        if ( !pNet->fTopPort || pNet->Dir != MSTA_PORT_OUT || pNet->fConst )
            continue;
        pNew = MstaCheckArrayAppend( &p->vChecks );
        pNew->CaptureClock       = MSTA_NO_ID;
        pNew->Setup.CaptureClock = MSTA_NO_ID;
        pNew->Hold.CaptureClock  = MSTA_NO_ID;
        pNew->Setup.Group        = MSTA_NO_ID;
        pNew->Hold.Group         = MSTA_NO_ID;
        pNew->InstId             = MSTA_NO_ID;
        pNew->nEndNet            = i;
        pNew->fToRegister        = 0;
    }
    p->nEndpoints = p->vChecks.nSize;
}

void Msta_TiWarnUntimedLatches( MstaTiming *p )
{
    int i, n = 0;
    for ( i = 0; i < p->vChecks.nSize; i++ )
    {
        MstaCheck *pCheck = MstaCheckArrayAt( &p->vChecks, i );
        MstaInst *pInst;
        MstaRegCheck *pReg;
        int nClkNet;
        if ( !pCheck->fToRegister )
            continue;
        pInst = Msta_TiInstAt( p, pCheck->InstId );
        if ( !pInst->pCell->fLatch )
            continue;
        pReg = MstaRegCheckArrayAt( &pInst->pCell->vRegs, pCheck->nCheck );
        nClkNet = Msta_TiInstPinNet( p, pCheck->InstId, pReg->ClkPin );
        if ( nClkNet >= 0 && !p->pfClockNet[nClkNet] )
            n++;
    }
    if ( n > 0 )
        Msta_WarnOnce( "%d 个锁存器的使能端不是时钟网络；它们按未约束端点报告", n );
}

int Msta_TiIsStartForClock( MstaTiming *p, int nNet, int nClock )
{
    MstaNet *pNet = MstaNetArrayAt( &p->pDes->vNets, nNet );
    MstaInst *pInst;
    MstaPin *pPin;
    int i;
    if ( p->pfClockNet[nNet] )
        return 0;
    if ( pNet->Driver.InstId == MSTA_NO_ID )
        return Msta_IsSet(Msta_SdcPortDelay(p->pSdc, nNet, nClock, 0, 1)) ||
               Msta_IsSet(Msta_SdcPortDelay(p->pSdc, nNet, nClock, 0, 0));
    pInst = Msta_TiInstAt( p, pNet->Driver.InstId );
    if ( !pInst->pCell->fSequential )
        return 0;
    pPin = MstaPinArrayAt( &pInst->pCell->vPins, pNet->Driver.PinId );
    for ( i = 0; i < pInst->pCell->vRegs.nSize; i++ )
    {
        MstaRegCheck *pReg = MstaRegCheckArrayAt( &pInst->pCell->vRegs, i );
        int nClkNet, fClkRises;
        if ( Msta_TiRegClkToQArc(p, pInst->pCell, pReg, pPin->Name, &fClkRises, 1, pNet->Driver.InstId) == NULL &&
             Msta_TiRegClkToQArc(p, pInst->pCell, pReg, pPin->Name, &fClkRises, 0, pNet->Driver.InstId) == NULL )
            continue;
        nClkNet = Msta_TiInstPinNet( p, pNet->Driver.InstId, pReg->ClkPin );
        if ( nClkNet >= 0 && Msta_TiNetHasDataClock(p, nClock, nClkNet) &&
             Msta_TiClockArrAt(p, nClock, nClkNet)->fReached )
            return 1;
    }
    return 0;
}

static int CheckHasEdge( MstaTiming *p, const MstaArc *pArc, int nInst,
                         int fMax, int fDataRise )
{
    pArc = Msta_TiTimingArcForCheckCorner( p, pArc, nInst, fMax );
    if ( pArc == NULL || (!Msta_TableExists(&pArc->ConstraintRise) &&
                          !Msta_TableExists(&pArc->ConstraintFall)) )
        return 1;
    return Msta_TableExists( fDataRise ? &pArc->ConstraintRise : &pArc->ConstraintFall );
}

static double CheckTime( MstaTiming *p, const MstaArc *pArc, int nInst,
                         int nClkPin, int nDataPin,
                         int nClock, int nClkNet, int nDataNet, int fMax, int fDataRise )
{
    int nCapCorner = Msta_SdcCaptureCorner( p->pSdc, fMax );
    double ClkDrvSlew, ClkSlew, DataDrvSlew, DataSlew, UnusedW = 0.0;
    MstaClockArr *pClkArr;
    pArc = Msta_TiTimingArcForCheckCorner( p, pArc, nInst, fMax );
    if ( pArc == NULL )
        return 0.0;
    pClkArr = Msta_TiClockArrAt( p, nClock, nClkNet );
    ClkDrvSlew = Msta_TiClockSlew( pClkArr, nCapCorner,
                                   Msta_TiArcClockRises(pArc), p->CaptureClockTag );
    ClkSlew = ClkDrvSlew;
    if ( nClkPin >= 0 )
        Msta_TiWireToPin( p, nClkNet, nInst, nClkPin, nCapCorner,
                          Msta_TiArcClockRises(pArc), 1, ClkDrvSlew, &UnusedW, &ClkSlew );
    DataDrvSlew = fDataRise ? Msta_TiCornerOf(p, fMax)->pSlewRise[nDataNet]
                            : Msta_TiCornerOf(p, fMax)->pSlewFall[nDataNet];
    DataSlew = DataDrvSlew;
    if ( nDataPin >= 0 )
        Msta_TiWireToPin( p, nDataNet, nInst, nDataPin, fMax, fDataRise, 0,
                          DataDrvSlew, &UnusedW, &DataSlew );
    if ( ClkSlew < 0.0 )
        ClkSlew = MSTA_DEFAULT_SLEW;
    if ( DataSlew < 0.0 )
        DataSlew = MSTA_DEFAULT_SLEW;
    return Msta_TiEvalConstraint( p, pArc, nInst, fMax, fDataRise, ClkSlew, DataSlew )
         * Msta_TiVarCheckScale( p, nInst, fMax, fDataRise );
}

/* =====================================================================
   6b. 捕获沿对齐与路径对象收集
   ===================================================================== */

static int Gcd( int a, int b )
{
    while ( b != 0 ) { int t = a % b; a = b; b = t; }
    return a < 0 ? -a : a;
}

static void FindClockPairEdges( MstaTiming *p, int nLaunchClock, int fLaunchRises,
                                int nCaptureClock, int fCaptureRises, int fSetup,
                                int *pnLaunchCycle, int *pnCaptureCycle )
{
    MstaClock *pLaunch = Msta_SdcClockByIndex( p->pSdc, nLaunchClock );
    MstaClock *pCapture = Msta_SdcClockByIndex( p->pSdc, nCaptureClock );
    double LaunchPhase = pLaunch ? ( fLaunchRises ? pLaunch->RiseEdge : pLaunch->FallEdge ) : 0.0;
    double CapturePhase = pCapture ? ( fCaptureRises ? pCapture->RiseEdge : pCapture->FallEdge ) : 0.0;
    double LaunchPeriod = Msta_TiClockPeriod( p, nLaunchClock );
    double CapturePeriod = Msta_TiClockPeriod( p, nCaptureClock );
    int i, k, kMax, fWarned = 0;
    double Best;

    *pnLaunchCycle = 0;
    *pnCaptureCycle = 0;

    for ( i = 0; i < p->nClockPairs; i++ )
    {
        MstaClockPair *pPair = &p->vClockPairs[i];
        if ( pPair->fValid && pPair->LaunchClock == nLaunchClock &&
             pPair->CaptureClock == nCaptureClock &&
             pPair->LaunchRises == fLaunchRises && pPair->CaptureRises == fCaptureRises &&
             pPair->fSetup == fSetup )
        {
            *pnLaunchCycle = pPair->LaunchCycle;
            *pnCaptureCycle = pPair->CaptureCycle;
            return;
        }
    }
    if ( LaunchPeriod <= 0.0 || CapturePeriod <= 0.0 )
        return;

    kMax = 1000;
    {
        int nLaunchPs = (int)( LaunchPeriod + 0.5 ), nCapturePs = (int)( CapturePeriod + 0.5 );
        int g = Gcd( nLaunchPs, nCapturePs );
        if ( g > 0 )
        {
            kMax = ( nCapturePs + g - 1 ) / g;
            if ( kMax > 1000 || kMax <= 0 ) { kMax = 1000; fWarned = 1; }
        }
    }

    Best = MSTA_NO_TIME;
    for ( k = 0; k <= kMax; k++ )
    {
        double Launch = LaunchPhase + (double)k * LaunchPeriod;
        double Delta;
        int m = (int)floor( ( Launch - CapturePhase ) / CapturePeriod );
        if ( fSetup )
        {
            while ( CapturePhase + (double)m * CapturePeriod <= Launch + 1e-9 )
                m++;
            Delta = CapturePhase + (double)m * CapturePeriod - Launch;
        }
        else
        {
            while ( CapturePhase + (double)m * CapturePeriod > Launch + 1e-9 )
                m--;
            Delta = Launch - ( CapturePhase + (double)m * CapturePeriod );
        }
        if ( Delta < Best )
        {
            Best = Delta;
            *pnLaunchCycle = k;
            *pnCaptureCycle = m;
        }
    }
    if ( fWarned )
        Msta_WarnOnce( "时钟周期之比太大；发出/捕获边沿的搜索只看前 1000 个发出周期" );
    if ( p->nClockPairs < MSTA_MAX_CLOCK_PAIRS )
    {
        MstaClockPair *pPair = &p->vClockPairs[p->nClockPairs++];
        pPair->LaunchClock = nLaunchClock;
        pPair->CaptureClock = nCaptureClock;
        pPair->LaunchRises = fLaunchRises;
        pPair->CaptureRises = fCaptureRises;
        pPair->fSetup = fSetup;
        pPair->LaunchCycle = *pnLaunchCycle;
        pPair->CaptureCycle = *pnCaptureCycle;
        pPair->fValid = 1;
    }
}

static double CaptureEdgeTime( MstaTiming *p, int fToRegister, int nCaptureNet,
                               int nPortNet, int nCapInst, int nCapClkPin,
                               int nLaunchClock, int fLaunchRises,
                               int nCaptureClock, int fCaptureRises,
                               int nCycles, int fSetup, int *pnLaunchCycle, int fLocalCapture )
{
    double CapturePeriod = Msta_TiClockPeriod( p, nCaptureClock );
    MstaClock *pCaptureClock = Msta_SdcClockByIndex( p->pSdc, nCaptureClock );
    double CapturePhase = pCaptureClock ? (fCaptureRises ? pCaptureClock->RiseEdge : pCaptureClock->FallEdge) : 0.0;
    double Arrival = 0.0, SourceEdge = CapturePhase;
    int k = 0, m = 0;

    FindClockPairEdges( p, nLaunchClock, fLaunchRises, nCaptureClock, fCaptureRises,
                        fSetup, &k, &m );
    SourceEdge += (double)m * CapturePeriod;
    SourceEdge += (double)(nCycles - 1) * CapturePeriod;
    if ( pnLaunchCycle != NULL )
        *pnLaunchCycle = k;
    if ( fToRegister )
    {
        if ( nCaptureClock >= 0 )
        {
            int nCapCorner = Msta_SdcCaptureCorner( p->pSdc, fSetup );
            MstaClockArr *pClkArr = Msta_TiClockArrAt( p, nCaptureClock, nCaptureNet );
            double ClkDrvSlew = Msta_TiClockSlew( pClkArr, nCapCorner, fLocalCapture, fCaptureRises );
            double WireD = 0.0, UnusedSlew;
            Arrival = Msta_TiClockArrival( pClkArr, nCapCorner, fLocalCapture, fCaptureRises );
            if ( nCapInst >= 0 && nCapClkPin >= 0 )
            {
                Msta_TiWireToPin( p, nCaptureNet, nCapInst, nCapClkPin, nCapCorner,
                                  fLocalCapture, 1, ClkDrvSlew, &WireD, &UnusedSlew );
                Arrival += WireD;
            }
        }
        else
            Arrival = 0.0;
    }
    else if ( pCaptureClock )
    {
        if ( nCaptureNet >= 0 )
        {
            MstaClockArr *pRefArr = Msta_TiClockArrAt( p, nCaptureClock, nCaptureNet );
            Arrival = Msta_TiClockArrival( pRefArr, Msta_SdcCaptureCorner(p->pSdc, fSetup),
                                           fLocalCapture, fCaptureRises );
        }
        else
        {
            int fSourceIncluded = Msta_SdcPortSourceLatencyIncluded(
                p->pSdc, nPortNet, nCaptureClock, 1, fSetup );
            int fNetworkIncluded = Msta_SdcPortNetworkLatencyIncluded(
                p->pSdc, nPortNet, nCaptureClock, 1, fSetup );
            if ( !fSourceIncluded )
                Arrival += Msta_TiClockSourceArrival( p, nCaptureClock,
                                                      Msta_SdcCaptureCorner(p->pSdc, fSetup),
                                                      fCaptureRises );
            if ( !pCaptureClock->fPropagated && !fNetworkIncluded )
                Arrival += pCaptureClock->NetworkLatency[Msta_SdcCaptureCorner(p->pSdc, fSetup)][fCaptureRises];
        }
    }
    return Arrival + SourceEdge;
}

static double LaunchCycleShift( MstaTiming *p, int nClock, int nLaunchCycle )
{
    return ( nLaunchCycle > 0 ) ? (double)nLaunchCycle * Msta_TiClockPeriod( p, nClock ) : 0.0;
}

void Msta_TiStartEndpoint( MstaTiming *p, int nNet, MstaSdcObject *pObj )
{
    MstaNet *pNet = MstaNetArrayAt( &p->pDes->vNets, nNet );
    pObj->pText = Msta_NetName( p->pDes, nNet );
    pObj->nInst = -1;
    pObj->nPin  = -1;
    pObj->nNet  = nNet;
    pObj->fRises = -1;
    if ( pNet->Driver.InstId != MSTA_NO_ID )
    {
        pObj->pText = Msta_InstName( p->pDes, pNet->Driver.InstId );
        pObj->nInst = pNet->Driver.InstId;
        pObj->nPin  = pNet->Driver.PinId;
    }
}

static void EndEndpoint( MstaTiming *p, MstaCheck *pCheck, int nDataPin, MstaSdcObject *pObj )
{
    if ( pCheck->fToRegister )
    {
        pObj->pText = Msta_InstName( p->pDes, pCheck->InstId );
        pObj->nInst = pCheck->InstId;
        pObj->nPin  = nDataPin;
        pObj->nNet  = pCheck->nEndNet;
    }
    else
    {
        pObj->pText = Msta_NetName( p->pDes, pCheck->nEndNet );
        pObj->nInst = -1;
        pObj->nPin  = -1;
        pObj->nNet  = pCheck->nEndNet;
    }
    pObj->fRises = -1;
}

int Msta_TiPathEndRise( MstaTiming *p, int nNet, int fMax )
{
    double Rise = Msta_TiCornerOf(p, fMax)->pArrRise[nNet];
    double Fall = Msta_TiCornerOf(p, fMax)->pArrFall[nNet];
    return fMax ? ( Rise >= Fall ) : ( Rise <= Fall );
}

static int CollectEdgePath( MstaTiming *p, int nEndNet, int fMax, int fEndRise,
                            int *pNets, char *pRises, int *pLoadPins, int nCap )
{
    MstaPrev *pPrevR = Msta_TiCornerOf(p, fMax)->pPrevRise;
    MstaPrev *pPrevF = Msta_TiCornerOf(p, fMax)->pPrevFall;
    int n = nEndNet, e = fEndRise, Count = 0;
    if ( e < 0 )
        e = Msta_TiPathEndRise( p, nEndNet, fMax );
    while ( n >= 0 && Count < nCap )
    {
        MstaPrev *pPrev = e ? pPrevR : pPrevF;
        pNets[Count]  = n;
        pRises[Count] = (char)e;
        if ( pLoadPins != NULL )
            pLoadPins[Count] = pPrev[n].LoadPin;
        Count++;
        if ( pPrev[n].Net < 0 )
            break;
        e = pPrev[n].Edge;
        n = pPrev[n].Net;
    }
    return Count;
}

static int TimingStartNetEdge( MstaTiming *p, int nNet, int fMax, int fRise, int *pfStartRise )
{
    int Guard = 0;
    while ( nNet >= 0 && Guard++ < p->pDes->vNets.nSize )
    {
        MstaPrev Prev = (fRise ? Msta_TiCornerOf(p, fMax)->pPrevRise
                               : Msta_TiCornerOf(p, fMax)->pPrevFall)[nNet];
        if ( Prev.Net < 0 )
            break;
        nNet = Prev.Net;
        fRise = Prev.Edge;
    }
    if ( pfStartRise )
        *pfStartRise = fRise;
    return nNet;
}

static int CollectPathObjects( MstaTiming *p, int nEndNet, int fMax, int fEndRise,
                               const MstaSdcObject *pEndObj, MstaSdcObject **ppObjects )
{
    int aNets[MSTA_PATH_OBJECTS], aLoadPins[MSTA_PATH_OBJECTS];
    char aRises[MSTA_PATH_OBJECTS];
    int *pNets = aNets, *pLoadPins = aLoadPins;
    char *pRises = aRises;
    int nNetCap = p->pDes->vNets.nSize + 2;
    int nNeedObj = 3 * nNetCap + 8;
    int nCount, i, Count = 0;

    if ( nNeedObj > p->nPathObjectsCap )
    {
        MstaSdcObject *pBuf = (MstaSdcObject *)realloc(
            p->pPathObjects, (size_t)nNeedObj * sizeof(MstaSdcObject) );
        assert( pBuf );
        p->pPathObjects = pBuf;
        p->nPathObjectsCap = nNeedObj;
    }
    if ( nNetCap > MSTA_PATH_OBJECTS )
    {
        pNets = (int *)malloc( (size_t)nNetCap * sizeof(int) );
        pLoadPins = (int *)malloc( (size_t)nNetCap * sizeof(int) );
        pRises = (char *)malloc( (size_t)nNetCap * sizeof(char) );
        assert( pNets && pLoadPins && pRises );
    }
    nCount = CollectEdgePath( p, nEndNet, fMax, fEndRise, pNets, pRises, pLoadPins, nNetCap );
    for ( i = nCount - 1; i >= 0; i-- )
    {
        MstaNet *pNet = MstaNetArrayAt( &p->pDes->vNets, pNets[i] );
        if ( pNet->Driver.InstId != MSTA_NO_ID )
        {
            if ( i + 1 < nCount && pLoadPins[i] >= 0 )
            {
                MstaSdcObject *pObj = &p->pPathObjects[Count++];
                pObj->pText  = NULL;
                pObj->nInst  = pNet->Driver.InstId;
                pObj->nPin   = pLoadPins[i];
                pObj->nNet   = pNets[i + 1];
                pObj->fRises = pRises[i + 1];
            }
            {
                MstaSdcObject *pObj = &p->pPathObjects[Count++];
                pObj->pText  = Msta_InstName( p->pDes, pNet->Driver.InstId );
                pObj->nInst  = pNet->Driver.InstId;
                pObj->nPin   = pNet->Driver.PinId;
                pObj->nNet   = pNets[i];
                pObj->fRises = pRises[i];
            }
        }
        {
            MstaSdcObject *pObj = &p->pPathObjects[Count++];
            pObj->pText  = Msta_NetName( p->pDes, pNets[i] );
            pObj->nInst  = -1;
            pObj->nPin   = -1;
            pObj->nNet   = pNets[i];
            pObj->fRises = pRises[i];
        }
    }
    if ( pEndObj != NULL )
        p->pPathObjects[Count++] = *pEndObj;
    if ( pNets != aNets )
    {
        free( pNets );
        free( pLoadPins );
        free( pRises );
    }
    *ppObjects = p->pPathObjects;
    return Count;
}

static int EndpointRises( MstaTiming *p, MstaCheck *pCheck, MstaRegCheck *pReg,
                          int nCaptureClock, int fSetup )
{
    int fRise, nRef;
    if ( pCheck->fToRegister )
    {
        int nClkNet;
        if ( pReg == NULL )
            return -1;
        nClkNet = Msta_TiInstPinNet( p, pCheck->InstId, pReg->ClkPin );
        return p->CaptureClockTag >= 0 ? p->CaptureClockTag
             : Msta_TiEffectiveClkRises( p, nCaptureClock, nClkNet, pReg->fClkRises );
    }
    fRise = !Msta_SdcPortClockFall( p->pSdc, pCheck->nEndNet, nCaptureClock, 1, fSetup );
    nRef = Msta_SdcPortReferenceNet( p->pSdc, pCheck->nEndNet, nCaptureClock, 1, fSetup );
    if ( nRef >= 0 && p->CaptureClockTag >= 0 )
        return p->CaptureClockTag;
    return fRise;
}

static void BuildStartEndpoint( MstaTiming *p, int nNet, int fMax,
                                MstaSdcEndpoint *pEnd, MstaSdcObject *pObj )
{
    Msta_TiStartEndpoint( p, nNet, pObj );
    pEnd->pObj   = pObj;
    pEnd->pClock = Msta_NameStr( Msta_SdcClockByIndex( p->pSdc,
                       Msta_TiCornerOf(p, fMax)->pnLaunchClock[nNet] )->Name );
    pEnd->fRises = Msta_TiCornerOf(p, fMax)->pfLaunchRises[nNet];
}

static void BuildEndEndpoint( MstaTiming *p, MstaCheck *pCheck, MstaRegCheck *pReg,
                              int nCaptureClock, int fSetup,
                              MstaSdcEndpoint *pEnd, MstaSdcObject *pObj )
{
    int nDataPin = -1;
    if ( pCheck->fToRegister && pReg )
    {
        MstaInst *pInst = Msta_TiInstAt( p, pCheck->InstId );
        nDataPin = Msta_CellPinIndexOf( pInst->pCell, pReg->DataPin );
    }
    EndEndpoint( p, pCheck, nDataPin, pObj );
    pEnd->pObj   = pObj;
    pEnd->pClock = Msta_NameStr( Msta_SdcClockByIndex(p->pSdc, nCaptureClock)->Name );
    pEnd->fRises = EndpointRises( p, pCheck, pReg, nCaptureClock, fSetup );
}

static MstaId PathGroupOf( MstaTiming *p, int nCaptureClock,
                           const MstaSdcEndpoint *pFrom, const MstaSdcEndpoint *pTo,
                           const MstaSdcObject *pObjects, int nObjects )
{
    MstaId nGroup = Msta_SdcFindPathGroup( p->pSdc, p->pDes, pFrom, pTo, pObjects, nObjects );
    MstaClock *pClock;
    if ( nGroup != MSTA_NO_ID )
        return nGroup;
    pClock = Msta_SdcClockByIndex( p->pSdc, nCaptureClock );
    return pClock ? pClock->Name : MSTA_NO_ID;
}

void Msta_TiFreeCheckCornerPath( MstaCheckCorner *pC )
{
    free( pC->pPath );
    free( pC->pPathArrival );
    free( pC->pPathEdge );
    free( pC->pPathLoadPin );
    pC->pPath = NULL;
    pC->pPathArrival = NULL;
    pC->pPathEdge = NULL;
    pC->pPathLoadPin = NULL;
    pC->nPath = 0;
}

void Msta_TiFreeCheckPaths( MstaCheck *pCheck )
{
    Msta_TiFreeCheckCornerPath( &pCheck->Setup );
    Msta_TiFreeCheckCornerPath( &pCheck->Hold );
}

static void SaveCheckCorner( MstaTiming *p, MstaCheck *pBest,
                             const MstaCheck *pCandidate, int fMax )
{
    MstaCorner *pCorner = Msta_TiCornerOf( p, fMax );
    MstaCheckCorner *pDst = fMax ? &pBest->Setup : &pBest->Hold;
    const MstaCheckCorner *pSrc = fMax ? &pCandidate->Setup : &pCandidate->Hold;
    int n, Count = 0, i, Edge;

    Msta_TiFreeCheckCornerPath( pDst );
    *pDst = *pSrc;
    pDst->fChecked = 1;
    pDst->pPath = NULL;
    pDst->pPathArrival = NULL;
    pDst->pPathEdge = NULL;
    pDst->pPathLoadPin = NULL;
    pDst->nPath = 0;
    pBest->CaptureClock = pCandidate->CaptureClock;
    if ( pCandidate->fPathSaved )
    {
        if ( pSrc->nPath > 0 && pSrc->pPath != NULL && pSrc->pPathArrival != NULL )
        {
            pDst->pPath = (int *)malloc( (size_t)pSrc->nPath * sizeof(int) );
            pDst->pPathArrival = (double *)malloc( (size_t)pSrc->nPath * sizeof(double) );
            pDst->pPathEdge = (int *)malloc( (size_t)pSrc->nPath * sizeof(int) );
            pDst->pPathLoadPin = (int *)malloc( (size_t)pSrc->nPath * sizeof(int) );
            assert( pDst->pPath && pDst->pPathArrival && pDst->pPathEdge && pDst->pPathLoadPin );
            memcpy( pDst->pPath, pSrc->pPath, (size_t)pSrc->nPath * sizeof(int) );
            memcpy( pDst->pPathArrival, pSrc->pPathArrival, (size_t)pSrc->nPath * sizeof(double) );
            if ( pSrc->pPathEdge )
                memcpy( pDst->pPathEdge, pSrc->pPathEdge, (size_t)pSrc->nPath * sizeof(int) );
            else
                memset( pDst->pPathEdge, 0, (size_t)pSrc->nPath * sizeof(int) );
            if ( pSrc->pPathLoadPin )
                memcpy( pDst->pPathLoadPin, pSrc->pPathLoadPin, (size_t)pSrc->nPath * sizeof(int) );
            else
                for ( i = 0; i < pSrc->nPath; i++ )
                    pDst->pPathLoadPin[i] = -1;
            pDst->nPath = pSrc->nPath;
        }
        pBest->fPathSaved = 1;
        return;
    }
    Edge = pSrc->fDataRise;
    for ( n = pBest->nEndNet; n >= 0 && Count < p->pDes->vNets.nSize; )
    {
        MstaPrev Prev = (Edge ? pCorner->pPrevRise : pCorner->pPrevFall)[n];
        Count++;
        n = Prev.Net;
        Edge = Prev.Edge;
    }
    pDst->pPath = (int *)malloc( (size_t)( Count > 0 ? Count : 1 ) * sizeof(int) );
    pDst->pPathArrival = (double *)malloc( (size_t)( Count > 0 ? Count : 1 ) * sizeof(double) );
    pDst->pPathEdge = (int *)malloc( (size_t)( Count > 0 ? Count : 1 ) * sizeof(int) );
    pDst->pPathLoadPin = (int *)malloc( (size_t)( Count > 0 ? Count : 1 ) * sizeof(int) );
    assert( pDst->pPath && pDst->pPathArrival && pDst->pPathEdge && pDst->pPathLoadPin );
    pDst->nPath = Count;
    Edge = pSrc->fDataRise;
    for ( n = pBest->nEndNet, i = 0; i < Count; i++ )
    {
        MstaPrev Prev = (Edge ? pCorner->pPrevRise : pCorner->pPrevFall)[n];
        pDst->pPath[i] = n;
        pDst->pPathArrival[i] = (Edge ? pCorner->pArrRise : pCorner->pArrFall)[n];
        pDst->pPathEdge[i] = Edge;
        pDst->pPathLoadPin[i] = Prev.LoadPin;
        n = Prev.Net;
        Edge = Prev.Edge;
    }
    pBest->fPathSaved = 1;
}

static void CopyCheckIdentity( MstaCheck *pDst, const MstaCheck *pSrc )
{
    pDst->InstId      = pSrc->InstId;
    pDst->nCheck      = pSrc->nCheck;
    pDst->nDataPin    = pSrc->nDataPin;
    pDst->nEndNet     = pSrc->nEndNet;
    pDst->fToRegister = pSrc->fToRegister;
    pDst->fAsync      = pSrc->fAsync;
    pDst->fRecovery   = pSrc->fRecovery;
    pDst->fRemoval    = pSrc->fRemoval;
    pDst->nAsyncCheck = pSrc->nAsyncCheck;
    pDst->CaptureClock       = MSTA_NO_ID;
    pDst->Setup.CaptureClock = MSTA_NO_ID;
    pDst->Hold.CaptureClock  = MSTA_NO_ID;
    pDst->Setup.Group        = MSTA_NO_ID;
    pDst->Hold.Group         = MSTA_NO_ID;
}

static void MergeEdgeReqArrays( MstaCheckCorner *pDst, const double EdgeReq[2],
                                const char fEdgeReq[2], int fMax )
{
    int e;
    for ( e = 0; e < 2; e++ )
    {
        if ( !fEdgeReq[e] )
            continue;
        if ( !pDst->fEdgeReq[e] ||
             ( fMax ? (EdgeReq[e] < pDst->EdgeReq[e])
                    : (EdgeReq[e] > pDst->EdgeReq[e]) ) )
        {
            pDst->EdgeReq[e] = EdgeReq[e];
            pDst->fEdgeReq[e] = 1;
        }
    }
}

static void KeepCornerCandidate( MstaTiming *p, MstaCheck *pBest,
                                 const MstaCheck *pCandidate, int fMax )
{
    MstaCheckCorner *pDst = Msta_CheckCorner( pBest, fMax );
    const MstaCheckCorner *pSrc = fMax ? &pCandidate->Setup : &pCandidate->Hold;
    double OldReq[2] = { pDst->EdgeReq[0], pDst->EdgeReq[1] };
    char fOld[2] = { pDst->fEdgeReq[0], pDst->fEdgeReq[1] };
    if ( pSrc->fChecked && (!pDst->fChecked || pSrc->Slack < pDst->Slack) )
    {
        SaveCheckCorner( p, pBest, pCandidate, fMax );
        MergeEdgeReqArrays( pDst, OldReq, fOld, fMax );
    }
    MergeEdgeReqArrays( pDst, pSrc->EdgeReq, pSrc->fEdgeReq, fMax );
}

static void KeepWorseCandidate( MstaTiming *p, MstaCheck *pBest, MstaCheck *pCandidate )
{
    if ( pCandidate->fCutByException )
        pBest->fCutByException = 1;
    KeepCornerCandidate( p, pBest, pCandidate, 1 );
    KeepCornerCandidate( p, pBest, pCandidate, 0 );
    Msta_TiFreeCheckPaths( pCandidate );
}

/* =====================================================================
   6c. 异步检查、锁存借时、例外与次优路径搜索
   ===================================================================== */

static void SearchCornerPath( MstaTiming *p, MstaCheck *pCheck, int nCaptureClock,
                              int nPropClock, int nStartClass, int fDataRise,
                              int fMax, int fAsync, int nDepth );

static void CheckAsyncEndpointEdge( MstaTiming *p, MstaCheck *pCheck, int nCaptureClock, int fDataRise,
                                    MstaPathExclude *pExclude, int *pnExclude,
                                    MstaPathExclude *pBranches, int *pnBranches )
{
    MstaInst *pInst = Msta_TiInstAt( p, pCheck->InstId );
    MstaCell *pCell = pInst->pCell;
    MstaAsyncCheck *pAsync = MstaAsyncCheckArrayAt( &pCell->vAsync, pCheck->nAsyncCheck );
    int nClkNet = Msta_TiInstPinNet( p, pCheck->InstId, pAsync->ClkPin );
    int nClock = nCaptureClock, nEndNet = pCheck->nEndNet, fStartRise;
    int nStart = TimingStartNetEdge( p, nEndNet, pCheck->fRecovery, fDataRise, &fStartRise );
    MstaCorner *pCorner = Msta_TiCornerOf( p, pCheck->fRecovery );
    int *pLaunchClock = pCorner->pnLaunchClock;
    double *pArrival = fDataRise ? pCorner->pArrRise : pCorner->pArrFall;
    MstaSdcObject FromObj, ToObj, *pPathObjects = NULL;
    MstaSdcEndpoint From, To;
    int nPathObjects, nCycles = 1, fFalse = 0;
    MstaId nArcId;
    MstaArc *pArc;
    double Check, Capture, Required, Shift, PathDelay, Cppr;
    int fClkRises, nLaunchCycle = 0;

    if ( nClkNet < 0 || nClock < 0 || pLaunchClock[nStart] < 0 ||
         !Msta_TiArrivalSet(pArrival[nEndNet], pCheck->fRecovery) )
        return;
    fClkRises = p->CaptureClockTag;
    if ( !Msta_TiClockTagValid(Msta_TiClockArrAt(p, nClock, nClkNet),
                                Msta_SdcCaptureCorner(p->pSdc, pCheck->fRecovery),
                                pAsync->fClkRises, fClkRises) )
        return;

    Msta_TiStartEndpoint( p, nStart, &FromObj );
    EndEndpoint( p, pCheck, Msta_CellPinIndexOf(pCell, pAsync->AsyncPin), &ToObj );
    From.pObj   = &FromObj;
    From.pClock = Msta_NameStr( Msta_SdcClockByIndex(p->pSdc, pLaunchClock[nStart])->Name );
    From.fRises = pCorner->pfLaunchRises[nStart];
    FromObj.fRises = fStartRise;
    ToObj.fRises = fDataRise;
    To.pObj     = &ToObj;
    To.pClock   = Msta_NameStr( Msta_SdcClockByIndex(p->pSdc, nClock)->Name );
    To.fRises   = fClkRises;
    nPathObjects = CollectPathObjects( p, nEndNet, pCheck->fRecovery, fDataRise,
                                       &ToObj, &pPathObjects );
    if ( pCheck->fRecovery ) pCheck->Setup.Group = Msta_NameId( MSTA_SDC_GROUP_ASYNC );
    else                     pCheck->Hold.Group  = Msta_NameId( MSTA_SDC_GROUP_ASYNC );
    Msta_SdcFindExceptionPath( p->pSdc, p->pDes, &From, &To, pCheck->fRecovery,
                               pPathObjects, nPathObjects, &nCycles, &fFalse );
    if ( fFalse )
    {
        pCheck->fCutByException = 1;
        if ( pExclude != NULL && pnExclude != NULL )
            *pnExclude += Msta_SdcPathExclusions( p->pSdc, p->pDes, &From, &To, pCheck->fRecovery,
                                                  pPathObjects, nPathObjects,
                                                  pExclude + *pnExclude,
                                                  MSTA_MAX_PATH_EXCLUDE - *pnExclude );
        if ( pBranches != NULL && pnBranches != NULL && ( pnExclude == NULL || *pnExclude == 0 ) )
            *pnBranches = Msta_SdcPathExclusionBranches( p->pSdc, p->pDes, &From, &To, pCheck->fRecovery,
                                                         pPathObjects, nPathObjects,
                                                         pBranches, MSTA_SDC_MAX_THRU );
        return;
    }

    int nClkPin = Msta_CellPinIndexOf( pCell, pAsync->ClkPin );
    double DataDrvSlew, EndWireD = 0.0, UnusedSlew;
    nArcId = pCheck->fRecovery
           ? (pAsync->RecoveryArc != MSTA_NO_ID ? pAsync->RecoveryArc : pAsync->RecoveryFallArc)
           : (pAsync->RemovalArc  != MSTA_NO_ID ? pAsync->RemovalArc  : pAsync->RemovalFallArc);
    pArc = Msta_CellArcById( pCell, nArcId );
    if ( pArc == NULL || !CheckHasEdge(p, pArc, pCheck->InstId, pCheck->fRecovery, fDataRise) )
        return;
    DataDrvSlew = fDataRise ? pCorner->pSlewRise[nEndNet] : pCorner->pSlewFall[nEndNet];
    Msta_TiWireToPin( p, nEndNet, pCheck->InstId, pCheck->nDataPin, pCheck->fRecovery, fDataRise, 0,
                      DataDrvSlew, &EndWireD, &UnusedSlew );
    Check = CheckTime( p, pArc, pCheck->InstId, nClkPin, pCheck->nDataPin,
                       nClock, nClkNet, nEndNet, pCheck->fRecovery, fDataRise );
    Capture = CaptureEdgeTime( p, 1, nClkNet, -1, pCheck->InstId, nClkPin,
                               pLaunchClock[nStart], pCorner->pfLaunchRises[nStart],
                               nClock, fClkRises, nCycles, pCheck->fRecovery, &nLaunchCycle, pAsync->fClkRises );
    Required = pCheck->fRecovery
             ? Capture - Msta_TiVarUncertainty(p, pLaunchClock[nStart], nClock, 1,
                                               pCorner->pfLaunchRises[nStart], fClkRises) - Check
             : Capture + Msta_TiVarUncertainty(p, pLaunchClock[nStart], nClock, 0,
                                               pCorner->pfLaunchRises[nStart], fClkRises) + Check;
    PathDelay = Msta_SdcFindPathDelay( p->pSdc, p->pDes, &From, &To, pPathObjects, nPathObjects, pCheck->fRecovery );
    Cppr = Msta_TiVarCpprCredit( p, nStart, pLaunchClock[nStart], nClock, nClkNet, pCheck->fRecovery,
                                 pCorner->pfLaunchRises[nStart], pAsync->fClkRises, fClkRises );
    if ( Msta_IsSet(PathDelay) )
        Cppr = 0;
    Required += pCheck->fRecovery ? Cppr : -Cppr;
    Msta_CheckCorner(pCheck, pCheck->fRecovery)->Cppr = Cppr;
    pCheck->CaptureClock = Msta_SdcClockByIndex( p->pSdc, nClock )->Name;
    if ( pCheck->fRecovery )
    {
        Shift = LaunchCycleShift( p, pLaunchClock[nStart], nLaunchCycle );
        pCheck->Setup.LaunchSourceRise = pCorner->pfLaunchRises[nStart];
        pCheck->Setup.CaptureSourceRise = fClkRises;
        pCheck->Setup.fDataRise = fDataRise;
        pCheck->Setup.Arrival = pArrival[nEndNet] + EndWireD + Shift;
        pCheck->Setup.Required = Required;
        pCheck->Setup.Slack = Required - pCheck->Setup.Arrival;
        pCheck->Setup.CheckTime = Check;
        pCheck->Setup.Uncertainty = Msta_TiVarUncertainty(p, pLaunchClock[nStart], nClock, 1,
                                                          pCorner->pfLaunchRises[nStart], fClkRises);
        pCheck->Setup.LaunchClock = Msta_SdcClockByIndex(p->pSdc, pLaunchClock[nStart])->Name;
        pCheck->Setup.CaptureClock = pCheck->CaptureClock;
        pCheck->Setup.LaunchTime = p->CornerMax.pdLaunchEdge[nStart] + Shift;
        pCheck->Setup.CaptureTime = Capture;
        pCheck->Setup.LaunchShift = Shift;
        pCheck->Setup.fPathBudget = 0;
        pCheck->Setup.fChecked = 1;
    }
    else
    {
        Shift = LaunchCycleShift( p, pLaunchClock[nStart], nLaunchCycle );
        pCheck->Hold.LaunchSourceRise = pCorner->pfLaunchRises[nStart];
        pCheck->Hold.CaptureSourceRise = fClkRises;
        pCheck->Hold.fDataRise = fDataRise;
        pCheck->Hold.Arrival = pArrival[nEndNet] + EndWireD + Shift;
        pCheck->Hold.Required = Required;
        pCheck->Hold.Slack = pCheck->Hold.Arrival - Required;
        pCheck->Hold.CheckTime = Check;
        pCheck->Hold.Uncertainty = Msta_TiVarUncertainty(p, pLaunchClock[nStart], nClock, 0,
                                                         pCorner->pfLaunchRises[nStart], fClkRises);
        pCheck->Hold.LaunchClock = Msta_SdcClockByIndex(p->pSdc, pLaunchClock[nStart])->Name;
        pCheck->Hold.CaptureClock = pCheck->CaptureClock;
        pCheck->Hold.LaunchTime = p->CornerMin.pdLaunchEdge[nStart] + Shift;
        pCheck->Hold.CaptureTime = Capture;
        pCheck->Hold.LaunchShift = Shift;
        pCheck->Hold.fPathBudget = 0;
        pCheck->Hold.fChecked = 1;
    }
    if ( Msta_IsSet(PathDelay) )
    {
        MstaCheckCorner *pResult = Msta_CheckCorner( pCheck, pCheck->fRecovery );
        pResult->CheckTime = PathDelay;
        pResult->fPathBudget = 1;
        pResult->Required = pResult->LaunchTime + PathDelay;
        pResult->Slack = pCheck->fRecovery ? pResult->Required - pResult->Arrival
                                           : pResult->Arrival - pResult->Required;
    }
    {
        MstaCheckCorner *pResult = Msta_CheckCorner( pCheck, pCheck->fRecovery );
        pResult->EdgeReq[fDataRise ? 1 : 0] = pResult->Required - pResult->LaunchShift - EndWireD;
        pResult->fEdgeReq[fDataRise ? 1 : 0] = 1;
    }
}

static void CheckAsyncEndpointOne( MstaTiming *p, MstaCheck *pCheck, int nCaptureClock,
                                   int nPropClock, int nStartClass )
{
    int Edge, Tag, fMax = pCheck->fRecovery;
    for ( Tag = 0; Tag < 2; Tag++ )
    for ( Edge = 1; Edge >= 0; Edge-- )
    {
        MstaCheck Candidate;
        p->CaptureClockTag = Tag;
        memset( &Candidate, 0, sizeof(Candidate) );
        CopyCheckIdentity( &Candidate, pCheck );
        SearchCornerPath( p, &Candidate, nCaptureClock, nPropClock, nStartClass, Edge, fMax, 1, 0 );
        if ( p->nPathExclude > 0 )
        {
            p->nPathExclude = 0;
            Msta_TiPropagateData( p, fMax, nPropClock, nStartClass );
        }
        KeepWorseCandidate( p, pCheck, &Candidate );
    }
    p->CaptureClockTag = -1;
    pCheck->fPathSaved = pCheck->Setup.fChecked || pCheck->Hold.fChecked;
}

static void CheckAsyncEndpoint( MstaTiming *p, MstaCheck *pCheck,
                                int nPropClock, int nStartClass )
{
    MstaInst *pInst = Msta_TiInstAt( p, pCheck->InstId );
    MstaAsyncCheck *pAsync = MstaAsyncCheckArrayAt( &pInst->pCell->vAsync, pCheck->nAsyncCheck );
    int nClkNet = Msta_TiInstPinNet( p, pCheck->InstId, pAsync->ClkPin );
    int c;

    if ( nClkNet < 0 )
        return;
    for ( c = 0; c < Msta_SdcClockCount( p->pSdc ); c++ )
    {
        MstaCheck Candidate;
        if ( !Msta_TiNetHasDataClock(p, c, nClkNet) )
            continue;
        memset( &Candidate, 0, sizeof(Candidate) );
        CopyCheckIdentity( &Candidate, pCheck );
        CheckAsyncEndpointOne( p, &Candidate, c, nPropClock, nStartClass );
        KeepWorseCandidate( p, pCheck, &Candidate );
    }
}

static double LatchOpenWindow( MstaClock *pClock, int fCloseRises )
{
    double d = fCloseRises ? ( pClock->RiseEdge - pClock->FallEdge )
                           : ( pClock->FallEdge - pClock->RiseEdge );
    while ( d <= 0.0 ) d += pClock->Period;
    return d;
}

static double LatchBorrow( MstaTiming *p, MstaCheck *pCheck, MstaCell *pCell,
                           int nCaptureClock, int fCloseRises,
                           double Capture, double *pRequired )
{
    MstaClock *pClock;
    double Borrow;
    if ( pCell == NULL || !pCell->fLatch )
        return MSTA_UNSET;
    Borrow = Msta_SdcMaxTimeBorrow( p->pSdc, pCheck->InstId );
    pClock = Msta_SdcClockByIndex( p->pSdc, nCaptureClock );
    if ( !Msta_IsSet(Borrow) || pClock == NULL )
        return MSTA_UNSET;
    *pRequired = Capture - LatchOpenWindow( pClock, fCloseRises ) + Borrow;
    return Borrow;
}

static int ApplyCornerExceptions( MstaTiming *p, MstaCheck *pCheck, MstaRegCheck *pReg,
                                  int nCaptureClock, int nStart, int fSetup, int fDataRise,
                                  int *pnCycles, double *pPathDelay,
                                  MstaPathExclude *pExclude, int *pnExclude,
                                  MstaPathExclude *pBranches, int *pnBranches )
{
    int nEndNet = pCheck->nEndNet;
    MstaSdcObject *pPathObjects = NULL;
    MstaSdcObject FromObj, ToObj;
    MstaSdcEndpoint From, To;
    int nPathObjects, fFalse = 0, fKeep = 1;
    BuildStartEndpoint( p, nStart, fSetup, &From, &FromObj );
    BuildEndEndpoint( p, pCheck, pReg, nCaptureClock, fSetup, &To, &ToObj );
    TimingStartNetEdge( p, nEndNet, fSetup, fDataRise, &FromObj.fRises );
    ToObj.fRises = fDataRise;
    nPathObjects = CollectPathObjects( p, nEndNet, fSetup, fDataRise, &ToObj, &pPathObjects );
    Msta_CheckCorner( pCheck, fSetup )->Group = PathGroupOf( p, nCaptureClock, &From, &To,
                                                             pPathObjects, nPathObjects );
    Msta_SdcFindExceptionPath( p->pSdc, p->pDes, &From, &To, fSetup,
                               pPathObjects, nPathObjects, pnCycles, &fFalse );
    if ( fFalse )
    {
        fKeep = 0;
        pCheck->fCutByException = 1;
        *pnExclude += Msta_SdcPathExclusions( p->pSdc, p->pDes, &From, &To, fSetup,
                                              pPathObjects, nPathObjects,
                                              pExclude + *pnExclude,
                                              MSTA_MAX_PATH_EXCLUDE - *pnExclude );
        if ( *pnExclude == 0 && pBranches != NULL && pnBranches != NULL )
            *pnBranches = Msta_SdcPathExclusionBranches( p->pSdc, p->pDes, &From, &To, fSetup,
                                                         pPathObjects, nPathObjects,
                                                         pBranches, MSTA_SDC_MAX_THRU );
    }
    *pPathDelay = Msta_SdcFindPathDelay( p->pSdc, p->pDes, &From, &To,
                                         pPathObjects, nPathObjects, fSetup );
    return fKeep;
}

static void EvaluateCheckCorner( MstaTiming *p, MstaCheck *pCheck, int fSetup,
                                 MstaCell *pCell, MstaRegCheck *pReg,
                                 int nCaptureClock, int nCaptureNet, int nStart,
                                 int nOutputRef, double OutDelay,
                                 int nCycles, double PathDelay, int fDataRise )
{
    MstaCorner *pCorner = Msta_TiCornerOf( p, fSetup );
    MstaCheckCorner *pResult = Msta_CheckCorner( pCheck, fSetup );
    int nEndNet = pCheck->nEndNet;
    int nLaunchClock = pCorner->pnLaunchClock[nStart];
    int nPortRef = pCheck->fToRegister ? -1 : nOutputRef;
    int fLocalCapture = pCheck->fToRegister ? pReg->fClkRises
        : !Msta_SdcPortClockFall( p->pSdc, nEndNet, nCaptureClock, 1, fSetup );
    int fCaptureRises = p->CaptureClockTag;
    int fLaunchRises = pCorner->pfLaunchRises[nStart];
    int nCapInst = pCheck->fToRegister ? pCheck->InstId : -1;
    int nCapClkPin = pCheck->fToRegister ? Msta_CellPinIndexOf(pCell, pReg->ClkPin) : -1;
    int nLaunchCycle = 0;
    double Capture, Check, Uncertainty, Required, Shift, EndWireD = 0.0, UnusedSlew;

    if ( pCheck->fToRegister )
    {
        double DataDrvSlew = fDataRise ? pCorner->pSlewRise[nEndNet] : pCorner->pSlewFall[nEndNet];
        Msta_TiWireToPin( p, nEndNet, pCheck->InstId, pCheck->nDataPin, fSetup, fDataRise, 0,
                          DataDrvSlew, &EndWireD, &UnusedSlew );
    }
    Capture = CaptureEdgeTime( p, pCheck->fToRegister,
                               pCheck->fToRegister ? nCaptureNet : nPortRef,
                               nEndNet, nCapInst, nCapClkPin,
                               nLaunchClock, fLaunchRises,
                               nCaptureClock, fCaptureRises,
                               nCycles, fSetup, &nLaunchCycle, fLocalCapture );
    Shift = LaunchCycleShift( p, nLaunchClock, nLaunchCycle );
    if ( pCheck->fToRegister )
    {
        MstaId nArc = fSetup
                    ? ( ( pReg->SetupArc != MSTA_NO_ID ) ? pReg->SetupArc : pReg->SetupFallArc )
                    : ( ( pReg->HoldArc  != MSTA_NO_ID ) ? pReg->HoldArc  : pReg->HoldFallArc );
        MstaArc *pArc = Msta_CellArcById( pCell, nArc );
        if ( pArc == NULL )
            return;
        if ( !CheckHasEdge(p, pArc, pCheck->InstId, fSetup, fDataRise) )
            return;
        Check = CheckTime( p, pArc, pCheck->InstId, nCapClkPin, pCheck->nDataPin,
                           nCaptureClock, nCaptureNet, nEndNet, fSetup, fDataRise );
    }
    else
        Check = fSetup ? OutDelay : -OutDelay;
    Uncertainty = Msta_TiVarUncertainty( p, nLaunchClock, nCaptureClock, fSetup,
                                         fLaunchRises, fCaptureRises );
    if ( fSetup )
    {
        Required = Capture - Uncertainty - Check;
        pResult->Borrow = LatchBorrow( p, pCheck, pCell, nCaptureClock, fCaptureRises,
                                       Capture, &Required );
    }
    else
        Required = Capture + Uncertainty + Check;
    pResult->Cppr = Msta_TiVarCpprCredit( p, nStart, nLaunchClock, nCaptureClock,
                                          pCheck->fToRegister ? nCaptureNet : nPortRef,
                                          fSetup, fLaunchRises, fLocalCapture, fCaptureRises );
    Required += fSetup ? pResult->Cppr : -pResult->Cppr;
    pResult->LaunchSourceRise = fLaunchRises;
    pResult->CaptureSourceRise = fCaptureRises;
    pResult->fDataRise   = fDataRise;
    pResult->Arrival     = (fDataRise ? pCorner->pArrRise[nEndNet] : pCorner->pArrFall[nEndNet])
                         + EndWireD + Shift;
    pResult->Required    = Required;
    pResult->Slack       = fSetup ? Required - pResult->Arrival : pResult->Arrival - Required;
    pResult->CheckTime   = Check;
    pResult->Uncertainty = Uncertainty;
    pResult->LaunchTime  = pCorner->pdLaunchEdge[nStart] + Shift;
    pResult->CaptureTime = Capture;
    pResult->LaunchClock = Msta_SdcClockByIndex(p->pSdc, nLaunchClock)->Name;
    pResult->CaptureClock = Msta_SdcClockByIndex(p->pSdc, nCaptureClock)->Name;
    pResult->LaunchShift = Shift;
    pResult->fPathBudget = 0;
    pResult->fChecked    = 1;
    if ( Msta_IsSet(PathDelay) )
    {
        pResult->Cppr = 0;
        pResult->CheckTime = PathDelay;
        pResult->fPathBudget = 1;
        pResult->Required = pResult->LaunchTime + PathDelay;
        pResult->Slack = fSetup ? pResult->Required - pResult->Arrival
                                : pResult->Arrival - pResult->Required;
    }
    pResult->EdgeReq[fDataRise ? 1 : 0] = pResult->Required - pResult->LaunchShift - EndWireD;
    pResult->fEdgeReq[fDataRise ? 1 : 0] = 1;
}

static int OutputClockTagValid( MstaTiming *p, int nClock, int nRefNet, int fSetup, int fLocalRise )
{
    if ( nRefNet < 0 )
        return p->CaptureClockTag == fLocalRise;
    return Msta_TiClockTagValid( Msta_TiClockArrAt(p, nClock, nRefNet),
                                 Msta_SdcCaptureCorner(p->pSdc, fSetup),
                                 fLocalRise, p->CaptureClockTag );
}

static void CheckEndpointAttempt( MstaTiming *p, MstaCheck *pCheck, int nCaptureClock,
                                  MstaPathExclude *pExclude, int *pnExclude,
                                  MstaPathExclude *pBranches, int *pnBranches,
                                  int fDataRise, int fWantSetup, int fWantHold )
{
    int nEndNet = pCheck->nEndNet;
    int nStartMax = fWantSetup ? TimingStartNetEdge( p, nEndNet, 1, fDataRise, NULL ) : -1;
    int nStartMin = fWantHold ? TimingStartNetEdge( p, nEndNet, 0, fDataRise, NULL ) : -1;
    int nCaptureNet = -1;
    int nOutputRefMax = -1, nOutputRefMin = -1;
    int fSetup = fWantSetup && Msta_TiArrivalSet(fDataRise ? p->CornerMax.pArrRise[nEndNet]
                                                           : p->CornerMax.pArrFall[nEndNet], 1)
                 && nStartMax >= 0 && p->CornerMax.pnLaunchClock[nStartMax] >= 0;
    int fHold = fWantHold && Msta_TiArrivalSet(fDataRise ? p->CornerMin.pArrRise[nEndNet]
                                                         : p->CornerMin.pArrFall[nEndNet], 0)
                && nStartMin >= 0 && p->CornerMin.pnLaunchClock[nStartMin] >= 0;
    int nSetupCycles = 1, nHoldCycles = 1;
    double OutMax = MSTA_UNSET, OutMin = MSTA_UNSET;
    double PathMax = MSTA_UNSET, PathMin = MSTA_UNSET;
    MstaCell *pCell = NULL;
    MstaRegCheck *pReg = NULL;
    MstaNetCons *pCons = NULL;

    if ( pCheck->fToRegister )
    {
        MstaInst *pInst = Msta_TiInstAt( p, pCheck->InstId );
        pCell = pInst->pCell;
        pReg  = MstaRegCheckArrayAt( &pCell->vRegs, pCheck->nCheck );
        nCaptureNet = Msta_TiInstPinNet( p, pCheck->InstId, pReg->ClkPin );
        if ( nCaptureNet < 0 )
            return;
    }
    else
    {
        pCons = Msta_SdcNetCons( p->pSdc, nEndNet );
        if ( pCons == NULL )
            return;
        OutMax = Msta_SdcPortDelay( p->pSdc, nEndNet, nCaptureClock, 1, 1 );
        OutMin = Msta_SdcPortDelay( p->pSdc, nEndNet, nCaptureClock, 1, 0 );
        nOutputRefMax = Msta_SdcPortReferenceNet( p->pSdc, nEndNet, nCaptureClock, 1, 1 );
        nOutputRefMin = Msta_SdcPortReferenceNet( p->pSdc, nEndNet, nCaptureClock, 1, 0 );
        if ( fSetup && !Msta_IsSet(OutMax) ) fSetup = 0;
        if ( fHold  && !Msta_IsSet(OutMin) ) fHold  = 0;
        if ( fSetup && nOutputRefMax >= 0 &&
             !Msta_TiClockArrAt(p, nCaptureClock, nOutputRefMax)->fReached )
            fSetup = 0;
        if ( fHold && nOutputRefMin >= 0 &&
             !Msta_TiClockArrAt(p, nCaptureClock, nOutputRefMin)->fReached )
            fHold = 0;
    }
    if ( nCaptureClock < 0 )
        return;

    if ( pCheck->fToRegister )
    {
        MstaClockArr *pClk = Msta_TiClockArrAt( p, nCaptureClock, nCaptureNet );
        if ( fSetup && !Msta_TiClockTagValid(pClk, Msta_SdcCaptureCorner(p->pSdc, 1), pReg->fClkRises, p->CaptureClockTag) )
            fSetup = 0;
        if ( fHold && !Msta_TiClockTagValid(pClk, Msta_SdcCaptureCorner(p->pSdc, 0), pReg->fClkRises, p->CaptureClockTag) )
            fHold = 0;
    }
    else
    {
        int fRiseMax = !Msta_SdcPortClockFall( p->pSdc, nEndNet, nCaptureClock, 1, 1 );
        int fRiseMin = !Msta_SdcPortClockFall( p->pSdc, nEndNet, nCaptureClock, 1, 0 );
        if ( fSetup && !OutputClockTagValid(p, nCaptureClock, nOutputRefMax, 1, fRiseMax) )
            fSetup = 0;
        if ( fHold && !OutputClockTagValid(p, nCaptureClock, nOutputRefMin, 0, fRiseMin) )
            fHold = 0;
    }
    if ( !fSetup && !fHold )
        return;

    if ( fSetup )
        fSetup = ApplyCornerExceptions( p, pCheck, pReg, nCaptureClock, nStartMax, 1, fDataRise,
                                        &nSetupCycles, &PathMax, pExclude, pnExclude,
                                        pBranches, pnBranches );
    if ( fHold )
        fHold = ApplyCornerExceptions( p, pCheck, pReg, nCaptureClock, nStartMin, 0, fDataRise,
                                       &nHoldCycles, &PathMin, pExclude, pnExclude,
                                       pBranches, pnBranches );
    if ( !fSetup && !fHold )
        return;

    pCheck->CaptureClock = Msta_SdcClockByIndex( p->pSdc, nCaptureClock )->Name;
    pCheck->Setup.LaunchClock = pCheck->Hold.LaunchClock = MSTA_NO_ID;
    if ( fSetup )
        EvaluateCheckCorner( p, pCheck, 1, pCell, pReg, nCaptureClock, nCaptureNet, nStartMax,
                             nOutputRefMax, OutMax, nSetupCycles, PathMax, fDataRise );
    if ( fHold )
        EvaluateCheckCorner( p, pCheck, 0, pCell, pReg, nCaptureClock, nCaptureNet, nStartMin,
                             nOutputRefMin, OutMin, nHoldCycles, PathMin, fDataRise );
}

static int AddPathExclusions( MstaTiming *p, const MstaPathExclude *pNew, int nNew )
{
    int i, j, fAny = 0;
    for ( i = 0; i < nNew; i++ )
    {
        if ( pNew[i].Net < 0 || pNew[i].EdgeMask == 0 )
            continue;
        for ( j = 0; j < p->nPathExclude; j++ )
        {
            int fNew;
            if ( p->vPathExclude[j].Net != pNew[i].Net ||
                 p->vPathExclude[j].Inst != pNew[i].Inst ||
                 p->vPathExclude[j].Pin != pNew[i].Pin )
                continue;
            fNew = pNew[i].EdgeMask & ~p->vPathExclude[j].EdgeMask;
            if ( fNew != 0 )
            {
                p->vPathExclude[j].EdgeMask |= pNew[i].EdgeMask;
                fAny = 1;
            }
            break;
        }
        if ( j == p->nPathExclude )
        {
            if ( p->nPathExclude >= MSTA_MAX_PATH_EXCLUDE )
                continue;
            p->vPathExclude[p->nPathExclude++] = pNew[i];
            fAny = 1;
        }
    }
    return fAny;
}

static void SearchCornerPath( MstaTiming *p, MstaCheck *pCheck, int nCaptureClock,
                              int nPropClock, int nStartClass, int fDataRise,
                              int fMax, int fAsync, int nDepth )
{
    int nIter;
    for ( nIter = 0; nIter <= MSTA_MAX_PATH_EXCLUDE; nIter++ )
    {
        MstaPathExclude vNew[MSTA_MAX_PATH_EXCLUDE];
        MstaPathExclude vBranches[MSTA_SDC_MAX_THRU];
        MstaCheck Try;
        const MstaCheckCorner *pTryCorner;
        int nNew = 0, nBranches = 0;
        memset( &Try, 0, sizeof(Try) );
        CopyCheckIdentity( &Try, pCheck );
        if ( fAsync )
            CheckAsyncEndpointEdge( p, &Try, nCaptureClock, fDataRise,
                                    vNew, &nNew, vBranches, &nBranches );
        else
            CheckEndpointAttempt( p, &Try, nCaptureClock, vNew, &nNew,
                                  vBranches, &nBranches, fDataRise, fMax, !fMax );
        pTryCorner = Msta_CheckCorner( &Try, fMax );
        if ( Try.fCutByException )
            pCheck->fCutByException = 1;
        if ( pTryCorner->fChecked )
        {
            KeepWorseCandidate( p, pCheck, &Try );
            pCheck->fPathSaved = 1;
            return;
        }
        Msta_TiFreeCheckPaths( &Try );
        if ( nNew > 0 && AddPathExclusions( p, vNew, nNew ) )
        {
            Msta_TiPropagateData( p, fMax, nPropClock, nStartClass );
            continue;
        }
        if ( nBranches > 0 && nDepth < 4 )
        {
            int nSaved = p->nPathExclude, b;
            MstaPathExclude vSaved[MSTA_MAX_PATH_EXCLUDE];
            if ( nSaved > 0 )
                memcpy( vSaved, p->vPathExclude, (size_t)nSaved * sizeof(MstaPathExclude) );
            for ( b = 0; b < nBranches; b++ )
            {
                if ( AddPathExclusions( p, &vBranches[b], 1 ) )
                {
                    MstaCheck Branch;
                    memset( &Branch, 0, sizeof(Branch) );
                    CopyCheckIdentity( &Branch, pCheck );
                    Msta_TiPropagateData( p, fMax, nPropClock, nStartClass );
                    SearchCornerPath( p, &Branch, nCaptureClock, nPropClock, nStartClass,
                                      fDataRise, fMax, fAsync, nDepth + 1 );
                    KeepWorseCandidate( p, pCheck, &Branch );
                    if ( pCheck->Setup.fChecked || pCheck->Hold.fChecked )
                        pCheck->fPathSaved = 1;
                }
                p->nPathExclude = nSaved;
                if ( nSaved > 0 )
                    memcpy( p->vPathExclude, vSaved, (size_t)nSaved * sizeof(MstaPathExclude) );
            }
            Msta_TiPropagateData( p, fMax, nPropClock, nStartClass );
            return;
        }
        return;
    }
}

static void CheckEndpointEdge( MstaTiming *p, MstaCheck *pCheck, int nCaptureClock,
                               int nPropClock, int nStartClass, int fDataRise )
{
    pCheck->Setup.fChecked = pCheck->Hold.fChecked = 0;
    pCheck->fCutByException = 0;
    SearchCornerPath( p, pCheck, nCaptureClock, nPropClock, nStartClass, fDataRise, 1, 0, 0 );
    if ( p->nPathExclude > 0 )
    {
        p->nPathExclude = 0;
        Msta_TiPropagateData( p, 1, nPropClock, nStartClass );
    }
    SearchCornerPath( p, pCheck, nCaptureClock, nPropClock, nStartClass, fDataRise, 0, 0, 0 );
    if ( p->nPathExclude > 0 )
    {
        p->nPathExclude = 0;
        Msta_TiPropagateData( p, 0, nPropClock, nStartClass );
    }
    pCheck->fPathSaved = pCheck->Setup.fChecked || pCheck->Hold.fChecked;
}

static void ClearPathExclusions( MstaTiming *p, int nPropClock, int nStartClass )
{
    if ( p->nPathExclude == 0 )
        return;
    p->nPathExclude = 0;
    Msta_TiPropagateData( p, 1, nPropClock, nStartClass );
    Msta_TiPropagateData( p, 0, nPropClock, nStartClass );
}

static void CheckEndpointOne( MstaTiming *p, MstaCheck *pCheck, int nCaptureClock,
                              int nPropClock, int nStartClass )
{
    int Edge, Tag;
    pCheck->Setup.fChecked = pCheck->Hold.fChecked = 0;
    for ( Tag = 0; Tag < 2; Tag++ )
    for ( Edge = 1; Edge >= 0; Edge-- )
    {
        MstaCheck Candidate;
        p->CaptureClockTag = Tag;
        memset( &Candidate, 0, sizeof(Candidate) );
        CopyCheckIdentity( &Candidate, pCheck );
        CheckEndpointEdge( p, &Candidate, nCaptureClock, nPropClock, nStartClass, Edge );
        KeepWorseCandidate( p, pCheck, &Candidate );
        ClearPathExclusions( p, nPropClock, nStartClass );
    }
    p->CaptureClockTag = -1;
    pCheck->fPathSaved = pCheck->Setup.fChecked || pCheck->Hold.fChecked;
}

static void CheckEndpoint( MstaTiming *p, MstaCheck *pCheck, int nOutputClock,
                           int nPropClock, int nStartClass )
{
    int c, nClkNet;

    if ( pCheck->fAsync )
    {
        CheckAsyncEndpoint( p, pCheck, nPropClock, nStartClass );
        return;
    }
    if ( !pCheck->fToRegister )
    {
        CheckEndpointOne( p, pCheck, nOutputClock, nPropClock, nStartClass );
        ClearPathExclusions( p, nPropClock, nStartClass );
        return;
    }
    {
        MstaInst *pInst = Msta_TiInstAt( p, pCheck->InstId );
        MstaRegCheck *pReg = MstaRegCheckArrayAt( &pInst->pCell->vRegs, pCheck->nCheck );
        nClkNet = Msta_TiInstPinNet( p, pCheck->InstId, pReg->ClkPin );
    }
    for ( c = 0; c < Msta_SdcClockCount(p->pSdc); c++ )
    {
        MstaCheck Candidate;
        if ( !Msta_TiNetHasDataClock(p, c, nClkNet) )
            continue;
        memset( &Candidate, 0, sizeof(Candidate) );
        CopyCheckIdentity( &Candidate, pCheck );
        CheckEndpointOne( p, &Candidate, c, nPropClock, nStartClass );
        KeepWorseCandidate( p, pCheck, &Candidate );
        ClearPathExclusions( p, nPropClock, nStartClass );
    }
}

void Msta_TiEvaluateCandidatePaths( MstaTiming *p, int nClock, int nStartClass )
{
    int j;
    Msta_TiPropagateData( p, 1, nClock, nStartClass );
    Msta_TiPropagateData( p, 0, nClock, nStartClass );
    for ( j = 0; j < p->vChecks.nSize; j++ )
    {
        MstaCheck *pBest = MstaCheckArrayAt( &p->vChecks, j );
        int k, nVariants = pBest->fToRegister ? 1 : Msta_SdcClockCount( p->pSdc );
        for ( k = 0; k < nVariants; k++ )
        {
            MstaCheck Candidate;
            memset( &Candidate, 0, sizeof(Candidate) );
            CopyCheckIdentity( &Candidate, pBest );
            CheckEndpoint( p, &Candidate, pBest->fToRegister ? -1 : k, nClock, nStartClass );
            KeepWorseCandidate( p, pBest, &Candidate );
        }
    }
}

void Msta_TiReevalSingleCheck( MstaTiming *p, int nCheckIdx, int nPropClock, int nStartClass )
{
    MstaCheck *pBest = MstaCheckArrayAt( &p->vChecks, nCheckIdx );
    MstaCheck Template;
    int k, nVariants = pBest->fToRegister ? 1 : Msta_SdcClockCount( p->pSdc );

    Msta_TiFreeCheckPaths( pBest );
    memset( &Template, 0, sizeof(Template) );
    CopyCheckIdentity( &Template, pBest );
    *pBest = Template;
    p->nPathExclude = 0;
    for ( k = 0; k < nVariants; k++ )
    {
        MstaCheck Candidate;
        memset( &Candidate, 0, sizeof(Candidate) );
        CopyCheckIdentity( &Candidate, pBest );
        CheckEndpoint( p, &Candidate, pBest->fToRegister ? -1 : k, nPropClock, nStartClass );
        KeepWorseCandidate( p, pBest, &Candidate );
    }
}

void Msta_TiCheckDataChecks( MstaTiming *p, int nClock, char *pfDone )
{
    int i;
    MstaClock *pClk = Msta_SdcClockByIndex( p->pSdc, nClock );
    if ( Msta_SdcDataCheckCount(p->pSdc) == 0 || pClk == NULL )
        return;
    Msta_TiPropagateData( p, 1, nClock, -1 );
    for ( i = 0; i < Msta_SdcDataCheckCount(p->pSdc); i++ )
    {
        MstaDataCheck *pCheck = Msta_SdcDataCheckByIndex( p->pSdc, i );
        int nFrom = pCheck->FromNet, nTo = pCheck->ToNet;
        int fFromRise = ( pCheck->FromRF == 'r' ) ? 1
                      : ( pCheck->FromRF == 'f' ) ? 0 : Msta_TiPathEndRise( p, nFrom, 1 );
        int fToRise   = ( pCheck->ToRF == 'r' )   ? 1
                      : ( pCheck->ToRF == 'f' )   ? 0 : Msta_TiPathEndRise( p, nTo, 1 );
        double FromArr = ( fFromRise ? p->CornerMax.pArrRise : p->CornerMax.pArrFall )[nFrom];
        double ToArr   = ( fToRise   ? p->CornerMax.pArrRise : p->CornerMax.pArrFall )[nTo];
        int nStartFrom, nStartTo;
        double FromPhase, ToPhase, Required, Slack;
        if ( !Msta_TiArrivalSet(FromArr, 1) || !Msta_TiArrivalSet(ToArr, 1) )
            continue;
        nStartFrom = TimingStartNetEdge( p, nFrom, 1, fFromRise, NULL );
        nStartTo   = TimingStartNetEdge( p, nTo,   1, fToRise,   NULL );
        if ( nStartFrom < 0 || nStartTo < 0 ||
             p->CornerMax.pnLaunchClock[nStartFrom] != nClock ||
             p->CornerMax.pnLaunchClock[nStartTo]   != nClock )
            continue;
        pfDone[i] = 1;
        p->nDataChecks++;
        if ( pCheck->fSetup )
        {
            FromPhase = p->CornerMax.pfLaunchRises[nStartFrom] ? pClk->RiseEdge : pClk->FallEdge;
            ToPhase   = p->CornerMax.pfLaunchRises[nStartTo]   ? pClk->RiseEdge : pClk->FallEdge;
            Required  = FromArr - FromPhase + ToPhase - pCheck->Value
                      - Msta_TiVarUncertainty( p, nClock, nClock, 1,
                                               p->CornerMax.pfLaunchRises[nStartFrom],
                                               p->CornerMax.pfLaunchRises[nStartTo] );
            Slack     = Required - ToArr;
            if ( Required < (fToRise ? p->CornerMax.pAuxReqRise[nTo]
                                     : p->CornerMax.pAuxReqFall[nTo]) )
            {
                if ( fToRise ) p->CornerMax.pAuxReqRise[nTo] = Required;
                else           p->CornerMax.pAuxReqFall[nTo] = Required;
            }
            if ( Slack < p->WorstDataCheckSetupSlack )
            {
                p->WorstDataCheckSetupSlack = Slack;
                p->DataCheckSetupFrom  = pCheck->FromText;
                p->DataCheckSetupTo    = pCheck->ToText;
                p->DataCheckSetupValue = pCheck->Value;
            }
        }
        if ( pCheck->fHold )
            Msta_WarnOnce( "set_data_check：-hold 未建模，已忽略；数据检查只做 setup 检查" );
    }
}

static int CheckClockGatingOne( MstaTiming *p, int nInst, MstaGateCheck *pGate, int nClock )
{
    MstaInst *pInst = Msta_TiInstAt( p, nInst );
    int nClkPin = Msta_CellPinIndexOf( pInst->pCell, pGate->ClkPin );
    int nEnPin  = Msta_CellPinIndexOf( pInst->pCell, pGate->EnablePin );
    int nClkNet = ( nClkPin >= 0 && nClkPin < pInst->nPins ) ? pInst->pNets[nClkPin] : -1;
    int nEnNet  = ( nEnPin  >= 0 && nEnPin  < pInst->nPins ) ? pInst->pNets[nEnPin]  : -1;
    MstaClockArr *pClkArr;
    double Value, Check, Capture, Required, Slack, Uncertainty;
    int fSet, nLaunchClock, fClkRises, nLaunchCycle = 0, Edge, nStartMax, nStartMin;

    if ( nClkNet < 0 || nEnNet < 0 || !Msta_TiNetHasDataClock(p, nClock, nClkNet) )
        return 0;
    pClkArr = Msta_TiClockArrAt( p, nClock, nClkNet );
    if ( !pClkArr->fReached )
        return 0;
    if ( p->CornerMax.pnLaunchClock[nEnNet] != nClock || p->CornerMin.pnLaunchClock[nEnNet] != nClock )
        return 0;

    fClkRises = p->CaptureClockTag;
    nLaunchClock = nClock;

    for ( Edge = 1; Edge >= 0; Edge-- )
    {
        nStartMax = TimingStartNetEdge( p, nEnNet, 1, Edge, NULL );
        nStartMin = TimingStartNetEdge( p, nEnNet, 0, Edge, NULL );
        Msta_SdcClockGatingValue( p->pSdc, nInst, 1, &Value, &fSet );
        if ( (fSet || (pGate->SetupArc != MSTA_NO_ID &&
             CheckHasEdge(p, Msta_CellArcById(pInst->pCell, pGate->SetupArc), nInst, 1, Edge))) &&
             Msta_TiClockTagValid(pClkArr, Msta_SdcCaptureCorner(p->pSdc, 1), pGate->fClkRises, fClkRises) &&
             Msta_TiArrivalSet(Edge ? p->CornerMax.pArrRise[nEnNet] : p->CornerMax.pArrFall[nEnNet], 1) )
        {
            MstaArc *pArc = ( pGate->SetupArc != MSTA_NO_ID )
                          ? Msta_CellArcById( pInst->pCell, pGate->SetupArc ) : NULL;
            double EnDrvSlew = Edge ? p->CornerMax.pSlewRise[nEnNet] : p->CornerMax.pSlewFall[nEnNet];
            double EndWireD = 0.0, UnusedSlew, Cppr, AuxReq;
            Msta_TiWireToPin( p, nEnNet, nInst, nEnPin, 1, Edge, 0, EnDrvSlew, &EndWireD, &UnusedSlew );
            Check = fSet ? Value : CheckTime( p, pArc, nInst, nClkPin, nEnPin, nClock, nClkNet, nEnNet, 1, Edge );
            Capture = CaptureEdgeTime( p, 1, nClkNet, -1, nInst, nClkPin, nLaunchClock,
                                       p->CornerMax.pfLaunchRises[nStartMax],
                                       nClock, fClkRises, 1, 1, &nLaunchCycle, pGate->fClkRises );
            Uncertainty = Msta_TiVarUncertainty( p, nLaunchClock, nClock, 1,
                                                 p->CornerMax.pfLaunchRises[nStartMax], fClkRises );
            Required = Capture - Uncertainty - Check;
            Slack = Required - ((Edge ? p->CornerMax.pArrRise[nEnNet] : p->CornerMax.pArrFall[nEnNet]) + EndWireD);
            Cppr = Msta_TiVarCpprCredit( p, nStartMax, nLaunchClock, nClock, nClkNet, 1,
                                         p->CornerMax.pfLaunchRises[nStartMax], pGate->fClkRises, fClkRises );
            AuxReq = Required + Cppr - EndWireD;
            Slack += Cppr;
            if ( AuxReq < (Edge ? p->CornerMax.pAuxReqRise[nEnNet]
                                 : p->CornerMax.pAuxReqFall[nEnNet]) )
            {
                if ( Edge ) p->CornerMax.pAuxReqRise[nEnNet] = AuxReq;
                else        p->CornerMax.pAuxReqFall[nEnNet] = AuxReq;
            }
            if ( Slack < p->WorstClkGatingSetupSlack )
            {
                p->WorstClkGatingSetupSlack = Slack;
                p->ClkGatingSetupInst = nInst;
                p->ClkGatingSetupPin  = pGate->EnablePin;
                p->ClkGatingSetupValue = Check;
            }
        }
        Msta_SdcClockGatingValue( p->pSdc, nInst, 0, &Value, &fSet );
        if ( (fSet || (pGate->HoldArc != MSTA_NO_ID &&
             CheckHasEdge(p, Msta_CellArcById(pInst->pCell, pGate->HoldArc), nInst, 0, Edge))) &&
             Msta_TiClockTagValid(pClkArr, Msta_SdcCaptureCorner(p->pSdc, 0), pGate->fClkRises, fClkRises) &&
             Msta_TiArrivalSet(Edge ? p->CornerMin.pArrRise[nEnNet] : p->CornerMin.pArrFall[nEnNet], 0) )
        {
            MstaArc *pArc = ( pGate->HoldArc != MSTA_NO_ID )
                          ? Msta_CellArcById( pInst->pCell, pGate->HoldArc ) : NULL;
            double EnDrvSlew = Edge ? p->CornerMin.pSlewRise[nEnNet] : p->CornerMin.pSlewFall[nEnNet];
            double EndWireD = 0.0, UnusedSlew, Cppr, AuxReq;
            Msta_TiWireToPin( p, nEnNet, nInst, nEnPin, 0, Edge, 0, EnDrvSlew, &EndWireD, &UnusedSlew );
            Check = fSet ? Value : CheckTime( p, pArc, nInst, nClkPin, nEnPin, nClock, nClkNet, nEnNet, 0, Edge );
            Capture = CaptureEdgeTime( p, 1, nClkNet, -1, nInst, nClkPin, nLaunchClock,
                                       p->CornerMin.pfLaunchRises[nStartMin],
                                       nClock, fClkRises, 1, 0, &nLaunchCycle, pGate->fClkRises );
            Uncertainty = Msta_TiVarUncertainty( p, nLaunchClock, nClock, 0,
                                                 p->CornerMin.pfLaunchRises[nStartMin], fClkRises );
            Required = Capture + Uncertainty + Check;
            Slack = ((Edge ? p->CornerMin.pArrRise[nEnNet] : p->CornerMin.pArrFall[nEnNet]) + EndWireD) - Required;
            Cppr = Msta_TiVarCpprCredit( p, nStartMin, nLaunchClock, nClock, nClkNet, 0,
                                         p->CornerMin.pfLaunchRises[nStartMin], pGate->fClkRises, fClkRises );
            AuxReq = Required - Cppr - EndWireD;
            Slack += Cppr;
            if ( AuxReq > (Edge ? p->CornerMin.pAuxReqRise[nEnNet]
                                 : p->CornerMin.pAuxReqFall[nEnNet]) )
            {
                if ( Edge ) p->CornerMin.pAuxReqRise[nEnNet] = AuxReq;
                else        p->CornerMin.pAuxReqFall[nEnNet] = AuxReq;
            }
            if ( Slack < p->WorstClkGatingHoldSlack )
            {
                p->WorstClkGatingHoldSlack = Slack;
                p->ClkGatingHoldInst = nInst;
                p->ClkGatingHoldPin  = pGate->EnablePin;
                p->ClkGatingHoldValue = Check;
            }
        }
    }
    return 1;
}

void Msta_TiCheckClockGating( MstaTiming *p, int nClock, int nStartClasses )
{
    int i, j, c, k, Tag, CaptureTag, nGates = 0;
    char *pSeen;
    for ( i = 0; i < p->pDes->vInsts.nSize; i++ )
        nGates += Msta_TiInstAt(p, i)->pCell->vGates.nSize;
    if ( nGates == 0 )
        return;
    pSeen = (char *)calloc( (size_t)nGates, 1 );
    assert( pSeen );
    for ( Tag = 0; Tag < 2; Tag++ )
    for ( c = 0; c < nStartClasses; c++ )
    {
        p->LaunchClockTag = Tag;
        Msta_TiPropagateData( p, 1, nClock, c );
        Msta_TiPropagateData( p, 0, nClock, c );
        for ( CaptureTag = 0; CaptureTag < 2; CaptureTag++ )
        {
            p->CaptureClockTag = CaptureTag;
            for ( i = 0, k = 0; i < p->pDes->vInsts.nSize; i++ )
            {
                MstaInst *pInst = Msta_TiInstAt( p, i );
                for ( j = 0; j < pInst->pCell->vGates.nSize; j++, k++ )
                    if ( CheckClockGatingOne(p, i, MstaGateCheckArrayAt(&pInst->pCell->vGates, j), nClock)
                         && !pSeen[k] )
                    {
                        pSeen[k] = 1;
                        p->nClkGatingChecks++;
                    }
            }
        }
    }
    p->LaunchClockTag = p->CaptureClockTag = -1;
    free( pSeen );
}
