/**CFile***************************************************************

  FileName    [msta_clock.c]

  Synopsis    [时钟树：网络标记、极性推导、生成时钟衔接与时钟到达/摆率传播。]

***********************************************************************/

#include "msta_timing_int.h"

/* 正向标记：从 create_clock 的源网络出发，沿组合时序弧向输出传播。
   只沿真实 arc 走，所以 ICG 的 enable/scan 侧不会被误标成时钟。 */
void Msta_TiMarkClockNets( MstaTiming *p )
{
    MstaDesign *pDes = p->pDes;
    int nNets = pDes->vNets.nSize;
    int *pQueue = (int *)malloc( (size_t)(nNets > 0 ? nNets : 1) * sizeof(int) );
    int nQueue = 0, c;
    assert( pQueue );

    for ( c = 0; c < Msta_SdcClockCount( p->pSdc ); c++ )
    {
        int nNet = Msta_SdcClockByIndex( p->pSdc, c )->SourceNet;
        if ( nNet >= 0 && !p->pfClockNet[nNet] )
        {
            p->pfClockNet[nNet] = 1;
            pQueue[nQueue++] = nNet;
        }
    }

    Msta_TiMarkCombFanout( p, p->pfClockNet, pQueue, nQueue, 0 );
    free( pQueue );
}

static int ClockArcSense( MstaTiming *p, int nInst, MstaInst *pInst,
                          MstaCell *pEvalCell, const MstaArc *pArc )
{
    signed char aCases[64];
    signed char *pCases = aCases;
    int fKnown, Sense;
    if ( pEvalCell->vPins.nSize > (int)sizeof(aCases) )
    {
        pCases = (signed char *)malloc( (size_t)pEvalCell->vPins.nSize );
        assert( pCases );
    }
    fKnown = Msta_TiFillInstCaseValues( p, nInst, pInst, pEvalCell, pCases );
    Sense = fKnown ? Msta_LibClockSense( pEvalCell, pArc, pCases ) : pArc->Sense;
    if ( pCases != aCases )
        free( pCases );
    return Sense;
}

/* 记下"这根网络在这个时钟的树里"，同时定它的时钟极性。
   set_clock_sense -positive/-negative 给的显式值优先。 */
static void MarkClockNetOf( MstaTiming *p, int nClock, int nNet, int Polarity )
{
    int fStop = 0;
    int nSet = Msta_SdcClockSense( p->pSdc, nNet, nClock, &fStop );
    if ( nSet != 0 )
        Polarity = nSet;
    Msta_TiClockArrAt( p, nClock, nNet )->Polarity = Polarity;
    p->pfClockOfNet[ (size_t)nClock * (size_t)p->pDes->vNets.nSize + (size_t)nNet ] = 1;
}

/* 这个时钟在这里停下吗（set_clock_sense -stop_propagation）。 */
static int ClockStopsAt( MstaTiming *p, int nClock, int nNet )
{
    int fStop = 0;
    (void)Msta_SdcClockSense( p->pSdc, nNet, nClock, &fStop );
    return fStop || Msta_TiClockArrAt( p, nClock, nNet )->fSourceOnly;
}

/* 每个时钟的时钟树里有哪些网络：从 create_clock 的源网络正向铺开，同时沿
   组合弧的 timing_sense 累积时钟极性（反相器把极性取反）。
   一根网络可以同时属于多个时钟（create_clock -add）。 */
void Msta_TiAttachClockIds( MstaTiming *p )
{
    MstaDesign *pDes = p->pDes;
    int n, c, nNets = pDes->vNets.nSize;
    int *pQueue = (int *)malloc( (size_t)(2 * nNets) * sizeof(int) );
    assert( pQueue );

    memset( p->pfClockOfNet, 0, (size_t)nNets * (size_t)Msta_SdcClockCount(p->pSdc) );
    for ( c = 0; c < Msta_SdcClockCount( p->pSdc ); c++ )
    {
        MstaClock *pChild = Msta_SdcClockByIndex( p->pSdc, c );
        int nMaster = Msta_SdcClockIndexOf( p->pSdc, pChild->MasterClock );
        if ( nMaster >= 0 && pChild->SourceNet >= 0 )
            Msta_TiClockArrAt( p, nMaster, pChild->SourceNet )->fSourceOnly = 1;
    }

    for ( c = 0; c < Msta_SdcClockCount( p->pSdc ); c++ )
    {
        MstaClock *pClock = Msta_SdcClockByIndex( p->pSdc, c );
        int nQueue = 0;
        if ( pClock->SourceNet < 0 )
            continue;                       /* 虚拟时钟没有源网络 */
        if ( !p->pfClockOfNet[ (size_t)c * (size_t)nNets + (size_t)pClock->SourceNet ] )
            MarkClockNetOf( p, c, pClock->SourceNet, 1 );
        pQueue[nQueue++] = pClock->SourceNet;
        for ( n = 0; n < nQueue; n++ )
        {
            MstaNet *pNet = MstaNetArrayAt( &pDes->vNets, pQueue[n] );
            int i;
            if ( pQueue[n] != pClock->SourceNet && ( pNet->fConst || pNet->fCaseValue ) )
                continue;
            if ( ClockStopsAt( p, c, pQueue[n] ) )
                continue;                   /* set_clock_sense -stop_propagation */
            for ( i = 0; i < pNet->vLoads.nSize; i++ )
            {
                MstaPinRef *pRef = MstaPinRefArrayAt( &pNet->vLoads, i );
                MstaInst *pLoad = Msta_TiInstAt( p, pRef->InstId );
                MstaCell *pCell = pLoad->pCell;
                MstaPin *pInPin = MstaPinArrayAt( &pCell->vPins, pRef->PinId );
                int m;
                /* 时钟往下走只经过组合单元（缓冲/反相器/ICG）；到 FF 时钟脚就停。 */
                if ( pCell->fSequential )
                    continue;
                for ( m = 0; m < pLoad->nPins && m < pCell->vPins.nSize; m++ )
                {
                    MstaPin *pPin = MstaPinArrayAt( &pCell->vPins, m );
                    MstaArc *pArc;
                    MstaNet *pOutNet;
                    int nOut = pLoad->pNets[m];
                    int Polarity, a;
                    if ( pPin->Dir != MSTA_DIR_OUTPUT || nOut < 0 )
                        continue;
                    if ( !p->pfClockNet[nOut] )
                        continue;
                    pOutNet = MstaNetArrayAt( &pDes->vNets, nOut );
                    if ( pOutNet->fConst || pOutNet->fCaseValue )
                        continue;
                    if ( Msta_SdcTimingDisabled( p->pSdc, pRef->InstId,
                                                 pInPin->Name, pPin->Name ) )
                        continue;
                    for ( a = 0; a < pCell->vArcs.nSize; a++ )
                    {
                        int Sense;
                        pArc = MstaArcArrayAt( &pCell->vArcs, a );
                        if ( pArc->InPin != pInPin->Name || pArc->OutPin != pPin->Name ||
                             !Msta_ArcIsComb( pCell, pArc ) )
                            continue;
                        Sense = ClockArcSense( p, pRef->InstId, pLoad, pCell, pArc );
                        if ( Sense < 0 )
                            continue;
                        Polarity = Msta_TiClockArrAt( p, c, pQueue[n] )->Polarity;
                        if ( Sense == MSTA_SENSE_NEGATIVE )
                            Polarity = -Polarity;
                        else if ( Sense == MSTA_SENSE_NONUNATE )
                            Polarity = 0;
                        else if ( Sense == MSTA_SENSE_UNKNOWN )
                            Msta_WarnOnce( "时钟路径经过的单元 \"%s\" 没有 timing_sense；"
                                           "按不反相处理时钟极性",
                                           Msta_NameStr(pCell->Name) );
                        if ( !p->pfClockOfNet[ (size_t)c * (size_t)nNets + (size_t)nOut ] )
                        {
                            MarkClockNetOf( p, c, nOut, Polarity );
                            pQueue[nQueue++] = nOut;
                        }
                        else
                        {
                            int fStop = 0, Old = Msta_TiClockArrAt( p, c, nOut )->Polarity;
                            int Force = Msta_SdcClockSense( p->pSdc, nOut, c, &fStop );
                            int Joined = Force ? Force : (Old == Polarity ? Old : 0);
                            if ( Old != Joined )
                            {
                                MarkClockNetOf( p, c, nOut, Joined );
                                pQueue[nQueue++] = nOut;
                            }
                        }
                    }
                }
            }
        }
    }
    free( pQueue );
}

/* =====================================================================
   5. 时钟传播（拓扑序排好之后调用）
   ===================================================================== */

static void ApplyIdealClockValues( MstaTiming *p, int nNet, MstaClockArr *pArr )
{
    int m, e, s;
    for ( m = 0; m < 2; m++ )
        for ( e = 0; e < 2; e++ )
        {
            double v = Msta_SdcIdealLatency( p->pSdc, nNet, m, e );
            if ( Msta_IsSet(v) )
                for ( s = 0; s < 2; s++ )
                    if ( Msta_TiClockTagValid(pArr, m, e, s) )
                        pArr->Arrival[m][e][s] = v;
            v = Msta_SdcIdealTransition( p->pSdc, nNet, m, e );
            if ( Msta_IsSet(v) )
                for ( s = 0; s < 2; s++ )
                    if ( Msta_TiClockTagValid(pArr, m, e, s) )
                        pArr->Slew[m][e][s] = v;
        }
    Msta_TiMergeClockSummary( pArr );
}

/* 生成时钟继承主时钟到源引脚的状态；时序输出还要加入对应 clk-to-Q。 */
static void SeedGeneratedClock( MstaTiming *p, int nClock, MstaClock *pClock )
{
    MstaNet *pNet;
    MstaInst *pDriver;
    MstaCell *pCell;
    MstaPin *pOut;
    int i, nMaster;
    if ( pClock->MasterClock == MSTA_NO_ID || pClock->SourceNet < 0 )
        return;
    pNet = MstaNetArrayAt( &p->pDes->vNets, pClock->SourceNet );
    if ( pNet->Driver.InstId == MSTA_NO_ID )
        return;
    pDriver = Msta_TiInstAt( p, pNet->Driver.InstId );
    pCell = pDriver->pCell;
    nMaster = Msta_SdcClockIndexOf( p->pSdc, pClock->MasterClock );
    if ( nMaster < 0 )
        return;
    if ( !pCell->fSequential )
    {
        MstaClockArr *pSrc = Msta_TiClockArrAt( p, nMaster, pClock->SourceNet );
        MstaClockArr *pDst = Msta_TiClockArrAt( p, nClock, pClock->SourceNet );
        int m, e;
        if ( !Msta_TiNetHasClock(p, nMaster, pClock->SourceNet) || !pSrc->fReached )
        {
            pDst->fReached = 0;
            Msta_WarnOnce( "生成时钟 \"%s\"：主时钟到达不了它的源引脚",
                           Msta_NameStr(pClock->Name) );
            return;
        }
        for ( m = 0; m < 2; m++ )
            for ( e = 0; e < 2; e++ )
            {
                int Label = Msta_TiEffectiveClkRises( p, nClock, pClock->SourceNet, e );
                pDst->Tags[e] |= 1 << Label;
                pDst->Arrival[m][e][Label] = Msta_TiClockArrival(pSrc, m, e, -1)
                                           + pClock->SourceLatency[m][Label];
                pDst->Prev[m][e][Label] = (MstaClockPrev){
                    nMaster, pClock->SourceNet, e, Msta_TiClockTagSelect(pSrc, m, e, -1), -1, -1
                };
                if ( pClock->fPropagated )
                    pDst->Slew[m][e][Label] = Msta_TiClockSlew( pSrc, m, e, -1 );
            }
        pDst->fReached = 1;
        pDst->nThroughGates = pSrc->nThroughGates;
        ApplyIdealClockValues( p, pClock->SourceNet, pDst );
        return;
    }
    pOut = MstaPinArrayAt( &pCell->vPins, pNet->Driver.PinId );
    for ( i = 0; i < pCell->vRegs.nSize; i++ )
    {
        MstaRegCheck *pReg = MstaRegCheckArrayAt( &pCell->vRegs, i );
        int nClkNet = Msta_TiInstPinNet( p, pNet->Driver.InstId, pReg->ClkPin );
        MstaClockArr *pDst, *pSrc;
        MstaArc *pArcMax, *pArcMin;
        double LoadMax, LoadMin;
        int fClkRises, m, e;
        if ( nClkNet < 0 || !Msta_TiNetHasClock(p, nMaster, nClkNet) ||
             !Msta_TiClockArrAt(p, nMaster, nClkNet)->fReached )
            continue;
        pArcMax = Msta_TiRegClkToQArc( p, pCell, pReg, pOut->Name, &fClkRises, 1, pNet->Driver.InstId );
        pArcMin = Msta_TiRegClkToQArc( p, pCell, pReg, pOut->Name, &fClkRises, 0, pNet->Driver.InstId );
        if ( pArcMax == NULL || pArcMin == NULL )
            continue;
        LoadMax = Msta_TiEffCap( p, pClock->SourceNet, 1, 1 );
        LoadMin = Msta_TiEffCap( p, pClock->SourceNet, 0, 1 );
        pDst = Msta_TiClockArrAt( p, nClock, pClock->SourceNet );
        pSrc = Msta_TiClockArrAt( p, nMaster, nClkNet );
        pDst->fReached = 1;
        for ( m = 0; m < 2; m++ )
            for ( e = 0; e < 2; e++ )
            {
                MstaArc *pArc = m ? pArcMax : pArcMin;
                int fClkRise = Msta_TiArcClockRises( pArc );
                int nClkPin = Msta_CellPinIndexOf( pCell, pReg->ClkPin );
                double DrvArr = Msta_TiClockArrival( pSrc, m, fClkRise, -1 );
                double DrvSlew = Msta_TiClockSlew( pSrc, m, fClkRise, -1 );
                double WireD = 0.0, PinSlew = DrvSlew, Scale;
                MstaDelayResult DR;
                int Label = Msta_TiEffectiveClkRises( p, nClock, pClock->SourceNet, e );
                Msta_TiWireToPin( p, nClkNet, pNet->Driver.InstId, nClkPin, m, fClkRise, 1,
                                  DrvSlew, &WireD, &PinSlew );
                Msta_TiEvalArc( p, pNet->Driver.InstId, nClkNet, pClock->SourceNet, nMaster,
                                pArc, PinSlew, m ? LoadMax : LoadMin, m, e, fClkRise, 1, &DR );
                Scale = Msta_TiVarArcScale( p, pNet->Driver.InstId, nMaster, nClkNet,
                                            pClock->SourceNet, m, fClkRise, 1 );
                pDst->Tags[e] |= 1 << Label;
                pDst->Arrival[m][e][Label] = DrvArr + WireD + DR.Delay * Scale
                    + pClock->SourceLatency[m][Label];
                pDst->Prev[m][e][Label] = (MstaClockPrev){
                    nMaster, nClkNet, fClkRise, Msta_TiClockTagSelect( pSrc, m, fClkRise, -1 ),
                    pNet->Driver.InstId, nClkPin
                };
                if ( pClock->fPropagated )
                    pDst->Slew[m][e][Label] = DR.OutSlew;
            }
        ApplyIdealClockValues( p, pClock->SourceNet, pDst );
        pDst->nThroughGates = pSrc->nThroughGates + 1;
        return;
    }
    {
        MstaClockArr *pDst = Msta_TiClockArrAt( p, nClock, pClock->SourceNet );
        if ( pClock->fPropagated )
        {
            int m, e;
            for ( m = 0; m < 2; m++ )
                for ( e = 0; e < 2; e++ )
                    pDst->Slew[m][e][Msta_TiEffectiveClkRises(p, nClock, pClock->SourceNet, e)] = 0.0;
            ApplyIdealClockValues( p, pClock->SourceNet, pDst );
        }
    }
    Msta_WarnOnce( "生成时钟 \"%s\"：无法推算经过时序单元的源延迟；"
                   "改用标注的源延迟，传播 slew 取 0",
                   Msta_NameStr(pClock->Name) );
}

void Msta_TiPropagateClocks( MstaTiming *p )
{
    MstaDesign *pDes = p->pDes;
    int nClocks = Msta_SdcClockCount( p->pSdc ), n, c, m, e, s, q;
    for ( c = 0; c < nClocks; c++ )
    {
        MstaClock *pClock = Msta_SdcClockByIndex( p->pSdc, c );
        MstaNetCons *pCons = pClock->SourceNet >= 0
                          ? Msta_SdcNetCons( p->pSdc, pClock->SourceNet ) : NULL;
        for ( n = 0; n < pDes->vNets.nSize; n++ )
        {
            MstaClockArr *pArr = Msta_TiClockArrAt( p, c, n );
            pArr->fReached = pArr->nThroughGates = 0;
            pArr->Tags[0] = pArr->Tags[1] = 0;
            memset( pArr->Prev, -1, sizeof(pArr->Prev) );
            for ( m = 0; m < 2; m++ )
                for ( e = 0; e < 2; e++ )
                    for ( s = 0; s < 2; s++ )
                    {
                        double InputSlew = pCons
                            ? (m ? (s ? pCons->InputSlewMaxRise : pCons->InputSlewMaxFall)
                                 : (s ? pCons->InputSlewMinRise : pCons->InputSlewMinFall))
                            : MSTA_UNSET;
                        double ClkSlew = pClock->Slew[m][s], Slew;
                        if ( !pClock->fPropagated )
                            Slew = Msta_IsSet(ClkSlew) ? ClkSlew : 0.0;
                        else if ( pClock->fPropagatedSet )
                            Slew = Msta_IsSet(InputSlew) ? InputSlew : 0.0;
                        else
                            Slew = Msta_IsSet(InputSlew) ? InputSlew
                                 : Msta_IsSet(ClkSlew)   ? ClkSlew : MSTA_DEFAULT_SLEW;
                        pArr->Slew[m][e][s] = Slew;
                        pArr->Arrival[m][e][s] = m ? -MSTA_NO_TIME : MSTA_NO_TIME;
                        if ( Msta_TiNetHasClock(p, c, n) &&
                             (n == pClock->SourceNet || !pClock->fPropagated) &&
                             (pArr->Polarity == 0 || s == Msta_TiEffectiveClkRises(p, c, n, e)) )
                        {
                            pArr->fReached = 1;
                            pArr->Tags[e] |= 1 << s;
                            pArr->Arrival[m][e][s] = pClock->SourceLatency[m][s];
                        }
                    }
            ApplyIdealClockValues( p, n, pArr );
        }
    }
    for ( c = 0; c < nClocks; c++ )
    {
        MstaClock *pClock = Msta_SdcClockByIndex( p->pSdc, c );
        SeedGeneratedClock( p, c, pClock );
        if ( !pClock->fPropagated )
        {
            if ( pClock->SourceNet >= 0 )
            {
                MstaClockArr Source = *Msta_TiClockArrAt( p, c, pClock->SourceNet );
                for ( n = 0; n < pDes->vNets.nSize; n++ )
                    if ( Msta_TiNetHasClock(p, c, n) )
                    {
                        MstaClockArr *pArr = Msta_TiClockArrAt( p, c, n );
                        for ( m = 0; m < 2; m++ )
                            for ( e = 0; e < 2; e++ )
                                for ( s = 0; s < 2; s++ )
                                    if ( Msta_TiClockTagValid(pArr, m, e, s) )
                                    {
                                        pArr->Arrival[m][e][s] = Msta_TiClockArrival( &Source, m, s, s );
                                        if ( !p->pfIdealNet[n] )
                                            pArr->Arrival[m][e][s] += pClock->NetworkLatency[m][s];
                                    }
                        ApplyIdealClockValues( p, n, pArr );
                    }
            }
            continue;
        }
        for ( q = 0; q < p->nTopoOrder; q++ )
        {
            int nNet = p->pTopoOrder[q], k, i, fAny = 0, nGates = 0;
            MstaNet *pNet = MstaNetArrayAt( &pDes->vNets, nNet );
            MstaClockArr *pArr = Msta_TiClockArrAt( p, c, nNet );
            MstaInst *pDriver;
            MstaCell *pCell;
            if ( !Msta_TiNetHasClock(p, c, nNet) || nNet == pClock->SourceNet ||
                 pNet->fCaseValue || pNet->Driver.InstId < 0 )
                continue;
            pDriver = Msta_TiInstAt( p, pNet->Driver.InstId );
            pCell = pDriver->pCell;
            if ( pCell->fSequential )
                continue;
            for ( k = 0; k < pDriver->nPins && k < pCell->vPins.nSize; k++ )
            {
                MstaPin *pPin = MstaPinArrayAt( &pCell->vPins, k );
                int nIn = pDriver->pNets[k], Stop = 0;
                int Force = Msta_SdcClockSense( p->pSdc, nNet, c, &Stop );
                MstaClockArr *pInArr;
                MstaId OutPinName;
                if ( pPin->Dir == MSTA_DIR_OUTPUT || nIn < 0 ||
                     !Msta_TiNetHasClock(p, c, nIn) || !Msta_TiClockArrAt(p, c, nIn)->fReached )
                    continue;
                pInArr = Msta_TiClockArrAt( p, c, nIn );
                OutPinName = MstaPinArrayAt( &pCell->vPins, pNet->Driver.PinId )->Name;
                if ( Msta_SdcTimingDisabled(p->pSdc, pNet->Driver.InstId, pPin->Name, OutPinName) )
                    continue;
                for ( m = 0; m < 2; m++ )
                {
                    MstaCell *pCombCell = Msta_TiTimingCombCell( p, pCell, pPin->Name, OutPinName, m );
                    double Load = Msta_TiEffCap( p, nNet, m, 1 );
                    int a, fAnyCorner = 0;
                    for ( a = 0; a < pCombCell->vArcs.nSize; a++ )
                    {
                        MstaArc *pArc = MstaArcArrayAt( &pCombCell->vArcs, a );
                        int Sense;
                        if ( pArc->InPin != pPin->Name || pArc->OutPin != OutPinName ||
                             !Msta_ArcIsComb( pCombCell, pArc ) )
                            continue;
                        Sense = ClockArcSense( p, pNet->Driver.InstId, pDriver, pCombCell, pArc );
                        if ( Sense < 0 )
                            continue;
                        fAnyCorner = 1;
                        for ( e = 0; e < 2; e++ )
                            for ( i = 0; i < 2; i++ )
                            {
                                if ( Sense != MSTA_SENSE_NONUNATE &&
                                     i != (Sense == MSTA_SENSE_NEGATIVE ? !e : e) )
                                    continue;
                                for ( s = 0; s < 2; s++ )
                                {
                                    int Label = Force ? (Force > 0 ? e : !e) : s;
                                    double Cand, Slew, WireD = 0.0, PinSlew;
                                    if ( !Msta_TiClockTagValid(pInArr, m, i, s) )
                                        continue;
                                    Cand = Msta_TiClockArrival( pInArr, m, i, s );
                                    Slew = Msta_TiClockSlew( pInArr, m, i, s );
                                    Msta_TiWireToPin( p, nIn, pNet->Driver.InstId, k, m, i, 1,
                                                      Slew, &WireD, &PinSlew );
                                    Cand += WireD;
                                    Slew = PinSlew;
                                    if ( !p->pfIdealNet[nNet] )
                                    {
                                        MstaDelayResult DR;
                                        double Scale = Msta_TiVarArcScale( p, pNet->Driver.InstId, c,
                                                                           nIn, nNet, m, i, 1 );
                                        Msta_TiEvalArc( p, pNet->Driver.InstId, nIn, nNet, c, pArc,
                                                        PinSlew, Load, m, e, i, 1, &DR );
                                        Cand += DR.Delay * Scale;
                                        Slew = DR.OutSlew;
                                    }
                                    if ( !Msta_TiClockTagValid(pArr, m, e, Label) ||
                                         (m ? Cand > pArr->Arrival[m][e][Label]
                                            : Cand < pArr->Arrival[m][e][Label]) )
                                    {
                                        pArr->Arrival[m][e][Label] = Cand;
                                        pArr->Prev[m][e][Label] = (MstaClockPrev){
                                            c, nIn, i, s, pNet->Driver.InstId, k
                                        };
                                        pArr->Slew[m][e][Label] = Slew;
                                    }
                                    pArr->Tags[e] |= 1 << Label;
                                    fAny = 1;
                                }
                            }
                    }
                    if ( fAnyCorner && pInArr->nThroughGates + 1 > nGates )
                        nGates = pInArr->nThroughGates + 1;
                }
            }
            if ( fAny )
            {
                pArr->fReached = 1;
                pArr->nThroughGates = nGates;
                ApplyIdealClockValues( p, nNet, pArr );
            }
        }
    }
}

int Msta_TimingClockNetCount( MstaTiming *p )
{
    int n, c = 0;
    for ( n = 0; n < p->pDes->vNets.nSize; n++ )
        if ( p->pfClockNet[n] )
            c++;
    return c;
}

int Msta_TimingClockCount( MstaTiming *p )
{
    return Msta_SdcClockCount( p->pSdc );
}

int Msta_TimingNetHasClock( MstaTiming *p, int nClock, int nNet )
{
    return Msta_TiNetHasClock( p, nClock, nNet );
}

const MstaClockArr *Msta_TimingClockArr( MstaTiming *p, int nClock, int nNet )
{
    return Msta_TiClockArrAt( p, nClock, nNet );
}
