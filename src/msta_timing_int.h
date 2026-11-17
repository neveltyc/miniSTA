/**CHeader*************************************************************

  FileName    [msta_timing_int.h]

  Synopsis    [时序引擎内部共享头：拆分成 7 个模块时共用的工具内联函数与内部接口。]

***********************************************************************/

#ifndef MSTA_TIMING_INT_H
#define MSTA_TIMING_INT_H

#include <math.h>
#include "msta_timing.h"
#include "msta_util.h"

#define MSTA_DEFAULT_SLEW   50.0     /* ps：摆率缺省值，没约束也没表可查时用 */
#define MSTA_DEFAULT_LOAD   5.0      /* fF：一个网络没有扇出时查表用的负载 */
#define MSTA_NO_TIME        (1e30)
#define MSTA_PATH_OBJECTS   4096

/* ---------------------------------------------------------------------
   0. 轻量内联访问器（供各个拆分模块共用）
   --------------------------------------------------------------------- */

static inline MstaInst *Msta_TiInstAt( MstaTiming *p, int nInst )
{
    return MstaInstArrayAt( &p->pDes->vInsts, nInst );
}

static inline MstaCorner *Msta_TiCornerOf( MstaTiming *p, int fMax )
{
    return fMax ? &p->CornerMax : &p->CornerMin;
}

static inline MstaClockArr *Msta_TiClockArrAt( MstaTiming *p, int nClock, int nNet )
{
    return &p->pClockArr[ (size_t)nClock * (size_t)p->pDes->vNets.nSize + (size_t)nNet ];
}

static inline int Msta_TiNetHasClock( MstaTiming *p, int nClock, int nNet )
{
    return nClock >= 0 && nNet >= 0 &&
           p->pfClockOfNet[ (size_t)nClock * (size_t)p->pDes->vNets.nSize + (size_t)nNet ];
}

static inline int Msta_TiNetHasDataClock( MstaTiming *p, int nClock, int nNet )
{
    return Msta_TiNetHasClock( p, nClock, nNet ) &&
           !Msta_TiClockArrAt( p, nClock, nNet )->fSourceOnly;
}

static inline int Msta_TiEffectiveClkRises( MstaTiming *p, int nClock, int nClkNet, int fRegRises )
{
    if ( nClock < 0 || nClkNet < 0 )
        return fRegRises;
    return Msta_TiClockArrAt( p, nClock, nClkNet )->Polarity < 0 ? !fRegRises : fRegRises;
}

static inline int Msta_TiClockTagValid( const MstaClockArr *pArr, int fMax, int Local, int Source )
{
    double v;
    if ( Local < 0 || Local > 1 || Source < 0 || Source > 1 )
        return 0;
    v = pArr->Arrival[fMax][Local][Source];
    return (pArr->Tags[Local] & (1 << Source)) &&
           v > -MSTA_NO_TIME / 2 && v < MSTA_NO_TIME / 2;
}

static inline int Msta_TiClockTagSelect( const MstaClockArr *pArr, int fMax, int Local, int Want )
{
    double R, F;
    if ( Want >= 0 )
        return Msta_TiClockTagValid( pArr, fMax, Local, Want ) ? Want : -1;
    if ( !Msta_TiClockTagValid( pArr, fMax, Local, 0 ) )
        return Msta_TiClockTagValid( pArr, fMax, Local, 1 ) ? 1 : -1;
    if ( !Msta_TiClockTagValid( pArr, fMax, Local, 1 ) )
        return 0;
    R = pArr->Arrival[fMax][Local][1];
    F = pArr->Arrival[fMax][Local][0];
    return fMax ? (R >= F) : (R <= F);
}

static inline double Msta_TiClockArrival( const MstaClockArr *pArr, int fMax, int Local, int Source )
{
    int s = Msta_TiClockTagSelect( pArr, fMax, Local, Source );
    return s >= 0 ? pArr->Arrival[fMax][Local][s] : (fMax ? -MSTA_NO_TIME : MSTA_NO_TIME);
}

static inline double Msta_TiClockSlew( const MstaClockArr *pArr, int fMax, int Local, int Source )
{
    int s = Msta_TiClockTagSelect( pArr, fMax, Local, Source );
    return s >= 0 ? pArr->Slew[fMax][Local][s] : MSTA_DEFAULT_SLEW;
}

static inline void Msta_TiMergeClockSummary( MstaClockArr *pArr )
{
    int e, s;
    pArr->MaxArrival = -MSTA_NO_TIME;
    pArr->MaxSlew = 0;
    for ( e = 0; e < 2; e++ )
        for ( s = 0; s < 2; s++ )
            if ( Msta_TiClockTagValid( pArr, 1, e, s ) )
            {
                pArr->MaxArrival = fmax( pArr->MaxArrival, pArr->Arrival[1][e][s] );
                pArr->MaxSlew = fmax( pArr->MaxSlew, pArr->Slew[1][e][s] );
            }
    if ( pArr->MaxArrival == -MSTA_NO_TIME )
        pArr->MaxArrival = 0;
}

static inline double Msta_TiClockSourceArrival( MstaTiming *p, int nClock, int fMax, int fSourceRise )
{
    MstaClock *pClock = Msta_SdcClockByIndex( p->pSdc, nClock );
    MstaClockArr *pSource;
    int Local;
    double Arrival;
    if ( pClock->MasterClock == MSTA_NO_ID || pClock->SourceNet < 0 )
        return pClock->SourceLatency[fMax][fSourceRise];
    pSource = Msta_TiClockArrAt( p, nClock, pClock->SourceNet );
    Local = Msta_TiClockTagValid( pSource, fMax, fSourceRise, fSourceRise ) ? fSourceRise : !fSourceRise;
    Arrival = Msta_TiClockArrival( pSource, fMax, Local, fSourceRise );
    if ( !pClock->fPropagated && !p->pfIdealNet[pClock->SourceNet] )
        Arrival -= pClock->NetworkLatency[fMax][fSourceRise];
    return Arrival;
}

static inline int Msta_TiArcClockRises( const MstaArc *pArc )
{
    return pArc->Type != MSTA_TT_SETUP_FALLING && pArc->Type != MSTA_TT_HOLD_FALLING &&
           pArc->Type != MSTA_TT_RECOVERY_FALLING && pArc->Type != MSTA_TT_REMOVAL_FALLING &&
           pArc->Type != MSTA_TT_FALL_EDGE;
}

static inline int Msta_TiInstPinNet( MstaTiming *p, int nInst, MstaId PinName )
{
    MstaInst *pInst = Msta_TiInstAt( p, nInst );
    int nPin = Msta_CellPinIndexOf( pInst->pCell, PinName );
    if ( nPin < 0 || pInst->pNets[nPin] == MSTA_NO_ID )
        return -1;
    return pInst->pNets[nPin];
}

static inline double Msta_TiArcDelayEdge( const MstaArc *pArc, double Slew, double Load, int fRise )
{
    const MstaTable *pMain = fRise ? &pArc->DelayRise : &pArc->DelayFall;
    const MstaTable *pAlt = fRise ? &pArc->DelayFall : &pArc->DelayRise;
    return Msta_TableExists(pMain) ? Msta_TableLookup(pMain, Slew, Load)
                                   : (Msta_TableExists(pAlt) ? Msta_TableLookup(pAlt, Slew, Load) : 0.0);
}

static inline double Msta_TiArcSlewEdge( const MstaArc *pArc, double Slew, double Load, int fRise )
{
    const MstaTable *pMain = fRise ? &pArc->TransRise : &pArc->TransFall;
    const MstaTable *pAlt = fRise ? &pArc->TransFall : &pArc->TransRise;
    double v;
    if ( !Msta_TableExists(pMain) && !Msta_TableExists(pAlt) )
        return MSTA_DEFAULT_SLEW;
    v = Msta_TableLookup( Msta_TableExists(pMain) ? pMain : pAlt, Slew, Load );
    return v >= 0.0 ? v : MSTA_DEFAULT_SLEW;
}

static inline double Msta_TiClockPeriod( MstaTiming *p, int nClock )
{
    MstaClock *pClock = Msta_SdcClockByIndex( p->pSdc, nClock );
    return ( pClock && pClock->Period > 0.0 ) ? pClock->Period : 0.0;
}

static inline double Msta_TiClockUncertainty( MstaTiming *p, int nLaunchClock, int nCaptureClock,
                                              int fSetup, int fLaunchRises, int fCaptureRises )
{
    return Msta_SdcClockUncertainty( p->pSdc, nLaunchClock, nCaptureClock,
                                     fSetup, fLaunchRises, fCaptureRises );
}

static inline int Msta_TiArrivalSet( double Arrival, int fMax )
{
    return fMax ? Arrival > -MSTA_NO_TIME / 2.0 : Arrival < MSTA_NO_TIME / 2.0;
}

static inline int Msta_TiCoupledInputEdge( MstaSense Sense, int fOutRise )
{
    if ( Sense == MSTA_SENSE_POSITIVE )
        return fOutRise;
    if ( Sense == MSTA_SENSE_NEGATIVE )
        return !fOutRise;
    return -1;
}

/* ---------------------------------------------------------------------
   跨模块内部接口（分别实现于 msta_graph / clock / prop / cppr / check / drc）
   --------------------------------------------------------------------- */

/* msta_graph.c */
MstaPin  *Msta_TiNetDriverPin( MstaTiming *p, MstaNet *pNet, int fMax );
MstaCell *Msta_TiTimingCellCorner( MstaTiming *p, MstaCell *pCell, int fMax );
MstaArc  *Msta_TiTimingArcById( MstaTiming *p, MstaCell *pCell, MstaId ArcId, int fMax );
MstaCell *Msta_TiTimingCombCell( MstaTiming *p, MstaCell *pCell, MstaId InPin, MstaId OutPin, int fMax );
MstaArc  *Msta_TiTimingCombArc( MstaTiming *p, MstaCell *pCell, MstaId InPin, MstaId OutPin, int fMax );
const MstaArc *Msta_TiTimingArcForCheckCorner( MstaTiming *p, const MstaArc *pArc,
                                               int nInst, int fMax );
int       Msta_TiFindNetLoadIdx( MstaTiming *p, int nNet, int nInst, int nPin );
double    Msta_TiNetLoadRaw( MstaTiming *p, int nNet, int fMax, int fFallback );
double    Msta_TiNetLoad( MstaTiming *p, int nNet, int fMax );
MstaArc  *Msta_TiRegClkToQArc( MstaTiming *p, MstaCell *pCell, MstaRegCheck *pReg,
                               MstaId OutPin, int *pfClkRises, int fMax, int nInst );
int       Msta_TiFillInstCaseValues( MstaTiming *p, int nInst, MstaInst *pInst,
                                     MstaCell *pEvalCell, signed char *pCases );
int       Msta_TiDataArcWhenActive( MstaTiming *p, int nInst, MstaInst *pInst,
                                    MstaCell *pEvalCell, const MstaArc *pArc );
void      Msta_TiMarkCombFanout( MstaTiming *p, char *pfMark, int *pQueue, int nQueue,
                                 int fStopAtIdealNoProp );
void      Msta_TiMarkIdealNets( MstaTiming *p );
void      Msta_TiBuildTopoOrder( MstaTiming *p );
void      Msta_TiComputeLogicLevels( MstaTiming *p );

/* msta_clock.c */
void      Msta_TiMarkClockNets( MstaTiming *p );
void      Msta_TiAttachClockIds( MstaTiming *p );
void      Msta_TiPropagateClocks( MstaTiming *p );

/* msta_prop.c */
void      Msta_TiEvalNetForward( MstaTiming *p, int fMax, int nNet,
                                 int nOnlyClock, int nOnlyStartClass );
void      Msta_TiPropagateData( MstaTiming *p, int fMax, int nOnlyClock, int nOnlyStartClass );
void      Msta_TiEvalNetRequired( MstaTiming *p, int fMax, int nNet );

/* msta_cppr.c */
void      Msta_TiBuildAocvDepths( MstaTiming *p );
double    Msta_TiAocvDelayFactor( MstaTiming *p, int nInst, int nClock, int nInNet, int nOutNet,
                                  int fMax, int fRise, int fClock );
MstaClockPrev Msta_TiLaunchClockPoint( MstaTiming *p, int nStart, int nClock, int fMax, int nSourceTag );
double    Msta_TiPathCppr( MstaTiming *p, int nStart, int nLaunchClock, int nCaptureClock,
                           int nCaptureNet, int fSetup, int nLaunchSource,
                           int nCaptureLocal, int nCaptureSource );
int       Msta_TiBuildStartClasses( MstaTiming *p );

/* msta_check.c */
void      Msta_TiBuildChecks( MstaTiming *p );
void      Msta_TiWarnUntimedLatches( MstaTiming *p );
void      Msta_TiFreeCheckCornerPath( MstaCheckCorner *pC );
void      Msta_TiFreeCheckPaths( MstaCheck *pCheck );
int       Msta_TiIsStartForClock( MstaTiming *p, int nNet, int nClock );
void      Msta_TiStartEndpoint( MstaTiming *p, int nNet, MstaSdcObject *pObj );
int       Msta_TiPathEndRise( MstaTiming *p, int nNet, int fMax );
void      Msta_TiEvaluateCandidatePaths( MstaTiming *p, int nClock, int nStartClass );
void      Msta_TiCheckDataChecks( MstaTiming *p, int nClock, char *pfDone );
void      Msta_TiCheckClockGating( MstaTiming *p, int nClock, int nStartClasses );
void      Msta_TiReevalSingleCheck( MstaTiming *p, int nCheckIdx, int nPropClock, int nStartClass );

/* msta_drc.c */
void      Msta_TiCheckDesignRules( MstaTiming *p );

/* ---------------------------------------------------------------------
   插件调度快速路径内联器（Wire / Delay / Variation / Refiner）
   --------------------------------------------------------------------- */

static inline double Msta_TiEffCapRaw( MstaTiming *p, int nNet, int fMax, int fClockPath, int fFallback )
{
    const MstaWireOps *pWire = p->Plugins.pWire ? p->Plugins.pWire : Msta_PluginWireZeroRc();
    MstaWireQuery Q;
    double Cap;
    Q.pTiming    = p;
    Q.pUserData  = pWire->pUserData;
    Q.nNet       = nNet;
    Q.nLoadInst  = -1;
    Q.nLoadPin   = -1;
    Q.nLoadIdx   = -1;
    Q.fMax       = fMax;
    Q.fRise      = 1;
    Q.fClockPath = fClockPath;
    Q.DriverSlew = MSTA_DEFAULT_SLEW;
    Cap = ( pWire->NetEffCap ? pWire->NetEffCap : Msta_PluginWireZeroRc()->NetEffCap )( &Q );
    if ( Cap > 0.0 )
        return Cap;
    return fFallback ? MSTA_DEFAULT_LOAD : 0.0;
}

static inline double Msta_TiEffCap( MstaTiming *p, int nNet, int fMax, int fClockPath )
{
    return Msta_TiEffCapRaw( p, nNet, fMax, fClockPath, 1 );
}

static inline void Msta_TiWireToPin( MstaTiming *p, int nNet, int nLoadInst, int nLoadPin,
                                     int fMax, int fRise, int fClockPath, double DriverSlew,
                                     double *pWireDelay, double *pSinkSlew )
{
    const MstaWireOps *pWire = p->Plugins.pWire;
    if ( pWire == NULL || pWire->EvalWireArc == NULL ||
         pWire == Msta_PluginWireZeroRc() || ( p->pfIdealNet && p->pfIdealNet[nNet] ) )
    {
        *pWireDelay = 0.0;
        *pSinkSlew = DriverSlew;
        return;
    }
    {
        MstaWireQuery Q;
        MstaWireResult Res;
        Q.pTiming    = p;
        Q.pUserData  = pWire->pUserData;
        Q.nNet       = nNet;
        Q.nLoadInst  = nLoadInst;
        Q.nLoadPin   = nLoadPin;
        Q.nLoadIdx   = Msta_TiFindNetLoadIdx( p, nNet, nLoadInst, nLoadPin );
        Q.fMax       = fMax;
        Q.fRise      = fRise;
        Q.fClockPath = fClockPath;
        Q.DriverSlew = DriverSlew;
        pWire->EvalWireArc( &Q, &Res );
        *pWireDelay = Res.WireDelay;
        *pSinkSlew = Res.SinkSlew >= 0.0 ? Res.SinkSlew : DriverSlew;
    }
}

static inline void Msta_TiEvalArc( MstaTiming *p, int nInst, int nInNet, int nOutNet, int nClock,
                                   const MstaArc *pArc, double InSlew, double EffCap,
                                   int fMax, int fOutRise, int fInRise, int fClockPath,
                                   MstaDelayResult *pOut )
{
    const MstaDelayOps *pDelay = p->Plugins.pDelay ? p->Plugins.pDelay : Msta_PluginDelayNldm();
    MstaDelayQuery Q;
    Q.pTiming    = p;
    Q.pUserData  = pDelay->pUserData;
    Q.nInst      = nInst;
    Q.nInNet     = nInNet;
    Q.nOutNet    = nOutNet;
    Q.nClock     = nClock;
    Q.pArc       = pArc;
    Q.InSlew     = InSlew;
    Q.EffCap     = EffCap;
    Q.fMax       = fMax;
    Q.fOutRise   = fOutRise;
    Q.fInRise    = fInRise;
    Q.fClockPath = fClockPath;
    ( pDelay->EvalArc ? pDelay->EvalArc : Msta_PluginDelayNldm()->EvalArc )( &Q, pOut );
    if ( pOut->OutSlew < 0.0 )
        pOut->OutSlew = MSTA_DEFAULT_SLEW;
}

static inline double Msta_TiEvalConstraint( MstaTiming *p, const MstaArc *pArc, int nInst,
                                            int fMax, int fDataRise, double ClkSlew, double DataSlew )
{
    const MstaDelayOps *pDelay = p->Plugins.pDelay;
    if ( pDelay != NULL && pDelay->EvalConstraint != NULL )
        return pDelay->EvalConstraint( p, pArc, nInst, fMax, fDataRise, ClkSlew, DataSlew );
    return Msta_PluginDelayNldm()->EvalConstraint( p, pArc, nInst, fMax, fDataRise, ClkSlew, DataSlew );
}

static inline double Msta_TiVarArcScale( MstaTiming *p, int nInst, int nClock, int nInNet, int nOutNet,
                                         int fMax, int fInRise, int fClockPath )
{
    const MstaVariationOps *pVar = p->Plugins.pVariation;
    if ( pVar != NULL && pVar->ArcScale != NULL )
        return pVar->ArcScale( p, nInst, nClock, nInNet, nOutNet, fMax, fInRise, fClockPath );
    return Msta_PluginVariationDefault()->ArcScale( p, nInst, nClock, nInNet, nOutNet,
                                                    fMax, fInRise, fClockPath );
}

static inline double Msta_TiVarCheckScale( MstaTiming *p, int nInst, int fMax, int fDataRise )
{
    const MstaVariationOps *pVar = p->Plugins.pVariation;
    if ( pVar != NULL && pVar->CheckScale != NULL )
        return pVar->CheckScale( p, nInst, fMax, fDataRise );
    return Msta_PluginVariationDefault()->CheckScale( p, nInst, fMax, fDataRise );
}

static inline double Msta_TiVarUncertainty( MstaTiming *p, int nLaunchClock, int nCaptureClock,
                                            int fSetup, int fLaunchRises, int fCaptureRises )
{
    const MstaVariationOps *pVar = p->Plugins.pVariation;
    if ( pVar != NULL && pVar->ClockUncertainty != NULL )
        return pVar->ClockUncertainty( p, nLaunchClock, nCaptureClock,
                                       fSetup, fLaunchRises, fCaptureRises );
    return Msta_PluginVariationDefault()->ClockUncertainty( p, nLaunchClock, nCaptureClock,
                                                            fSetup, fLaunchRises, fCaptureRises );
}

static inline double Msta_TiVarCpprCredit( MstaTiming *p, int nStart, int nLaunchClock,
                                           int nCaptureClock, int nCaptureNet, int fSetup,
                                           int nLaunchSource, int nCaptureLocal, int nCaptureSource )
{
    const MstaVariationOps *pVar = p->Plugins.pVariation;
    if ( pVar != NULL && pVar->CpprCredit != NULL )
        return pVar->CpprCredit( p, nStart, nLaunchClock, nCaptureClock, nCaptureNet,
                                 fSetup, nLaunchSource, nCaptureLocal, nCaptureSource );
    return Msta_PluginVariationDefault()->CpprCredit( p, nStart, nLaunchClock, nCaptureClock,
                                                      nCaptureNet, fSetup, nLaunchSource,
                                                      nCaptureLocal, nCaptureSource );
}

#endif /* MSTA_TIMING_INT_H */
