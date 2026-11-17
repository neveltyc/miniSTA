/**CFile***************************************************************

  FileName    [msta_plugin.c]

  Synopsis    [C99 内置默认插件（nldm / zero_rc / ocv_aocv_cppr）与示范插件（elmore_rc / pba_retrace）。]

***********************************************************************/

#include "msta_timing_int.h"
#include <math.h>

/* =====================================================================
   1. 默认延迟插件：NLDM 双线性插值
   ===================================================================== */

static void NldmEvalArc( const MstaDelayQuery *pQ, MstaDelayResult *pOut )
{
    pOut->Delay = Msta_TiArcDelayEdge( pQ->pArc, pQ->InSlew, pQ->EffCap, pQ->fOutRise );
    pOut->OutSlew = Msta_TiArcSlewEdge( pQ->pArc, pQ->InSlew, pQ->EffCap, pQ->fOutRise );
}

static double NldmEvalConstraint( const struct MstaTiming *pT, const MstaArc *pArc,
                                  int nInst, int fMax, int fDataRise,
                                  double ClkSlew, double DataSlew )
{
    const MstaTable *pTable;
    (void)pT; (void)nInst; (void)fMax;
    pTable = fDataRise ? &pArc->ConstraintRise : &pArc->ConstraintFall;
    return Msta_TableExists(pTable) ? Msta_TableLookup( pTable, ClkSlew, DataSlew ) : 0.0;
}

static const MstaDelayOps s_NldmDelayOps = {
    MSTA_PLUGIN_ABI_VERSION,
    "nldm",
    NULL,
    NldmEvalArc,
    NldmEvalConstraint
};

const MstaDelayOps *Msta_PluginDelayNldm( void ) { return &s_NldmDelayOps; }

/* =====================================================================
   2. 线网插件：zero_rc（零线延迟基准）与 elmore_rc（区分每个扇出）
   ===================================================================== */

static double SumPinLoadsCap( MstaTiming *p, int nNet, int fMax, double WireCapPerLoad, double *pSdcExtra, int *pfSub )
{
    MstaNet *pNet = MstaNetArrayAt( &p->pDes->vNets, nNet );
    MstaNetCons *pCons = Msta_SdcNetCons( p->pSdc, nNet );
    MstaId CornerLib = Msta_SdcOperatingLibrary( p->pSdc, fMax );
    double Sum = 0.0;
    int i;

    for ( i = 0; i < pNet->vLoads.nSize; i++ )
    {
        MstaPinRef *pRef = MstaPinRefArrayAt( &pNet->vLoads, i );
        MstaInst *pLoad = Msta_TiInstAt( p, pRef->InstId );
        MstaPin *pPin = Msta_LibPinForCorner( p->pLib, pLoad->pCell, pRef->PinId, CornerLib );
        Sum += pPin->Cap + WireCapPerLoad;
    }
    *pSdcExtra = 0.0;
    *pfSub = 0;
    if ( pCons )
    {
        double Extra = fMax ? pCons->LoadMax : pCons->LoadMin;
        if ( Msta_IsSet(Extra) )
        {
            *pSdcExtra = Extra;
            *pfSub = pCons->fSubtractPinLoad;
            if ( pCons->fSubtractPinLoad )
                return Extra;
            Sum += Extra;
        }
    }
    return Sum;
}

static double ZeroRcNetEffCap( const MstaWireQuery *pQ )
{
    double Extra = 0.0;
    int fSub = 0;
    return SumPinLoadsCap( pQ->pTiming, pQ->nNet, pQ->fMax, 0.0, &Extra, &fSub );
}

static void ZeroRcEvalWireArc( const MstaWireQuery *pQ, MstaWireResult *pOut )
{
    pOut->WireDelay = 0.0;
    pOut->SinkSlew = pQ->DriverSlew;
}

static const MstaWireOps s_ZeroRcWireOps = {
    MSTA_PLUGIN_ABI_VERSION,
    "zero_rc",
    NULL,
    ZeroRcNetEffCap,
    ZeroRcEvalWireArc
};

const MstaWireOps *Msta_PluginWireZeroRc( void ) { return &s_ZeroRcWireOps; }

/* Elmore RC 梯形互连模型：
   设驱动网络共有 M 个扇出（j = 0 .. M-1），第 m 段串联电阻为 R_seg（kOhm），
   节点 j 的并联电容 C_j = C_pin(j) + C_seg（fF）。则第 i 个扇出的 Elmore 延迟：
       Delay(i) = sum_{m=0..i} R_seg * sum_{j=m..M-1} C_j
                = R_seg * sum_{j=0..M-1} (min(i, j) + 1) * C_j   (仅 O(M) 一次遍历！)
   摆率退化采用 Bakoglu 斜坡近似：SinkSlew = hypot(DriverSlew, 2.2 * WireDelay)。 */
static double ElmoreNetEffCap( const MstaWireQuery *pQ )
{
    const MstaElmoreConfig *pCfg = (const MstaElmoreConfig *)pQ->pUserData;
    double WireC = pCfg ? pCfg->CPerFanout : 2.0;
    double Extra = 0.0;
    int fSub = 0;
    return SumPinLoadsCap( pQ->pTiming, pQ->nNet, pQ->fMax, WireC, &Extra, &fSub );
}

static void ElmoreEvalWireArc( const MstaWireQuery *pQ, MstaWireResult *pOut )
{
    MstaTiming *p = pQ->pTiming;
    const MstaElmoreConfig *pCfg = (const MstaElmoreConfig *)pQ->pUserData;
    double WireR = pCfg ? pCfg->RPerFanout : 0.05;
    double WireC = pCfg ? pCfg->CPerFanout : 2.0;
    MstaNet *pNet = MstaNetArrayAt( &p->pDes->vNets, pQ->nNet );
    MstaNetCons *pCons = Msta_SdcNetCons( p->pSdc, pQ->nNet );
    MstaId CornerLib = Msta_SdcOperatingLibrary( p->pSdc, pQ->fMax );
    int M = pNet->vLoads.nSize;
    int iLoad = pQ->nLoadIdx;
    double Sum = 0.0, WireDelay;
    int j;

    if ( iLoad < 0 || iLoad >= M || M == 0 || WireR <= 0.0 )
    {
        pOut->WireDelay = 0.0;
        pOut->SinkSlew = pQ->DriverSlew;
        return;
    }
    for ( j = 0; j < M; j++ )
    {
        MstaPinRef *pRef = MstaPinRefArrayAt( &pNet->vLoads, j );
        MstaInst *pLoad = Msta_TiInstAt( p, pRef->InstId );
        MstaPin *pPin = Msta_LibPinForCorner( p->pLib, pLoad->pCell, pRef->PinId, CornerLib );
        int Hops = ( j < iLoad ? j : iLoad ) + 1;
        Sum += (double)Hops * ( pPin->Cap + WireC );
    }
    if ( pCons )
    {
        double Extra = pQ->fMax ? pCons->LoadMax : pCons->LoadMin;
        if ( Msta_IsSet(Extra) && !pCons->fSubtractPinLoad && Extra > 0.0 )
            Sum += Extra;
    }
    WireDelay = WireR * Sum;
    pOut->WireDelay = WireDelay;
    pOut->SinkSlew = hypot( pQ->DriverSlew, 2.2 * WireDelay );
}

static MstaElmoreConfig s_DefaultElmoreCfg = { 0.05, 2.0 };
static const MstaWireOps s_DefaultElmoreOps = {
    MSTA_PLUGIN_ABI_VERSION,
    "elmore_rc",
    &s_DefaultElmoreCfg,
    ElmoreNetEffCap,
    ElmoreEvalWireArc
};

const MstaWireOps *Msta_PluginDefaultElmoreWire( void ) { return &s_DefaultElmoreOps; }

void Msta_PluginInitElmoreWire( MstaWireOps *pOps, MstaElmoreConfig *pCfg,
                                double RPerFanoutKOhm, double CPerFanoutFf )
{
    pCfg->RPerFanout = RPerFanoutKOhm;
    pCfg->CPerFanout = CPerFanoutFf;
    pOps->AbiVersion = MSTA_PLUGIN_ABI_VERSION;
    pOps->pName = "elmore_rc";
    pOps->pUserData = pCfg;
    pOps->NetEffCap = ElmoreNetEffCap;
    pOps->EvalWireArc = ElmoreEvalWireArc;
}

/* =====================================================================
   3. 偏差插件：默认 OCV + AOCV + Uncertainty + CPPR
   ===================================================================== */

static double DefaultVarArcScale( const struct MstaTiming *pT, int nInst, int nClock,
                                  int nInNet, int nOutNet, int fMax, int fInRise, int fClockPath )
{
    MstaTiming *p = (MstaTiming *)pT;
    double Derate = fClockPath
        ? Msta_SdcClockDerateClock( p->pSdc, nClock, nInst, fMax, fInRise )
        : Msta_SdcDataDerateInst( p->pSdc, nInst, fMax, fInRise );
    double Aocv = Msta_TiAocvDelayFactor( p, nInst, nClock, nInNet, nOutNet,
                                          fMax, fInRise, fClockPath );
    return Derate * Aocv;
}

static double DefaultVarCheckScale( const struct MstaTiming *pT, int nInst, int fMax, int fDataRise )
{
    return Msta_SdcCheckDerateInst( pT->pSdc, nInst, fMax, fDataRise );
}

static double DefaultVarUncertainty( const struct MstaTiming *pT, int nLaunchClock,
                                     int nCaptureClock, int fSetup,
                                     int fLaunchRises, int fCaptureRises )
{
    return Msta_SdcClockUncertainty( pT->pSdc, nLaunchClock, nCaptureClock,
                                     fSetup, fLaunchRises, fCaptureRises );
}

static double DefaultVarCpprCredit( const struct MstaTiming *pT, int nStart, int nLaunchClock,
                                    int nCaptureClock, int nCaptureNet, int fSetup,
                                    int nLaunchSource, int nCaptureLocal, int nCaptureSource )
{
    return Msta_TiPathCppr( (MstaTiming *)pT, nStart, nLaunchClock, nCaptureClock,
                            nCaptureNet, fSetup, nLaunchSource, nCaptureLocal, nCaptureSource );
}

static const MstaVariationOps s_DefaultVariationOps = {
    MSTA_PLUGIN_ABI_VERSION,
    "ocv_aocv_cppr",
    NULL,
    DefaultVarArcScale,
    DefaultVarCheckScale,
    DefaultVarUncertainty,
    DefaultVarCpprCredit
};

const MstaVariationOps *Msta_PluginVariationDefault( void ) { return &s_DefaultVariationOps; }

/* =====================================================================
   4. 路径精修插件：none 与 pba_retrace（真路径逐级重溯 / 真实 AOCV 深度）
   ===================================================================== */

static void NoneRefineCheck( struct MstaTiming *pT, struct MstaCheck *pCheck, int fSetup )
{
    (void)pT; (void)pCheck; (void)fSetup;
}

static const MstaPathRefinerOps s_NoneRefinerOps = {
    MSTA_PLUGIN_ABI_VERSION,
    "none",
    NULL,
    NoneRefineCheck
};

const MstaPathRefinerOps *Msta_PluginRefinerNone( void ) { return &s_NoneRefinerOps; }

static double PbaStageVarScale( MstaTiming *p, int nInst, int nInNet, int nOutNet,
                                int fMax, int fInRise, int ExactPathDepth )
{
    if ( p->pSdc->fAocv && p->pSdc->vAocv.nSize )
    {
        MstaInst *pInst = Msta_TiInstAt( p, nInst );
        return Msta_SdcDataDerateInst( p->pSdc, nInst, fMax, fInRise )
             * Msta_AocvFactor( p->pSdc, pInst, 0, fInRise, fMax, ExactPathDepth );
    }
    return Msta_TiVarArcScale( p, nInst, -1, nInNet, nOutNet, fMax, fInRise, 0 );
}

static void PbaRetraceCheck( struct MstaTiming *p, struct MstaCheck *pCheck, int fSetup )
{
    MstaCheckCorner *pC = Msta_CheckCorner( pCheck, fSetup );
    MstaCorner *pCorner = Msta_TiCornerOf( p, fSetup );
    int fMax = fSetup;
    int nPath, i, nStart, StartEdge, ExactDepth = 0;
    double PathArr, PathSlew, EndWireD = 0.0, EndPinSlew;

    if ( !pC->fChecked || pC->nPath <= 0 || pC->pPath == NULL ||
         pC->pPathArrival == NULL || pC->pPathEdge == NULL || pC->pPathLoadPin == NULL )
        return;

    nPath = pC->nPath;
    for ( i = nPath - 1; i >= 0; i-- )
    {
        int nNet = pC->pPath[i];
        MstaNet *pNet = MstaNetArrayAt( &p->pDes->vNets, nNet );
        if ( pNet->Driver.InstId != MSTA_NO_ID && !p->pfIdealNet[nNet] )
            ExactDepth++;
    }
    if ( ExactDepth < 1 )
        ExactDepth = 1;

    nStart = pC->pPath[nPath - 1];
    StartEdge = pC->pPathEdge[nPath - 1];
    PathArr = pC->pPathArrival[nPath - 1];
    PathSlew = StartEdge ? pCorner->pSlewRise[nStart] : pCorner->pSlewFall[nStart];

    /* 起点级重溯：若为时序单元输出，按真实时钟沿与真实路径 AOCV 深度重算 clk2q。 */
    {
        MstaNet *pStartNet = MstaNetArrayAt( &p->pDes->vNets, nStart );
        double IdealLat = Msta_SdcIdealLatency( p->pSdc, nStart, fMax, StartEdge );
        if ( !Msta_IsSet(IdealLat) && pStartNet->Driver.InstId != MSTA_NO_ID )
        {
            int nLaunchClk = Msta_SdcClockIndexOf( p->pSdc, pC->LaunchClock );
            int nDriverInst = pStartNet->Driver.InstId;
            MstaInst *pInst = Msta_TiInstAt( p, nDriverInst );
            MstaCell *pCell = pInst->pCell;
            MstaPin *pOutPin = MstaPinArrayAt( &pCell->vPins, pStartNet->Driver.PinId );
            MstaClock *pClkObj = nLaunchClk >= 0 ? Msta_SdcClockByIndex( p->pSdc, nLaunchClk ) : NULL;
            if ( pCell->fSequential && nLaunchClk >= 0 )
            {
                int k;
                double Load = Msta_TiEffCap( p, nStart, fMax, 0 );
                for ( k = 0; k < pCell->vRegs.nSize; k++ )
                {
                    MstaRegCheck *pReg = MstaRegCheckArrayAt( &pCell->vRegs, k );
                    int fClkRises = pReg->fClkRises;
                    MstaArc *pArc = Msta_TiRegClkToQArc( p, pCell, pReg, pOutPin->Name,
                                                         &fClkRises, fMax, nDriverInst );
                    int nClkPin = Msta_CellPinIndexOf( pCell, pReg->ClkPin );
                    int nClkNet = (nClkPin >= 0 && nClkPin < pInst->nPins) ? pInst->pNets[nClkPin] : -1;
                    MstaClockArr *pClkArr;
                    double ClkDrvArr, ClkDrvSlew, ClkWireD, ClkPinSlew, EdgeTime;
                    if ( pArc == NULL || nClkNet < 0 || !Msta_TiNetHasDataClock(p, nLaunchClk, nClkNet) )
                        continue;
                    pClkArr = Msta_TiClockArrAt( p, nLaunchClk, nClkNet );
                    if ( !Msta_TiClockTagValid(pClkArr, fMax, fClkRises, pC->LaunchSourceRise) )
                        continue;
                    ClkDrvArr = Msta_TiClockArrival( pClkArr, fMax, fClkRises, pC->LaunchSourceRise );
                    ClkDrvSlew = Msta_TiClockSlew( pClkArr, fMax, fClkRises, pC->LaunchSourceRise );
                    Msta_TiWireToPin( p, nClkNet, nDriverInst, nClkPin, fMax, fClkRises, 1,
                                      ClkDrvSlew, &ClkWireD, &ClkPinSlew );
                    EdgeTime = ClkDrvArr + ClkWireD +
                        (pClkObj ? (pC->LaunchSourceRise ? pClkObj->RiseEdge : pClkObj->FallEdge) : 0.0);
                    if ( p->pfIdealNet[nStart] )
                    {
                        PathArr = EdgeTime;
                        PathSlew = ClkPinSlew;
                    }
                    else
                    {
                        MstaDelayResult DR;
                        double Scale = PbaStageVarScale( p, nDriverInst, -1, nStart,
                                                         fMax, fClkRises, ExactDepth );
                        Msta_TiEvalArc( p, nDriverInst, nClkNet, nStart, nLaunchClk, pArc,
                                        ClkPinSlew, Load, fMax, StartEdge, fClkRises, 0, &DR );
                        PathArr = EdgeTime + DR.Delay * Scale;
                        PathSlew = DR.OutSlew;
                    }
                    pC->pPathArrival[nPath - 1] = PathArr;
                    break;
                }
            }
        }
    }

    /* 组合逻辑沿选中路径逐级精确传播，避免跨路径混合假设。 */
    for ( i = nPath - 2; i >= 0; i-- )
    {
        int nOutNet = pC->pPath[i];
        int OutEdge = pC->pPathEdge[i];
        int nLoadPin = pC->pPathLoadPin[i];
        int nInNet = pC->pPath[i + 1];
        int InEdge = pC->pPathEdge[i + 1];
        MstaNet *pOutNet = MstaNetArrayAt( &p->pDes->vNets, nOutNet );
        int nDriverInst = pOutNet->Driver.InstId;
        MstaInst *pDriver;
        MstaCell *pCell, *pCombCell;
        MstaPin *pInPin, *pOutPin;
        double WireD = 0.0, PinSlew = PathSlew, Load, IdealLat;
        double BestDelay = 0.0, BestSlew = PathSlew;
        int a, fHasArc = 0, fGotCand = 0;

        Msta_TiWireToPin( p, nInNet, nDriverInst, nLoadPin, fMax, InEdge, 0,
                          PathSlew, &WireD, &PinSlew );
        PathArr += WireD;
        IdealLat = Msta_SdcIdealLatency( p->pSdc, nOutNet, fMax, OutEdge );
        if ( Msta_IsSet(IdealLat) )
        {
            PathArr = IdealLat;
            PathSlew = OutEdge ? pCorner->pSlewRise[nOutNet] : pCorner->pSlewFall[nOutNet];
            pC->pPathArrival[i] = PathArr;
            continue;
        }
        if ( p->pfIdealNet[nOutNet] || nDriverInst == MSTA_NO_ID || nLoadPin < 0 )
        {
            PathSlew = PinSlew;
            pC->pPathArrival[i] = PathArr;
            continue;
        }
        pDriver = Msta_TiInstAt( p, nDriverInst );
        pCell = pDriver->pCell;
        if ( nLoadPin >= pCell->vPins.nSize || pOutNet->Driver.PinId >= pCell->vPins.nSize )
        {
            PathSlew = PinSlew;
            pC->pPathArrival[i] = PathArr;
            continue;
        }
        pInPin = MstaPinArrayAt( &pCell->vPins, nLoadPin );
        pOutPin = MstaPinArrayAt( &pCell->vPins, pOutNet->Driver.PinId );
        Load = Msta_TiEffCap( p, nOutNet, fMax, 0 );
        pCombCell = Msta_TiTimingCombCell( p, pCell, pInPin->Name, pOutPin->Name, fMax );
        for ( a = 0; a < pCombCell->vArcs.nSize; a++ )
        {
            MstaArc *pArc = MstaArcArrayAt( &pCombCell->vArcs, a );
            int Coupled;
            MstaDelayResult DR;
            double Delay, Scale;
            if ( pArc->InPin != pInPin->Name || pArc->OutPin != pOutPin->Name ||
                 !Msta_ArcIsComb( pCombCell, pArc ) )
                continue;
            fHasArc = 1;
            if ( !Msta_TiDataArcWhenActive( p, nDriverInst, pDriver, pCombCell, pArc ) )
                continue;
            Coupled = Msta_TiCoupledInputEdge( pArc->Sense, OutEdge );
            if ( Coupled >= 0 && InEdge != Coupled )
                continue;
            Msta_TiEvalArc( p, nDriverInst, nInNet, nOutNet, -1, pArc,
                            PinSlew, Load, fMax, OutEdge, InEdge, 0, &DR );
            Scale = PbaStageVarScale( p, nDriverInst, nInNet, nOutNet,
                                      fMax, InEdge, ExactDepth );
            Delay = DR.Delay * Scale;
            if ( !fGotCand || ( fMax ? (Delay > BestDelay) : (Delay < BestDelay) ) )
            {
                BestDelay = Delay;
                BestSlew = DR.OutSlew;
                fGotCand = 1;
            }
        }
        if ( fGotCand )
        {
            PathArr += BestDelay;
            PathSlew = BestSlew;
        }
        else if ( !fHasArc )
            PathSlew = PinSlew;
        pC->pPathArrival[i] = PathArr;
    }

    /* 终点最后一段互连与寄存器 setup/hold 约束也按真实 path slew 重算。 */
    {
        int nEndNet = pC->pPath[0];
        Msta_TiWireToPin( p, nEndNet, pCheck->fToRegister ? pCheck->InstId : -1,
                          pCheck->fToRegister ? pCheck->nDataPin : -1,
                          fMax, pC->fDataRise, 0, PathSlew, &EndWireD, &EndPinSlew );
        pC->Arrival = PathArr + EndWireD + pC->LaunchShift;
        if ( pCheck->fToRegister && !pC->fPathBudget )
        {
            MstaInst *pEndInst = Msta_TiInstAt( p, pCheck->InstId );
            MstaCell *pEndCell = pEndInst->pCell;
            const MstaArc *pChkArc = NULL;
            MstaId ClkPin = MSTA_NO_ID;
            if ( pCheck->fAsync )
            {
                MstaAsyncCheck *pAsync = MstaAsyncCheckArrayAt( &pEndCell->vAsync, pCheck->nAsyncCheck );
                MstaId nArc = pCheck->fRecovery
                    ? (pAsync->RecoveryArc != MSTA_NO_ID ? pAsync->RecoveryArc : pAsync->RecoveryFallArc)
                    : (pAsync->RemovalArc  != MSTA_NO_ID ? pAsync->RemovalArc  : pAsync->RemovalFallArc);
                pChkArc = Msta_CellArcById( pEndCell, nArc );
                ClkPin = pAsync->ClkPin;
            }
            else
            {
                MstaRegCheck *pReg = MstaRegCheckArrayAt( &pEndCell->vRegs, pCheck->nCheck );
                MstaId nArc = fSetup
                    ? (pReg->SetupArc != MSTA_NO_ID ? pReg->SetupArc : pReg->SetupFallArc)
                    : (pReg->HoldArc  != MSTA_NO_ID ? pReg->HoldArc  : pReg->HoldFallArc);
                pChkArc = Msta_CellArcById( pEndCell, nArc );
                ClkPin = pReg->ClkPin;
            }
            pChkArc = Msta_TiTimingArcForCheckCorner( p, pChkArc, pCheck->InstId, fMax );
            MstaId nCapName = (pC->CaptureClock != MSTA_NO_ID) ? pC->CaptureClock : pCheck->CaptureClock;
            int nCapClk = Msta_SdcClockIndexOf( p->pSdc, nCapName );
            int nClkPinIdx = Msta_CellPinIndexOf( pEndCell, ClkPin );
            int nCapNet = (nClkPinIdx >= 0 && nClkPinIdx < pEndInst->nPins) ? pEndInst->pNets[nClkPinIdx] : -1;
            if ( pChkArc != NULL && nCapClk >= 0 && nCapNet >= 0 )
            {
                int nCapCorner = Msta_SdcCaptureCorner( p->pSdc, fSetup );
                int fArcClkRises = Msta_TiArcClockRises( pChkArc );
                MstaClockArr *pClkArr = Msta_TiClockArrAt( p, nCapClk, nCapNet );
                double ClkDrvSlew = Msta_TiClockSlew( pClkArr, nCapCorner,
                                                      fArcClkRises, pC->CaptureSourceRise );
                double UnusedWire = 0.0, ClkPinSlew = ClkDrvSlew;
                double DataPinSlew = EndPinSlew;
                double NewCheck, OldCheck = pC->CheckTime;
                Msta_TiWireToPin( p, nCapNet, pCheck->InstId, nClkPinIdx, nCapCorner,
                                  fArcClkRises, 1, ClkDrvSlew, &UnusedWire, &ClkPinSlew );
                if ( ClkPinSlew < 0.0 )
                    ClkPinSlew = MSTA_DEFAULT_SLEW;
                if ( DataPinSlew < 0.0 )
                    DataPinSlew = MSTA_DEFAULT_SLEW;
                NewCheck = Msta_TiEvalConstraint( p, pChkArc, pCheck->InstId, fMax, pC->fDataRise,
                                                  ClkPinSlew, DataPinSlew )
                         * Msta_TiVarCheckScale( p, pCheck->InstId, fMax, pC->fDataRise );
                pC->CheckTime = NewCheck;
                if ( !(fSetup && pEndCell->fLatch && Msta_IsSet(pC->Borrow)) )
                    pC->Required += fSetup ? (OldCheck - NewCheck) : (NewCheck - OldCheck);
            }
        }
        pC->Slack = fSetup ? (pC->Required - pC->Arrival) : (pC->Arrival - pC->Required);
        pC->EdgeReq[pC->fDataRise ? 1 : 0] = pC->Required - pC->LaunchShift - EndWireD;
        pC->fEdgeReq[pC->fDataRise ? 1 : 0] = 1;
    }
}

static const MstaPathRefinerOps s_PbaRetraceRefinerOps = {
    MSTA_PLUGIN_ABI_VERSION,
    "pba_retrace",
    NULL,
    PbaRetraceCheck
};

const MstaPathRefinerOps *Msta_PluginRefinerPbaRetrace( void ) { return &s_PbaRetraceRefinerOps; }

/* =====================================================================
   初始化默认插件集
   ===================================================================== */

void Msta_PluginSetInitDefaults( MstaPluginSet *pSet )
{
    if ( pSet == NULL )
        return;
    pSet->pDelay = Msta_PluginDelayNldm();
    pSet->pWire = Msta_PluginWireZeroRc();
    pSet->pVariation = Msta_PluginVariationDefault();
    pSet->pRefiner = Msta_PluginRefinerNone();
}
