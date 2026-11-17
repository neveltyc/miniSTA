/**CFile***************************************************************

  FileName    [msta_prop.c]

  Synopsis    [到达时间（AT）前向传播与要求时间（RAT）后向传播。]

***********************************************************************/

#include "msta_timing_int.h"

/* 输入加载脚是否已被当前路径例外搜寻屏蔽（用于 -through inst/pin）。 */
static int IsLoadPinExcluded( const MstaTiming *p, int nInst, int nPin, int nInEdge )
{
    int i, Mask = nInEdge ? 1 : 2;
    for ( i = 0; i < p->nPathExclude; i++ )
        if ( p->vPathExclude[i].Inst == nInst && p->vPathExclude[i].Pin == nPin &&
             ( p->vPathExclude[i].EdgeMask & Mask ) )
            return 1;
    return 0;
}

/* 判断 set_driving_cell 选出的单元弧是否满足 -pin / -from_pin 与组合弧要求。 */
static int DriveCellArcMatch( const MstaCell *pDrive, const MstaArc *pArc,
                              const MstaNetCons *pCons )
{
    if ( !Msta_ArcIsComb( pDrive, pArc ) )
        return 0;
    if ( pCons->DrivingPin != MSTA_NO_ID && pArc->OutPin != pCons->DrivingPin )
        return 0;
    if ( pCons->DrivingFromPin != MSTA_NO_ID && pArc->InPin != pCons->DrivingFromPin )
        return 0;
    return 1;
}

typedef struct {
    double BestR, BestF;
    double SlewR, SlewF;
    int    nPrevR, nPrevF;
    int    nEdgeR, nEdgeF;
    int    nLoadPinR, nLoadPinF;
} MstaDataAccum;

/* 算一个时钟在输入端口上生成的 (Rise/Fall 到达、摆率、出发沿)；不合法时返回 0。 */
static int PropagateInputPortClock( MstaTiming *p, int fMax, int nNet,
                                    const MstaNetCons *pCons, int nClock,
                                    double *pCandR, double *pCandF,
                                    double *pSlewR, double *pSlewF,
                                    double *pEdge, int *pfClkFall )
{
    double Delay = Msta_SdcPortDelay( p->pSdc, nNet, nClock, 0, fMax );
    MstaClock *pClock;
    int fClkFall, nRefNet, fSourceRise;
    double Edge, SlewInR, SlewInF, Unset = fMax ? -MSTA_NO_TIME : MSTA_NO_TIME;

    if ( !Msta_IsSet( Delay ) )
        return 0;
    pClock = Msta_SdcClockByIndex( p->pSdc, nClock );
    if ( pClock == NULL )
        return 0;
    fClkFall = Msta_SdcPortClockFall( p->pSdc, nNet, nClock, 0, fMax );
    nRefNet = Msta_SdcPortReferenceNet( p->pSdc, nNet, nClock, 0, fMax );
    fSourceRise = !fClkFall;
    if ( nRefNet >= 0 )
    {
        MstaClockArr *pRefArr = Msta_TiClockArrAt( p, nClock, nRefNet );
        if ( !pRefArr->fReached )
            return 0;
        fSourceRise = Msta_TiClockTagSelect( pRefArr, fMax, !fClkFall, p->LaunchClockTag );
        if ( fSourceRise < 0 )
            return 0;
        fClkFall = !fSourceRise;
        Edge = ( fSourceRise ? pClock->RiseEdge : pClock->FallEdge )
             + Msta_TiClockArrival( pRefArr, fMax,
                                    !Msta_SdcPortClockFall(p->pSdc, nNet, nClock, 0, fMax),
                                    fSourceRise );
    }
    else
    {
        int fSourceIncluded = Msta_SdcPortSourceLatencyIncluded( p->pSdc, nNet, nClock, 0, fMax );
        int fNetworkIncluded = Msta_SdcPortNetworkLatencyIncluded( p->pSdc, nNet, nClock, 0, fMax );
        if ( p->LaunchClockTag >= 0 && p->LaunchClockTag != fSourceRise )
            return 0;
        Edge = fClkFall ? pClock->FallEdge : pClock->RiseEdge;
        if ( !fSourceIncluded )
            Edge += Msta_TiClockSourceArrival( p, nClock, fMax, fSourceRise );
        if ( !pClock->fPropagated && !fNetworkIncluded )
            Edge += pClock->NetworkLatency[fMax][fSourceRise];
    }
    SlewInR = fMax ? pCons->InputSlewMaxRise : pCons->InputSlewMinRise;
    SlewInF = fMax ? pCons->InputSlewMaxFall : pCons->InputSlewMinFall;
    *pCandR = *pCandF = Edge + Delay;
    *pSlewR = Msta_IsSet(SlewInR) ? SlewInR : MSTA_DEFAULT_SLEW;
    *pSlewF = Msta_IsSet(SlewInF) ? SlewInF : MSTA_DEFAULT_SLEW;
    /* set_driving_cell：把外部驱动单元当成一级，按端口实际负载查延迟和摆率。 */
    if ( pCons->DrivingCell != MSTA_NO_ID )
    {
        const char *pDriveName = Msta_NameStr( pCons->DrivingCell );
        MstaCell *pDrive = Msta_LibFindCell( p->pLib, pDriveName );
        if ( pDrive != NULL && strchr(pDriveName, '/') == NULL )
            pDrive = Msta_TiTimingCellCorner( p, pDrive, fMax );
        if ( pDrive != NULL )
        {
            double Load = Msta_TiEffCap( p, nNet, fMax, 0 );
            double InR = Msta_IsSet(pCons->DriveInSlewRise)
                       ? pCons->DriveInSlewRise : (Msta_IsSet(SlewInR) ? SlewInR : 0.0);
            double InF = Msta_IsSet(pCons->DriveInSlewFall)
                       ? pCons->DriveInSlewFall : (Msta_IsSet(SlewInF) ? SlewInF : 0.0);
            double Base = Edge + Delay;
            double DriveBestR = Unset, DriveBestF = Unset;
            double DriveSlewR = *pSlewR, DriveSlewF = *pSlewF;
            int DriveSet[2] = { 0, 0 };
            int a;
            for ( a = 0; a < pDrive->vArcs.nSize; a++ )
            {
                MstaArc *pArc = MstaArcArrayAt( &pDrive->vArcs, a );
                int e;
                if ( !DriveCellArcMatch( pDrive, pArc, pCons ) )
                    continue;
                for ( e = 0; e < 2; e++ )
                {
                    int Coupled = Msta_TiCoupledInputEdge( pArc->Sense, e ), nInEdge;
                    double *pDBest = e ? &DriveBestR : &DriveBestF;
                    double *pDSlew = e ? &DriveSlewR : &DriveSlewF;
                    for ( nInEdge = 0; nInEdge < 2; nInEdge++ )
                    {
                        double InSlew, Cand;
                        MstaDelayResult DR;
                        if ( Coupled >= 0 && nInEdge != Coupled )
                            continue;
                        InSlew = nInEdge ? InR : InF;
                        Msta_TiEvalArc( p, -1, -1, nNet, -1, pArc, InSlew, Load, fMax, e, nInEdge, 0, &DR );
                        Cand = Base + DR.Delay;
                        if ( !DriveSet[e] || ( fMax ? ( Cand > *pDBest ) : ( Cand < *pDBest ) ) )
                        {
                            DriveSet[e] = 1;
                            *pDBest = Cand;
                            *pDSlew = DR.OutSlew * pCons->DriveMultiply;
                        }
                    }
                }
            }
            if ( DriveSet[1] ) { *pCandR = DriveBestR; *pSlewR = DriveSlewR; }
            if ( DriveSet[0] ) { *pCandF = DriveBestF; *pSlewF = DriveSlewF; }
        }
    }
    *pEdge = Edge;
    *pfClkFall = fClkFall;
    return 1;
}

/* 算一个时钟在时序单元输出脚 Q 上生成的 (Rise/Fall 到达、摆率、出发沿)；不合法返回 0。 */
static int PropagateSeqPinClock( MstaTiming *p, int fMax, int nNet, int nDriverInst,
                                 MstaCell *pCell, MstaPin *pOutPin, double Load, int nClock,
                                 double *pCandR, double *pCandF,
                                 double *pSlewR, double *pSlewF,
                                 double *pEdge, int *pfClkRises )
{
    int k;
    MstaClock *pClock = Msta_SdcClockByIndex( p->pSdc, nClock );
    for ( k = 0; k < pCell->vRegs.nSize; k++ )
    {
        MstaRegCheck *pReg = MstaRegCheckArrayAt( &pCell->vRegs, k );
        MstaArc *pArc;
        MstaClockArr *pClkArr;
        int nClkPin = Msta_CellPinIndexOf( pCell, pReg->ClkPin );
        int nClkNet = (nClkPin >= 0 && nClkPin < Msta_TiInstAt(p, nDriverInst)->nPins)
                    ? Msta_TiInstAt(p, nDriverInst)->pNets[nClkPin] : -1;
        int fClkRises = pReg->fClkRises, fLocalRise;
        double ClkDrvSlew, ClkWireD = 0.0, ClkPinSlew, Edge;
        if ( nClkNet < 0 )
            continue;
        pArc = Msta_TiRegClkToQArc( p, pCell, pReg, pOutPin->Name, &fClkRises, fMax, nDriverInst );
        if ( pArc == NULL || !Msta_TiNetHasDataClock(p, nClock, nClkNet) )
            continue;
        pClkArr = Msta_TiClockArrAt( p, nClock, nClkNet );
        if ( !pClkArr->fReached )
            continue;
        fLocalRise = fClkRises;
        fClkRises = Msta_TiClockTagSelect( pClkArr, fMax, fLocalRise, p->LaunchClockTag );
        if ( fClkRises < 0 )
            continue;
        ClkDrvSlew = Msta_TiClockSlew( pClkArr, fMax, fLocalRise, fClkRises );
        Edge = Msta_TiClockArrival( pClkArr, fMax, fLocalRise, fClkRises );
        Msta_TiWireToPin( p, nClkNet, nDriverInst, nClkPin, fMax, fLocalRise, 1,
                          ClkDrvSlew, &ClkWireD, &ClkPinSlew );
        Edge += ClkWireD;
        if ( pClock != NULL )
            Edge += fClkRises ? pClock->RiseEdge : pClock->FallEdge;
        if ( p->pfIdealNet[nNet] )
        {
            *pCandR = *pCandF = Edge;
            *pSlewR = *pSlewF = ClkPinSlew;
        }
        else
        {
            double Scale = Msta_TiVarArcScale( p, nDriverInst, -1, -1, nNet, fMax, fLocalRise, 0 );
            MstaDelayResult DRR, DRF;
            Msta_TiEvalArc( p, nDriverInst, nClkNet, nNet, nClock, pArc, ClkPinSlew, Load, fMax, 1, fLocalRise, 0, &DRR );
            Msta_TiEvalArc( p, nDriverInst, nClkNet, nNet, nClock, pArc, ClkPinSlew, Load, fMax, 0, fLocalRise, 0, &DRF );
            *pCandR = Edge + DRR.Delay * Scale;
            *pCandF = Edge + DRF.Delay * Scale;
            *pSlewR = DRR.OutSlew;
            *pSlewF = DRF.OutSlew;
        }
        *pEdge = Edge;
        *pfClkRises = fClkRises;
        return 1;
    }
    return 0;
}

/* 把单条组合时序弧（pArc==NULL 表示缺弧零延迟回退）的候选传播结果更新进 pAcc。 */
static void PropagateCombPinArc( MstaTiming *p, int fMax, int nDriverInst, int nLoadPin,
                                 int nInNet, int nNet, double Load, const MstaArc *pArc,
                                 MstaDataAccum *pAcc )
{
    const MstaCorner *pCorner = Msta_TiCornerOf( p, fMax );
    MstaSense Sense = pArc ? pArc->Sense : MSTA_SENSE_UNKNOWN;
    int e;
    for ( e = 0; e < 2; e++ )
    {
        double *pBestE = e ? &pAcc->BestR : &pAcc->BestF;
        double *pSlewBest = e ? &pAcc->SlewR : &pAcc->SlewF;
        int *pPrevBest = e ? &pAcc->nPrevR : &pAcc->nPrevF;
        int *pEdgeBest = e ? &pAcc->nEdgeR : &pAcc->nEdgeF;
        int *pLoadPinBest = e ? &pAcc->nLoadPinR : &pAcc->nLoadPinF;
        int Coupled = Msta_TiCoupledInputEdge( Sense, e ), nInEdge;
        /* non_unate 必须比较完整候选延迟：输入到达较早的边沿
           也可能因 slew 或 OCV 系数成为 max 角的获胜路径。 */
        for ( nInEdge = 0; nInEdge < 2; nInEdge++ )
        {
            double InArr, InSlew, WireD = 0.0, PinSlew, Cand, CandSlew;
            if ( Coupled >= 0 && nInEdge != Coupled )
                continue;
            InArr = nInEdge ? pCorner->pArrRise[nInNet] : pCorner->pArrFall[nInNet];
            if ( !Msta_TiArrivalSet( InArr, fMax ) )
                continue;
            if ( IsLoadPinExcluded( p, nDriverInst, nLoadPin, nInEdge ) )
                continue;
            InSlew = nInEdge ? pCorner->pSlewRise[nInNet] : pCorner->pSlewFall[nInNet];
            Msta_TiWireToPin( p, nInNet, nDriverInst, nLoadPin, fMax, nInEdge, 0,
                              InSlew, &WireD, &PinSlew );
            if ( pArc == NULL || p->pfIdealNet[nNet] )
            {
                Cand = InArr + WireD;
                CandSlew = PinSlew;
            }
            else
            {
                MstaDelayResult DR;
                double Scale = Msta_TiVarArcScale( p, nDriverInst, -1, nInNet, nNet, fMax, nInEdge, 0 );
                Msta_TiEvalArc( p, nDriverInst, nInNet, nNet, -1, pArc, PinSlew, Load, fMax, e, nInEdge, 0, &DR );
                Cand = InArr + WireD + DR.Delay * Scale;
                CandSlew = DR.OutSlew;
            }
            if ( fMax ? ( Cand > *pBestE ) : ( Cand < *pBestE ) )
            {
                *pBestE = Cand;
                *pSlewBest = CandSlew;
                *pPrevBest = nInNet;
                *pEdgeBest = nInEdge;
                *pLoadPinBest = nLoadPin;
            }
        }
    }
}

/* 一个角的数据前向传播：按拓扑序求每根网络的升/降沿到达、摆率、前驱和起点信息。
   nOnlyClock >= 0 时只传由该时钟出发的数据；nOnlyClock < 0 时合并全设计所有时钟域，
   为 DRC 与网络级 AT/RAT/Slack 查询提供全局结果。 */
void Msta_TiEvalNetForward( MstaTiming *p, int fMax, int nNet, int nOnlyClock, int nOnlyStartClass )
{
    MstaDesign *pDes = p->pDes;
    MstaCorner *pCorner = Msta_TiCornerOf( p, fMax );
    double *pArrR  = pCorner->pArrRise;
    double *pArrF  = pCorner->pArrFall;
    double *pSlewR = pCorner->pSlewRise;
    double *pSlewF = pCorner->pSlewFall;
    double *pArrM  = pCorner->pArr;
    double *pSlewM = pCorner->pSlew;
    int    *pPrevNet  = pCorner->pPrevNet;
    MstaPrev *pPrevR  = pCorner->pPrevRise;
    MstaPrev *pPrevF  = pCorner->pPrevFall;
    int    *pSrcClock = pCorner->pnLaunchClock;
    double *pSrcEdge  = pCorner->pdLaunchEdge;
    char   *pSrcRises = pCorner->pfLaunchRises;
    double Unset = fMax ? -MSTA_NO_TIME : MSTA_NO_TIME;
    int nClocks = Msta_SdcClockCount( p->pSdc );
    MstaNet *pNet = MstaNetArrayAt( &pDes->vNets, nNet );
    MstaDataAccum Acc;
    int nDriverInst = pNet->Driver.InstId;

    Acc.BestR = Acc.BestF = Unset;
    Acc.SlewR = Acc.SlewF = MSTA_DEFAULT_SLEW;
    Acc.nPrevR = Acc.nPrevF = -1;
    Acc.nEdgeR = Acc.nEdgeF = -1;
    Acc.nLoadPinR = Acc.nLoadPinF = -1;

    pArrR[nNet] = pArrF[nNet] = Unset;
    pSlewR[nNet] = pSlewF[nNet] = MSTA_DEFAULT_SLEW;
    pPrevNet[nNet] = -1;
    pPrevR[nNet].Net = pPrevR[nNet].Edge = pPrevR[nNet].LoadPin = -1;
    pPrevF[nNet].Net = pPrevF[nNet].Edge = pPrevF[nNet].LoadPin = -1;
    pSrcClock[nNet] = -1;
    pSrcEdge[nNet] = 0.0;
    pSrcRises[nNet] = 1;

    if ( !pNet->fCaseValue && !p->pfClockNet[nNet] )
    {
        if ( nDriverInst == MSTA_NO_ID )
        {
            /* 情况 A：顶层输入端口。 */
            if ( nOnlyStartClass < 0 || p->pStartClass[nNet] == nOnlyStartClass )
            {
                MstaNetCons *pCons = Msta_SdcNetCons( p->pSdc, nNet );
                int cBeg = nOnlyClock >= 0 ? nOnlyClock : 0;
                int cEnd = nOnlyClock >= 0 ? nOnlyClock + 1 : nClocks;
                double BestOverall = Unset;
                int c;
                for ( c = cBeg; c < cEnd; c++ )
                {
                    double CandR, CandF, SlewR, SlewF, Edge, Worst;
                    int fClkFall;
                    if ( !PropagateInputPortClock(p, fMax, nNet, pCons, c,
                                                  &CandR, &CandF, &SlewR, &SlewF,
                                                  &Edge, &fClkFall) )
                        continue;
                    if ( fMax ? ( CandR > Acc.BestR ) : ( CandR < Acc.BestR ) )
                    {   Acc.BestR = CandR; Acc.SlewR = SlewR; }
                    if ( fMax ? ( CandF > Acc.BestF ) : ( CandF < Acc.BestF ) )
                    {   Acc.BestF = CandF; Acc.SlewF = SlewF; }
                    Worst = fMax ? fmax(CandR, CandF) : fmin(CandR, CandF);
                    if ( pSrcClock[nNet] < 0 ||
                         ( fMax ? ( Worst > BestOverall ) : ( Worst < BestOverall ) ) )
                    {
                        BestOverall = Worst;
                        pSrcClock[nNet] = c;
                        pSrcEdge[nNet]  = Edge;
                        pSrcRises[nNet] = (char)( fClkFall ? 0 : 1 );
                    }
                }
            }
        }
        else
        {
            MstaInst *pDriver = Msta_TiInstAt( p, nDriverInst );
            MstaCell *pCell = pDriver->pCell;
            MstaPin *pOutPin = MstaPinArrayAt( &pCell->vPins, pNet->Driver.PinId );
            double Load = Msta_TiEffCap( p, nNet, fMax, 0 );

            if ( pCell->fSequential )
            {
                /* 情况 B：FF/锁存器的输出脚作为路径起点。 */
                if ( nOnlyStartClass < 0 || p->pStartClass[nNet] == nOnlyStartClass )
                {
                    int cBeg = nOnlyClock >= 0 ? nOnlyClock : 0;
                    int cEnd = nOnlyClock >= 0 ? nOnlyClock + 1 : nClocks;
                    double BestOverall = Unset;
                    int c;
                    for ( c = cBeg; c < cEnd; c++ )
                    {
                        double CandR, CandF, SlewR, SlewF, Edge, Worst;
                        int fClkRises;
                        if ( !PropagateSeqPinClock(p, fMax, nNet, nDriverInst, pCell,
                                                   pOutPin, Load, c, &CandR, &CandF,
                                                   &SlewR, &SlewF, &Edge, &fClkRises) )
                            continue;
                        if ( fMax ? ( CandR > Acc.BestR ) : ( CandR < Acc.BestR ) )
                        {   Acc.BestR = CandR; Acc.SlewR = SlewR; }
                        if ( fMax ? ( CandF > Acc.BestF ) : ( CandF < Acc.BestF ) )
                        {   Acc.BestF = CandF; Acc.SlewF = SlewF; }
                        Worst = fMax ? fmax(CandR, CandF) : fmin(CandR, CandF);
                        if ( pSrcClock[nNet] < 0 ||
                             ( fMax ? ( Worst > BestOverall ) : ( Worst < BestOverall ) ) )
                        {
                            BestOverall = Worst;
                            pSrcClock[nNet] = c;
                            pSrcEdge[nNet]  = Edge;
                            pSrcRises[nNet] = (char)fClkRises;
                        }
                    }
                }
            }
            else
            {
                /* 情况 C：组合单元。按 timing_sense 把输入边沿耦合到输出边沿。 */
                int i;
                for ( i = 0; i < pDriver->nPins && i < pCell->vPins.nSize; i++ )
                {
                    MstaPin *pIn = MstaPinArrayAt( &pCell->vPins, i );
                    MstaCell *pCombCell;
                    int nInNet = pDriver->pNets[i], a, nCombArcs = 0;
                    if ( pIn->Dir == MSTA_DIR_OUTPUT || nInNet < 0 )
                        continue;
                    if ( p->pfClockNet[nInNet] )
                        continue;       /* 时钟脚由寄存器模型负责，不是数据弧 */
                    if ( nOnlyClock >= 0 ? ( pSrcClock[nInNet] != nOnlyClock )
                                         : ( pSrcClock[nInNet] < 0 ) )
                        continue;
                    if ( Msta_SdcTimingDisabled( p->pSdc, nDriverInst, pIn->Name, pOutPin->Name ) )
                        continue;
                    pCombCell = Msta_TiTimingCombCell( p, pCell, pIn->Name, pOutPin->Name, fMax );
                    for ( a = 0; a < pCombCell->vArcs.nSize; a++ )
                    {
                        MstaArc *pArc = MstaArcArrayAt( &pCombCell->vArcs, a );
                        if ( pArc->InPin == pIn->Name && pArc->OutPin == pOutPin->Name &&
                             Msta_ArcIsComb( pCombCell, pArc ) )
                            nCombArcs++;
                    }
                    if ( nCombArcs == 0 )
                    {
                        if ( Msta_CellHasAsyncArc( pCell, pIn->Name, pOutPin->Name ) )
                            continue;
                        if ( !pCell->fBlackBox )
                            Msta_WarnOnce( "单元 \"%s\" 没有从引脚 \"%s\" 到 \"%s\" 的时序弧",
                                           Msta_NameStr(pCell->Name), Msta_NameStr(pIn->Name),
                                           Msta_NameStr(pOutPin->Name) );
                        PropagateCombPinArc( p, fMax, nDriverInst, i, nInNet, nNet, Load, NULL, &Acc );
                        continue;
                    }
                    for ( a = 0; a < pCombCell->vArcs.nSize; a++ )
                    {
                        MstaArc *pArc = MstaArcArrayAt( &pCombCell->vArcs, a );
                        if ( pArc->InPin != pIn->Name || pArc->OutPin != pOutPin->Name ||
                             !Msta_ArcIsComb( pCombCell, pArc ) )
                            continue;
                        if ( !Msta_TiDataArcWhenActive( p, nDriverInst, pDriver, pCombCell, pArc ) )
                            continue;
                        PropagateCombPinArc( p, fMax, nDriverInst, i, nInNet, nNet, Load, pArc, &Acc );
                    }
                }
            }
        }
    }

    /* ---- 合并两个边沿：取该分析角更差的那个，并同步前驱与起点信息 ---- */
    {
        int nUseRise, i;
        double Best, v;
        v = Msta_SdcIdealLatency( p->pSdc, nNet, fMax, 1 );
        if ( Msta_IsSet(v) ) Acc.BestR = v;
        v = Msta_SdcIdealLatency( p->pSdc, nNet, fMax, 0 );
        if ( Msta_IsSet(v) ) Acc.BestF = v;
        v = Msta_SdcIdealTransition( p->pSdc, nNet, fMax, 1 );
        if ( Msta_IsSet(v) ) Acc.SlewR = v;
        v = Msta_SdcIdealTransition( p->pSdc, nNet, fMax, 0 );
        if ( Msta_IsSet(v) ) Acc.SlewF = v;

        for ( i = 0; i < p->nPathExclude; i++ )
            if ( p->vPathExclude[i].Inst < 0 && p->vPathExclude[i].Net == nNet )
            {
                if ( p->vPathExclude[i].EdgeMask & 1 )
                {   Acc.BestR = Unset; Acc.SlewR = MSTA_DEFAULT_SLEW;   }
                if ( p->vPathExclude[i].EdgeMask & 2 )
                {   Acc.BestF = Unset; Acc.SlewF = MSTA_DEFAULT_SLEW;   }
            }
        nUseRise = fMax ? ( Acc.BestR >= Acc.BestF ) : ( Acc.BestR <= Acc.BestF );
        Best = nUseRise ? Acc.BestR : Acc.BestF;
        pPrevR[nNet].Net     = Acc.nPrevR;
        pPrevR[nNet].Edge    = Acc.nEdgeR;
        pPrevR[nNet].LoadPin = Acc.nLoadPinR;
        pPrevF[nNet].Net     = Acc.nPrevF;
        pPrevF[nNet].Edge    = Acc.nEdgeF;
        pPrevF[nNet].LoadPin = Acc.nLoadPinF;
        pArrR[nNet]  = Acc.BestR;
        pArrF[nNet]  = Acc.BestF;
        pSlewR[nNet] = Acc.SlewR;
        pSlewF[nNet] = Acc.SlewF;
        if ( Msta_TiArrivalSet( Best, fMax ) )
        {
            pArrM[nNet]  = Best;
            pSlewM[nNet] = nUseRise ? Acc.SlewR : Acc.SlewF;
            pPrevNet[nNet] = nUseRise ? Acc.nPrevR : Acc.nPrevF;
            if ( pPrevNet[nNet] >= 0 )
            {
                pSrcClock[nNet] = pSrcClock[pPrevNet[nNet]];
                pSrcEdge[nNet]  = pSrcEdge[pPrevNet[nNet]];
                pSrcRises[nNet] = pSrcRises[pPrevNet[nNet]];
            }
        }
        else
        {
            pArrM[nNet]  = 0.0;
            pSlewM[nNet] = MSTA_DEFAULT_SLEW;
        }
    }
}

void Msta_TiPropagateData( MstaTiming *p, int fMax, int nOnlyClock, int nOnlyStartClass )
{
    int q;
    for ( q = 0; q < p->nTopoOrder; q++ )
        Msta_TiEvalNetForward( p, fMax, p->pTopoOrder[q], nOnlyClock, nOnlyStartClass );
}

/* 沿着指定输入引脚/组合弧从输出侧要求时间反推到输入侧。 */
static void EvalRequiredPinArcInto( MstaTiming *p, int fMax, int nDriverInst,
                                      int nLoadPin, int nInNet, int nOutNet,
                                      double Load, const MstaArc *pArc,
                                      double *pInReqRise, double *pInReqFall )
{
    const MstaCorner *pCorner = Msta_TiCornerOf( p, fMax );
    MstaSense Sense = pArc ? pArc->Sense : MSTA_SENSE_UNKNOWN;
    int eOut;
    for ( eOut = 0; eOut < 2; eOut++ )
    {
        double OutReq, IdealLat;
        int Coupled, nInEdge;
        IdealLat = Msta_SdcIdealLatency( p->pSdc, nOutNet, fMax, eOut );
        if ( Msta_IsSet(IdealLat) )
            continue;
        OutReq = eOut ? pCorner->pReqRise[nOutNet] : pCorner->pReqFall[nOutNet];
        if ( !Msta_TimingTimeValid( OutReq ) )
            continue;
        Coupled = Msta_TiCoupledInputEdge( Sense, eOut );
        for ( nInEdge = 0; nInEdge < 2; nInEdge++ )
        {
            double InArr, InSlew, WireD = 0.0, PinSlew, CellD = 0.0, CandReq;
            double *pInReq;
            if ( Coupled >= 0 && nInEdge != Coupled )
                continue;
            InArr = nInEdge ? pCorner->pArrRise[nInNet] : pCorner->pArrFall[nInNet];
            if ( !Msta_TiArrivalSet( InArr, fMax ) )
                continue;
            InSlew = nInEdge ? pCorner->pSlewRise[nInNet] : pCorner->pSlewFall[nInNet];
            Msta_TiWireToPin( p, nInNet, nDriverInst, nLoadPin, fMax, nInEdge, 0,
                              InSlew, &WireD, &PinSlew );
            if ( pArc != NULL && !p->pfIdealNet[nOutNet] )
            {
                MstaDelayResult DR;
                double Scale = Msta_TiVarArcScale( p, nDriverInst, -1, nInNet, nOutNet,
                                                   fMax, nInEdge, 0 );
                Msta_TiEvalArc( p, nDriverInst, nInNet, nOutNet, -1, pArc, PinSlew, Load,
                                fMax, eOut, nInEdge, 0, &DR );
                CellD = DR.Delay * Scale;
            }
            CandReq = OutReq - ( WireD + CellD );
            pInReq = nInEdge ? pInReqRise : pInReqFall;
            if ( fMax ? ( CandReq < *pInReq ) : ( CandReq > *pInReq ) )
                *pInReq = CandReq;
        }
    }
}

static void EvalRequiredCombPinOut( MstaTiming *p, int fMax, int nDriverInst, int nLoadPin,
                                    const MstaPin *pIn, int nInNet, int nOutNet,
                                    const MstaPin *pOutPin, MstaInst *pDriver, MstaCell *pCell,
                                    double Load, double *pInReqRise, double *pInReqFall )
{
    MstaCell *pCombCell;
    int a, nCombArcs = 0;
    if ( Msta_SdcTimingDisabled( p->pSdc, nDriverInst, pIn->Name, pOutPin->Name ) )
        return;
    pCombCell = Msta_TiTimingCombCell( p, pCell, pIn->Name, pOutPin->Name, fMax );
    for ( a = 0; a < pCombCell->vArcs.nSize; a++ )
    {
        MstaArc *pArc = MstaArcArrayAt( &pCombCell->vArcs, a );
        if ( pArc->InPin == pIn->Name && pArc->OutPin == pOutPin->Name &&
             Msta_ArcIsComb( pCombCell, pArc ) )
            nCombArcs++;
    }
    if ( nCombArcs == 0 )
    {
        if ( !Msta_CellHasAsyncArc( pCell, pIn->Name, pOutPin->Name ) )
            EvalRequiredPinArcInto( p, fMax, nDriverInst, nLoadPin, nInNet, nOutNet,
                                    Load, NULL, pInReqRise, pInReqFall );
        return;
    }
    for ( a = 0; a < pCombCell->vArcs.nSize; a++ )
    {
        MstaArc *pArc = MstaArcArrayAt( &pCombCell->vArcs, a );
        if ( pArc->InPin != pIn->Name || pArc->OutPin != pOutPin->Name ||
             !Msta_ArcIsComb( pCombCell, pArc ) )
            continue;
        if ( !Msta_TiDataArcWhenActive( p, nDriverInst, pDriver, pCombCell, pArc ) )
            continue;
        EvalRequiredPinArcInto( p, fMax, nDriverInst, nLoadPin, nInNet, nOutNet,
                                Load, pArc, pInReqRise, pInReqFall );
    }
}

static void TightenNetReqFromCheck( const MstaCheck *pCheck, int fMax,
                                    double *pReqRise, double *pReqFall )
{
    const MstaCheckCorner *pC = fMax ? &pCheck->Setup : &pCheck->Hold;
    int e;
    if ( !pC->fChecked )
        return;
    if ( pC->fEdgeReq[0] || pC->fEdgeReq[1] )
    {
        for ( e = 0; e < 2; e++ )
        {
            double *pTarget = e ? pReqRise : pReqFall;
            if ( !pC->fEdgeReq[e] )
                continue;
            if ( fMax ? ( pC->EdgeReq[e] < *pTarget ) : ( pC->EdgeReq[e] > *pTarget ) )
                *pTarget = pC->EdgeReq[e];
        }
    }
    else
    {
        double EndReq = pC->Required - pC->LaunchShift;
        double *pTarget = pC->fDataRise ? pReqRise : pReqFall;
        if ( fMax ? ( EndReq < *pTarget ) : ( EndReq > *pTarget ) )
            *pTarget = EndReq;
    }
}

static void UpdateNetReqSummary( MstaCorner *pCorner, int nNet, int fMax )
{
    double UnsetReq = fMax ? MSTA_NO_TIME : -MSTA_NO_TIME;
    double R = pCorner->pReqRise[nNet], F = pCorner->pReqFall[nNet];
    int fR = Msta_TimingTimeValid( R ), fF = Msta_TimingTimeValid( F );
    if ( fR && fF )
        pCorner->pReq[nNet] = fMax ? fmin(R, F) : fmax(R, F);
    else if ( fR )
        pCorner->pReq[nNet] = R;
    else if ( fF )
        pCorner->pReq[nNet] = F;
    else
        pCorner->pReq[nNet] = UnsetReq;
}

/* 拉取式（pull）单网络后向 RAT 求值：供逆拓扑分级增量队列使用（结果允许变松也允许变紧）。 */
void Msta_TiEvalNetRequired( MstaTiming *p, int fMax, int nInNet )
{
    MstaDesign *pDes = p->pDes;
    MstaCorner *pCorner = Msta_TiCornerOf( p, fMax );
    MstaNet *pInNetObj = MstaNetArrayAt( &pDes->vNets, nInNet );
    double UnsetReq = fMax ? MSTA_NO_TIME : -MSTA_NO_TIME;
    double ReqR = pCorner->pAuxReqRise ? pCorner->pAuxReqRise[nInNet] : UnsetReq;
    double ReqF = pCorner->pAuxReqFall ? pCorner->pAuxReqFall[nInNet] : UnsetReq;
    int i;

    if ( p->pFirstCheck != NULL )
    {
        for ( i = p->pFirstCheck[nInNet]; i >= 0; i = p->pNextCheck[i] )
            TightenNetReqFromCheck( MstaCheckArrayAt(&p->vChecks, i), fMax, &ReqR, &ReqF );
    }

    if ( !p->pfClockNet[nInNet] )
    {
        for ( i = 0; i < pInNetObj->vLoads.nSize; i++ )
        {
            MstaPinRef *pRef = MstaPinRefArrayAt( &pInNetObj->vLoads, i );
            int nDriverInst = pRef->InstId, nLoadPin = pRef->PinId, o;
            MstaInst *pDriver = Msta_TiInstAt( p, nDriverInst );
            MstaCell *pCell = pDriver->pCell;
            const MstaPin *pIn;
            if ( pCell->fSequential || nLoadPin < 0 || nLoadPin >= pCell->vPins.nSize )
                continue;
            pIn = MstaPinArrayAt( &pCell->vPins, nLoadPin );
            for ( o = 0; o < pDriver->nPins && o < pCell->vPins.nSize; o++ )
            {
                const MstaPin *pOutPin = MstaPinArrayAt( &pCell->vPins, o );
                int nOutNet = pDriver->pNets[o];
                MstaNet *pOutNetObj;
                double Load;
                if ( pOutPin->Dir != MSTA_DIR_OUTPUT || nOutNet < 0 || nOutNet >= pDes->vNets.nSize )
                    continue;
                pOutNetObj = MstaNetArrayAt( &pDes->vNets, nOutNet );
                if ( pOutNetObj->Driver.InstId != nDriverInst || pOutNetObj->Driver.PinId != o )
                    continue;
                if ( pOutNetObj->fCaseValue || p->pfClockNet[nOutNet] )
                    continue;
                if ( !Msta_TimingTimeValid(pCorner->pReqRise[nOutNet]) &&
                     !Msta_TimingTimeValid(pCorner->pReqFall[nOutNet]) )
                    continue;
                Load = Msta_TiEffCap( p, nOutNet, fMax, 0 );
                EvalRequiredCombPinOut( p, fMax, nDriverInst, nLoadPin, pIn, nInNet,
                                        nOutNet, pOutPin, pDriver, pCell, Load, &ReqR, &ReqF );
            }
        }
    }

    pCorner->pReqRise[nInNet] = ReqR;
    pCorner->pReqFall[nInNet] = ReqF;
    UpdateNetReqSummary( pCorner, nInNet, fMax );
}

/* 教科书式逆拓扑序要求时间（RAT）后向传播：从各端点检查的要求时间（还原到第 0 拍
   到达坐标系）逆着拓扑序反推，计算设计里每根网络的升/降沿要求时间与余量。 */
void Msta_TimingPropagateRequired( MstaTiming *p, int fMax )
{
    MstaDesign *pDes = p->pDes;
    MstaCorner *pCorner = Msta_TiCornerOf( p, fMax );
    int nNets = pDes->vNets.nSize;
    double UnsetReq = fMax ? MSTA_NO_TIME : -MSTA_NO_TIME;
    int i, q, n;

    for ( n = 0; n < nNets; n++ )
    {
        pCorner->pReq[n] = UnsetReq;
        pCorner->pReqRise[n] = ( pCorner->pAuxReqRise != NULL ) ? pCorner->pAuxReqRise[n] : UnsetReq;
        pCorner->pReqFall[n] = ( pCorner->pAuxReqFall != NULL ) ? pCorner->pAuxReqFall[n] : UnsetReq;
    }

    /* 1. 由已评估的端点检查放入终点要求时间（去掉 LaunchShift 与终点线延迟以对齐第 0 拍 AT）。 */
    for ( i = 0; i < p->vChecks.nSize; i++ )
    {
        MstaCheck *pCheck = MstaCheckArrayAt( &p->vChecks, i );
        int nEnd = pCheck->nEndNet;
        if ( nEnd < 0 || nEnd >= nNets )
            continue;
        TightenNetReqFromCheck( pCheck, fMax, &pCorner->pReqRise[nEnd], &pCorner->pReqFall[nEnd] );
    }

    /* 2. 逆拓扑序遍历所有组合驱动网络，把要求时间反推到扇入输入网络（在 FF.Q/PI 处自然停止）。 */
    for ( q = p->nTopoOrder - 1; q >= 0; q-- )
    {
        int nOutNet = p->pTopoOrder[q];
        MstaNet *pNet = MstaNetArrayAt( &pDes->vNets, nOutNet );
        int nDriverInst = pNet->Driver.InstId;
        MstaInst *pDriver;
        MstaCell *pCell;
        MstaPin *pOutPin;
        double Load;

        if ( pNet->fCaseValue || p->pfClockNet[nOutNet] || nDriverInst == MSTA_NO_ID )
            continue;
        if ( !Msta_TimingTimeValid(pCorner->pReqRise[nOutNet]) &&
             !Msta_TimingTimeValid(pCorner->pReqFall[nOutNet]) )
            continue;
        pDriver = Msta_TiInstAt( p, nDriverInst );
        pCell = pDriver->pCell;
        if ( pCell->fSequential )
            continue;
        if ( pNet->Driver.PinId < 0 || pNet->Driver.PinId >= pCell->vPins.nSize )
            continue;
        pOutPin = MstaPinArrayAt( &pCell->vPins, pNet->Driver.PinId );
        Load = Msta_TiEffCap( p, nOutNet, fMax, 0 );

        for ( i = 0; i < pDriver->nPins && i < pCell->vPins.nSize; i++ )
        {
            MstaPin *pIn = MstaPinArrayAt( &pCell->vPins, i );
            int nInNet = pDriver->pNets[i];
            if ( pIn->Dir == MSTA_DIR_OUTPUT || nInNet < 0 )
                continue;
            if ( p->pfClockNet[nInNet] )
                continue;
            EvalRequiredCombPinOut( p, fMax, nDriverInst, i, pIn, nInNet,
                                    nOutNet, pOutPin, pDriver, pCell, Load,
                                    &pCorner->pReqRise[nInNet], &pCorner->pReqFall[nInNet] );
        }
    }

    /* 3. 汇总每根网络的最紧要求时间（只做摘要用；计算 Slack 仍应逐边沿比较）。 */
    for ( n = 0; n < nNets; n++ )
        UpdateNetReqSummary( pCorner, n, fMax );
}
