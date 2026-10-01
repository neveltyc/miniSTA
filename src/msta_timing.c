/**CFile***************************************************************

  FileName    [msta_timing.c]

  Synopsis    [时序引擎：端点 -> 时钟树 -> 拓扑序 -> 时钟传播 -> 到达与检查 -> 汇总。]

  分节编号就是 Msta_TimingAnalyze 里的步骤号（流程见 msta_timing.h 文件头）。
  阅读顺序大体就是执行顺序，例外是第 5 步时钟传播：它紧跟时钟树和理想网络，
  但要等第 4 步拓扑序排好才调用；总入口直接调用的几个函数放在第 7 节。
  每一步只做一件事，接口都是本文件内的 static 函数，外部只见
  msta_timing.h 里那几个。

***********************************************************************/

#include <math.h>
#include "msta_timing.h"
#include "msta_util.h"

#define MSTA_DEFAULT_SLEW   50.0     /* ps：摆率缺省值，没约束也没表可查时用（如无 input_transition
                                        的起点、缺转换表的弧、源 slew 都没给的传播时钟） */
#define MSTA_DEFAULT_LOAD   5.0      /* fF：一个网络没有扇出时查表用的负载 */
#define MSTA_NO_TIME        (1e30)

/* =====================================================================
   0. 小工具
   ===================================================================== */

static MstaInst *InstAt( MstaTiming *p, int nInst )
{
    return MstaInstArrayAt( &p->pDes->vInsts, nInst );
}

static MstaCorner *CornerOf( MstaTiming *p, int fMax )
{
    return fMax ? &p->CornerMax : &p->CornerMin;
}

/* 时钟树状态按 [时钟][网络] 存（create_clock -add 允许一根网络挂多个时钟）。 */
static MstaClockArr *ClockArrAt( MstaTiming *p, int nClock, int nNet )
{
    return &p->pClockArr[ (size_t)nClock * (size_t)p->pDes->vNets.nSize + (size_t)nNet ];
}

/* 这根网络在这个时钟的时钟树里吗。 */
static int NetHasClock( MstaTiming *p, int nClock, int nNet )
{
    return nClock >= 0 && nNet >= 0 &&
           p->pfClockOfNet[ (size_t)nClock * (size_t)p->pDes->vNets.nSize + (size_t)nNet ];
}

/* 生成时钟的定义点仍保存主时钟的插入延迟，但数据检查由生成时钟接管。 */
static int NetHasDataClock( MstaTiming *p, int nClock, int nNet )
{
    return NetHasClock(p,nClock,nNet) && !ClockArrAt(p,nClock,nNet)->fSourceOnly;
}

/* 寄存器在这个时钟上的有效触发沿：库定义的边沿按时钟极性取反。
   时钟路径上有反相器（或 set_clock_sense -negative）时，源上的上升沿到
   CLK 脚已经是下降沿，反过来也一样。 */
static int EffectiveClkRises( MstaTiming *p, int nClock, int nClkNet, int fRegRises )
{
    if ( nClock < 0 || nClkNet < 0 )
        return fRegRises;
    return ClockArrAt( p, nClock, nClkNet )->Polarity < 0 ? !fRegRises : fRegRises;
}

/* (本地沿, 源沿) 的含义见 msta_timing.h 的 MstaClockArr。Tags 里有这个源沿、
   且到达已算出才有效；两个源沿是两条时钟路径，不能合并后检查。 */
static int ClockTagValid( const MstaClockArr *pArr, int fMax, int Local, int Source )
{
    if ( Local < 0 || Local > 1 || Source < 0 || Source > 1 ) return 0;
    double v = pArr->Arrival[fMax][Local][Source];
    return (pArr->Tags[Local] & (1 << Source)) && v > -MSTA_NO_TIME / 2 && v < MSTA_NO_TIME / 2;
}

/* 选本地沿 Local 上用哪个源沿：Want>=0 只认这个源沿；Want=-1 两种都看，取该角
   最差的（max 取晚、min 取早）。返回选中的源沿，没有可用的返回 -1。 */
static int ClockTagSelect( const MstaClockArr *pArr, int fMax, int Local, int Want )
{
    if ( Want >= 0 ) return ClockTagValid(pArr,fMax,Local,Want) ? Want : -1;
    if ( !ClockTagValid(pArr,fMax,Local,0) ) return ClockTagValid(pArr,fMax,Local,1) ? 1 : -1;
    if ( !ClockTagValid(pArr,fMax,Local,1) ) return 0;
    double R = pArr->Arrival[fMax][Local][1], F = pArr->Arrival[fMax][Local][0];
    return fMax ? R >= F : R <= F;
}

static double ClockArrival( const MstaClockArr *pArr, int fMax, int Local, int Source )
{
    int s = ClockTagSelect(pArr,fMax,Local,Source);
    return s >= 0 ? pArr->Arrival[fMax][Local][s] : (fMax ? -MSTA_NO_TIME : MSTA_NO_TIME);
}

static double ClockSlew( const MstaClockArr *pArr, int fMax, int Local, int Source )
{
    int s = ClockTagSelect(pArr,fMax,Local,Source);
    return s >= 0 ? pArr->Slew[fMax][Local][s] : MSTA_DEFAULT_SLEW;
}

static void MergeClockSummary( MstaClockArr *pArr )
{
    int e, s;
    pArr->MaxArrival = -MSTA_NO_TIME;
    pArr->MaxSlew = 0;
    for ( e = 0; e < 2; e++ )
        for ( s = 0; s < 2; s++ )
            if ( ClockTagValid(pArr,1,e,s) )
            {
                pArr->MaxArrival = fmax(pArr->MaxArrival,pArr->Arrival[1][e][s]);
                pArr->MaxSlew = fmax(pArr->MaxSlew,pArr->Slew[1][e][s]);
            }
    if ( pArr->MaxArrival == -MSTA_NO_TIME ) pArr->MaxArrival = 0;
}

/* 时钟源延迟（source latency）：主时钟取 SDC 给的值；生成时钟取它源网络上的到达，
   其中含从主时钟继承的延迟。理想生成时钟的这个到达里已加过 network latency
   （见 PropagateClocks），而调用方会按需另加，所以先减掉以免重复计入。 */
static double ClockSourceArrival( MstaTiming *p, int nClock, int fMax, int fSourceRise )
{
    MstaClock *pClock = Msta_SdcClockByIndex(p->pSdc,nClock);
    double Arrival;
    if ( pClock->MasterClock == MSTA_NO_ID || pClock->SourceNet < 0 )
        return pClock->SourceLatency[fMax][fSourceRise];
    MstaClockArr *pSource = ClockArrAt(p,nClock,pClock->SourceNet);
    int Local = ClockTagValid(pSource,fMax,fSourceRise,fSourceRise) ? fSourceRise : !fSourceRise;
    Arrival = ClockArrival(pSource,fMax,Local,fSourceRise);
    if ( !pClock->fPropagated && !p->pfIdealNet[pClock->SourceNet] )
        Arrival -= pClock->NetworkLatency[fMax][fSourceRise];
    return Arrival;
}

static int ArcClockRises( const MstaArc *pArc )
{
    return pArc->Type != MSTA_TT_SETUP_FALLING && pArc->Type != MSTA_TT_HOLD_FALLING &&
           pArc->Type != MSTA_TT_RECOVERY_FALLING && pArc->Type != MSTA_TT_REMOVAL_FALLING &&
           pArc->Type != MSTA_TT_FALL_EDGE;
}

/* 某实例的某个脚（按脚名）连到的网络号；没连返回 -1。 */
static int InstPinNet( MstaTiming *p, int nInst, MstaId PinName )
{
    MstaInst *pInst = InstAt( p, nInst );
    int nPin = Msta_CellPinIndexOf( pInst->pCell, PinName );
    if ( nPin < 0 || pInst->pNets[nPin] == MSTA_NO_ID )
        return -1;
    return pInst->pNets[nPin];
}

/* 查一张延迟表；缺表时退回另一张（rise/fall 互为备份）。 */
static double ArcDelayEdge( const MstaArc *pArc, double Slew, double Load, int fRise )
{
    const MstaTable *pMain = fRise ? &pArc->DelayRise : &pArc->DelayFall;
    const MstaTable *pAlt = fRise ? &pArc->DelayFall : &pArc->DelayRise;
    return Msta_TableExists(pMain) ? Msta_TableLookup(pMain,Slew,Load)
                                   : (Msta_TableExists(pAlt) ? Msta_TableLookup(pAlt,Slew,Load) : 0.0);
}

/* 输出摆率用：查一张转换表，缺表时退回另一张。 */
static double ArcSlewEdge( const MstaArc *pArc, double Slew, double Load, int fRise )
{
    const MstaTable *pMain = fRise ? &pArc->TransRise : &pArc->TransFall;
    const MstaTable *pAlt = fRise ? &pArc->TransFall : &pArc->TransRise;
    if ( !Msta_TableExists(pMain) && !Msta_TableExists(pAlt) ) return MSTA_DEFAULT_SLEW;
    double v = Msta_TableLookup(Msta_TableExists(pMain) ? pMain : pAlt,Slew,Load);
    return v >= 0.0 ? v : MSTA_DEFAULT_SLEW;
}

/* 网络的驱动脚（没有驱动返回 NULL）。 */
static MstaPin *NetDriverPin( MstaTiming *p, MstaNet *pNet, int fMax )
{
    MstaInst *pInst;
    if ( pNet->Driver.InstId == MSTA_NO_ID )
        return NULL;
    pInst = InstAt( p, pNet->Driver.InstId );
    return Msta_LibPinForCorner(p->pLib,pInst->pCell,pNet->Driver.PinId,
                                Msta_SdcOperatingLibrary(p->pSdc,fMax));
}

/* set_driving_cell 的驱动单元：挑一条组合弧（-pin 指定输出脚、-from_pin 指定输入脚）。 */
static MstaArc *DriveCellArc( MstaCell *pDrive, MstaNetCons *pCons )
{
    int i;
    for ( i = 0; i < pDrive->vArcs.nSize; i++ )
    {
        MstaArc *pArc = MstaArcArrayAt( &pDrive->vArcs, i );
        if ( pCons->DrivingPin != MSTA_NO_ID && pArc->OutPin != pCons->DrivingPin )
            continue;
        if ( pCons->DrivingFromPin != MSTA_NO_ID && pArc->InPin != pCons->DrivingFromPin )
            continue;
        return pArc;
    }
    return NULL;
}

/* 取当前 max/min corner 的 cell definition；没有 alternate library cell 时回退默认值。 */
static MstaCell *TimingCellCorner( MstaTiming *p, MstaCell *pCell, int fMax )
{
    MstaId Library = Msta_SdcOperatingLibrary(p->pSdc,fMax);
    MstaCell *pCornerCell = Msta_LibCellForCorner(p->pLib,pCell,Library);
    if ( pCell != NULL && Library != MSTA_NO_ID && pCornerCell == pCell &&
         pCell->LibName != Library )
        Msta_WarnOnce("corner library \"%s\" lacks some design cells; those cells use their default library definitions",
                      Msta_NameStr(Library));
    return pCornerCell;
}

/* 取当前 corner 的等价 arc。ArcId 来自默认 cell 的结构索引。 */
static MstaArc *TimingArcById( MstaTiming *p, MstaCell *pCell, MstaId ArcId, int fMax )
{
    MstaArc *pArc = Msta_LibArcForCorner(p->pLib,pCell,ArcId,
                                         Msta_SdcOperatingLibrary(p->pSdc,fMax));
    if ( pArc == NULL && pCell != NULL && ArcId >= 0 && ArcId < pCell->vArcs.nSize )
    {
        Msta_WarnOnce("selected corner library \"%s\" lacks some timing arcs; missing arcs use the default cell definition",
                      Msta_NameStr(Msta_SdcOperatingLibrary(p->pSdc,fMax)));
        pArc = Msta_CellArcById(pCell,ArcId);
    }
    return pArc;
}

/* 按当前 corner 在对应 cell variant 上找组合弧。 */
static MstaArc *TimingCombArc( MstaTiming *p, MstaCell *pCell,
                               MstaId InPin, MstaId OutPin, int fMax )
{
    MstaCell *pCornerCell = TimingCellCorner(p,pCell,fMax);
    MstaArc *pArc = Msta_CellCombArc(pCornerCell,InPin,OutPin);
    if ( pArc == NULL && pCornerCell != pCell )
    {
        Msta_WarnOnce("selected corner library \"%s\" lacks some combinational arcs",
                      Msta_NameStr(Msta_SdcOperatingLibrary(p->pSdc,fMax)));
        pArc = Msta_CellCombArc(pCell,InPin,OutPin);
    }
    return pArc;
}

/* 一根网络的负载 = 所有扇入脚的输入电容之和（+ set_load 指定的那部分）。
   fFallback=1 时，没有负载的网络用一个默认值顶上（查表用）。 */
static double NetLoadRaw( MstaTiming *p, int nNet, int fMax, int fFallback )
{
    MstaNet *pNet = MstaNetArrayAt( &p->pDes->vNets, nNet );
    MstaNetCons *pCons = Msta_SdcNetCons( p->pSdc, nNet );
    double Sum = 0.0;
    int i;
    for ( i = 0; i < pNet->vLoads.nSize; i++ )
    {
        MstaPinRef *pRef = MstaPinRefArrayAt( &pNet->vLoads, i );
        MstaInst *pLoad = InstAt( p, pRef->InstId );
        MstaPin *pPin = Msta_LibPinForCorner(p->pLib,pLoad->pCell,pRef->PinId,
                                Msta_SdcOperatingLibrary(p->pSdc,fMax));
        Sum += pPin->Cap;
    }
    if ( pCons )
    {
        double Extra = fMax ? pCons->LoadMax : pCons->LoadMin;
        if ( Msta_IsSet(Extra) )
        {
            /* -subtract_pin_load：给的这个值就是总负载，脚电容不另加。 */
            if ( pCons->fSubtractPinLoad )
                return Extra;
            Sum += Extra;
        }
    }
    if ( Sum > 0.0 )
        return Sum;
    return fFallback ? MSTA_DEFAULT_LOAD : 0.0;
}

static double NetLoad( MstaTiming *p, int nNet, int fMax )
{
    return NetLoadRaw( p, nNet, fMax, 1 );
}

/* 找一个输出脚的 clk2q 弧：检查项里记下的 Q 脚优先；同一个寄存器还有另一个
   输出脚（比如网表用的是 QN）时，按"时钟脚 -> 这个输出脚"的边沿弧现找一条。 */
static MstaArc *RegClkToQArc( MstaTiming *p, MstaCell *pCell,
                              MstaRegCheck *pReg, MstaId OutPin,
                              int *pfClkRises, int fMax, int nInst )
{
    if ( Msta_SdcTimingDisabled(p->pSdc,nInst,pReg->ClkPin,OutPin) ) return NULL;
    MstaCell *pCornerCell = TimingCellCorner(p,pCell,fMax);
    int i;
    if ( pReg->QPin == OutPin && pReg->ClkToQArc != MSTA_NO_ID )
        return TimingArcById( p,pCell,pReg->ClkToQArc,fMax );
    for ( i = 0; i < pCornerCell->vArcs.nSize; i++ )
    {
        MstaArc *pArc = MstaArcArrayAt( &pCornerCell->vArcs, i );
        int fEdge = ( pArc->Type == MSTA_TT_RISE_EDGE || pArc->Type == MSTA_TT_FALL_EDGE ||
                      pArc->Type == MSTA_TT_RISE_BOTH || pArc->Type == MSTA_TT_FALL_BOTH );
        if ( !fEdge || pArc->OutPin != OutPin || pArc->InPin != pReg->ClkPin )
            continue;
        *pfClkRises = pReg->fClkRises;
        return pArc;
    }
    return NULL;
}

static double ClockPeriod( MstaTiming *p, int nClock )
{
    MstaClock *pClock = Msta_SdcClockByIndex( p->pSdc, nClock );
    return ( pClock && pClock->Period > 0.0 ) ? pClock->Period : 0.0;
}

/* 一条检查的不确定度：带起止时钟和边沿，交给约束层决定用哪条。 */
static double ClockUncertainty( MstaTiming *p, int nLaunchClock, int nCaptureClock,
                                int fSetup, int fLaunchRises, int fCaptureRises )
{
    return Msta_SdcClockUncertainty( p->pSdc, nLaunchClock, nCaptureClock,
                                     fSetup, fLaunchRises, fCaptureRises );
}

/* 同一个逻辑弧按 min/max 库映射。 */
static const MstaArc *CheckArcForCorner( MstaTiming *p, const MstaArc *pArc,
                                       int nInst, int fMax )
{
    if ( pArc != NULL && nInst >= 0 )
    {
        MstaCell *pCell = InstAt(p,nInst)->pCell;
        int i;
        for ( i = 0; i < pCell->vArcs.nSize; i++ )
            if ( pArc == MstaArcArrayAt(&pCell->vArcs,i) )
            { pArc = TimingArcById(p,pCell,(MstaId)i,fMax); break; }
    }
    return pArc;
}

/* 约束弧只给了一个数据边沿的表时，另一个边沿不做这项检查；弧不存在或
   两张表都没有时照常检查，检查值按 0 算（见 CheckTime）。 */
static int CheckHasEdge( MstaTiming *p, const MstaArc *pArc, int nInst,
                         int fMax, int fDataRise )
{
    pArc = CheckArcForCorner(p,pArc,nInst,fMax);
    if ( pArc == NULL || (!Msta_TableExists(&pArc->ConstraintRise) &&
                          !Msta_TableExists(&pArc->ConstraintFall)) ) return 1;
    return Msta_TableExists(fDataRise ? &pArc->ConstraintRise : &pArc->ConstraintFall);
}

/* 约束表按数据边沿选择；模板负责把 (时钟摆率, 数据摆率) 映射到索引轴。 */
static double CheckTime( MstaTiming *p, const MstaArc *pArc, int nInst,
                         int nClock, int nClkNet, int nDataNet, int fMax, int fDataRise )
{
    double ClkSlew, DataSlew;
    pArc = CheckArcForCorner(p,pArc,nInst,fMax);
    if ( pArc == NULL ) return 0.0;
    MstaClockArr *pClkArr = ClockArrAt( p, nClock, nClkNet );
    /* setup 捕获时钟取 early，hold 取 late；检查表与数据仍取各自分析角。 */
    ClkSlew = ClockSlew(pClkArr,!fMax,ArcClockRises(pArc),p->CaptureClockTag);
    DataSlew = fDataRise ? CornerOf(p,fMax)->pSlewRise[nDataNet]
                         : CornerOf(p,fMax)->pSlewFall[nDataNet];
    if ( ClkSlew < 0.0 )
        ClkSlew = MSTA_DEFAULT_SLEW;
    if ( DataSlew < 0.0 )
        DataSlew = MSTA_DEFAULT_SLEW;
    const MstaTable *pTable = fDataRise ? &pArc->ConstraintRise : &pArc->ConstraintFall;
    return (Msta_TableExists(pTable) ? Msta_TableLookup(pTable, ClkSlew, DataSlew) : 0.0)
           * Msta_SdcCheckDerateInst(p->pSdc,nInst,fMax);
}

/* =====================================================================
   1. 端点
   ===================================================================== */

static void BuildChecks( MstaTiming *p )
{
    MstaDesign *pDes = p->pDes;
    int i, k;

    for ( i = 0; i < pDes->vInsts.nSize; i++ )
    {
        MstaInst *pInst = InstAt( p, i );
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
                continue;                      /* 数据脚接常量：不需要检查 */

            pNew = MstaCheckArrayAppend( &p->vChecks );
            pNew->InstId        = (MstaId)i;
            pNew->nCheck        = k;
            pNew->nDataPin      = nDataPin;
            pNew->nEndNet       = nDataNet;
            pNew->fToRegister   = 1;
        }
        for ( k = 0; k < pCell->vAsync.nSize; k++ )
        {
            MstaAsyncCheck *pAsync = MstaAsyncCheckArrayAt(&pCell->vAsync,k);
            int nAsyncPin = Msta_CellPinIndexOf(pCell,pAsync->AsyncPin);
            int nClkPin = Msta_CellPinIndexOf(pCell,pAsync->ClkPin);
            int nAsyncNet;
            if ( nAsyncPin < 0 || nClkPin < 0 ) continue;
            nAsyncNet = pInst->pNets[nAsyncPin];
            if ( nAsyncNet == MSTA_NO_ID || pInst->pNets[nClkPin] == MSTA_NO_ID ) continue;
            if ( pAsync->RecoveryArc != MSTA_NO_ID || pAsync->RecoveryFallArc != MSTA_NO_ID )
            {
                pNew = MstaCheckArrayAppend(&p->vChecks);
                pNew->InstId = (MstaId)i; pNew->nAsyncCheck = k; pNew->nEndNet = nAsyncNet;
                pNew->fToRegister = 1; pNew->fAsync = 1; pNew->fRecovery = 1;
            }
            if ( pAsync->RemovalArc != MSTA_NO_ID || pAsync->RemovalFallArc != MSTA_NO_ID )
            {
                pNew = MstaCheckArrayAppend(&p->vChecks);
                pNew->InstId = (MstaId)i; pNew->nAsyncCheck = k; pNew->nEndNet = nAsyncNet;
                pNew->fToRegister = 1; pNew->fAsync = 1; pNew->fRemoval = 1;
            }
        }
    }

    /* 顶层输出也是端点：reg2out，用 set_output_delay 卡。 */
    for ( i = 0; i < pDes->vNets.nSize; i++ )
    {
        MstaNet *pNet = MstaNetArrayAt( &pDes->vNets, i );
        MstaCheck *pNew;
        if ( !pNet->fTopPort || pNet->Dir != MSTA_PORT_OUT || pNet->fConst )
            continue;
        pNew = MstaCheckArrayAppend( &p->vChecks );
        pNew->InstId      = MSTA_NO_ID;
        pNew->nEndNet     = i;
        pNew->fToRegister = 0;
    }
    p->nEndpoints = p->vChecks.nSize;
}

/* =====================================================================
   2. 时钟树
   ===================================================================== */

static void MarkCombFanout( MstaTiming *p, char *pfMark, int *pQueue, int nQueue,
                            int fStopAtIdealNoProp )
{
    MstaDesign *pDes = p->pDes;
    int q;
    for ( q = 0; q < nQueue; q++ )
    {
        MstaNet *pNet = MstaNetArrayAt( &pDes->vNets, pQueue[q] );
        int i;
        if ( fStopAtIdealNoProp && Msta_SdcIdealNoPropagate( p->pSdc, pQueue[q] ) )
            continue;
        for ( i = 0; i < pNet->vLoads.nSize; i++ )
        {
            MstaPinRef *pRef = MstaPinRefArrayAt( &pNet->vLoads, i );
            MstaInst *pLoad = InstAt( p, pRef->InstId );
            MstaCell *pCell = pLoad->pCell;
            MstaPin *pInPin = MstaPinArrayAt( &pCell->vPins, pRef->PinId );
            int k;
            if ( pCell->fSequential )
                continue;
            for ( k = 0; k < pLoad->nPins && k < pCell->vPins.nSize; k++ )
            {
                MstaPin *pOutPin = MstaPinArrayAt( &pCell->vPins, k );
                int nOut = pLoad->pNets[k];
                if ( pOutPin->Dir != MSTA_DIR_OUTPUT || nOut < 0 )
                    continue;
                if ( Msta_CellCombArc( pCell, pInPin->Name, pOutPin->Name ) == NULL )
                    continue;
                if ( Msta_SdcTimingDisabled(p->pSdc,pRef->InstId,pInPin->Name,pOutPin->Name) )
                    continue;
                if ( !pfMark[nOut] )
                {
                    pfMark[nOut] = 1;
                    pQueue[nQueue++] = nOut;
                }
            }
        }
    }
}

/* 正向标记：从 create_clock 的源网络出发，沿组合时序弧向输出传播。
   只沿真实 arc 走，所以 ICG 的 enable/scan 侧不会被误标成时钟。 */
static void MarkClockNets( MstaTiming *p )
{
    MstaDesign *pDes = p->pDes;
    int nNets = pDes->vNets.nSize;
    int *pQueue = (int *)malloc( (size_t)(nNets > 0 ? nNets : 1) * sizeof(int) );
    int nQueue = 0, c;
    assert( pQueue );

    for ( c = 0; c < Msta_SdcClockCount(p->pSdc); c++ )
    {
        int nNet = Msta_SdcClockByIndex( p->pSdc, c )->SourceNet;
        if ( nNet >= 0 && !p->pfClockNet[nNet] )
        {
            p->pfClockNet[nNet] = 1;
            pQueue[nQueue++] = nNet;
        }
    }

    MarkCombFanout( p, p->pfClockNet, pQueue, nQueue, 0 );
    free( pQueue );
}

static int ClockArcSense( MstaTiming *p, MstaInst *pInst, const MstaArc *pArc )
{
    int i, Known = 0;
    signed char *Cases = (signed char *)malloc((size_t)pInst->pCell->vPins.nSize);
    assert(Cases);
    for ( i = 0; i < pInst->pCell->vPins.nSize; i++ )
    {
        int n = i < pInst->nPins ? pInst->pNets[i] : -1;
        MstaNet *pNet = n >= 0 ? MstaNetArrayAt(&p->pDes->vNets,n) : NULL;
        int Value = pNet ? (pNet->fCaseValue ? pNet->fCaseValue : pNet->fConst) : 0;
        Cases[i] = Value == 1 || Value == 2 ? (signed char)(Value - 1) : -1;
        if ( Cases[i] >= 0 ) Known = 1;
    }
    int Sense = Known ? Msta_LibClockSense(pInst->pCell,pArc,Cases) : pArc->Sense;
    free(Cases);
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
    ClockArrAt( p, nClock, nNet )->Polarity = Polarity;
    p->pfClockOfNet[ (size_t)nClock * (size_t)p->pDes->vNets.nSize + (size_t)nNet ] = 1;
}

/* 这个时钟在这里停下吗（set_clock_sense -stop_propagation）。 */
static int ClockStopsAt( MstaTiming *p, int nClock, int nNet )
{
    int fStop = 0;
    (void)Msta_SdcClockSense( p->pSdc, nNet, nClock, &fStop );
    return fStop || ClockArrAt(p,nClock,nNet)->fSourceOnly;
}

/* 每个时钟的时钟树里有哪些网络：从 create_clock 的源网络正向铺开，同时沿
   组合弧的 timing_sense 累积时钟极性（反相器把极性取反）。
   一根网络可以同时属于多个时钟（create_clock -add）。 */
static void AttachClockIds( MstaTiming *p )
{
    MstaDesign *pDes = p->pDes;
    int n, c, nNets = pDes->vNets.nSize;
    int *pQueue = (int *)malloc( (size_t)(2 * nNets) * sizeof(int) );
    assert( pQueue );

    memset( p->pfClockOfNet, 0, (size_t)nNets * (size_t)Msta_SdcClockCount(p->pSdc) );
    for ( c = 0; c < Msta_SdcClockCount(p->pSdc); c++ )
    {
        MstaClock *pChild = Msta_SdcClockByIndex(p->pSdc,c);
        int nMaster = Msta_SdcClockIndexOf(p->pSdc,pChild->MasterClock);
        if ( nMaster >= 0 && pChild->SourceNet >= 0 )
            ClockArrAt(p,nMaster,pChild->SourceNet)->fSourceOnly = 1;
    }

    for ( c = 0; c < Msta_SdcClockCount( p->pSdc ); c++ )
    {
        MstaClock *pClock = Msta_SdcClockByIndex( p->pSdc, c );
        int nQueue = 0;
        if ( pClock->SourceNet < 0 )
            continue;                       /* 虚拟时钟没有源网络 */
        MarkClockNetOf( p, c, pClock->SourceNet, 1 );
        pQueue[nQueue++] = pClock->SourceNet;
        for ( n = 0; n < nQueue; n++ )
        {
            MstaNet *pNet = MstaNetArrayAt( &pDes->vNets, pQueue[n] );
            int i;
            if ( ClockStopsAt( p, c, pQueue[n] ) )
                continue;                   /* set_clock_sense -stop_propagation */
            for ( i = 0; i < pNet->vLoads.nSize; i++ )
            {
                MstaPinRef *pRef = MstaPinRefArrayAt( &pNet->vLoads, i );
                MstaInst *pLoad = InstAt( p, pRef->InstId );
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
                    int nOut = pLoad->pNets[m];
                    int Polarity;
                    if ( pPin->Dir != MSTA_DIR_OUTPUT || nOut < 0 )
                        continue;
                    if ( !p->pfClockNet[nOut] )
                        continue;
                    pArc = Msta_CellCombArc( pCell, pInPin->Name, pPin->Name );
                    if ( pArc == NULL )
                        continue;
                    if ( Msta_SdcTimingDisabled( p->pSdc, pRef->InstId,
                                                 pInPin->Name, pPin->Name ) )
                        continue;
                    int Sense = ClockArcSense(p,pLoad,pArc);
                    if ( Sense < 0 ) continue;
                    Polarity = ClockArrAt( p, c, pQueue[n] )->Polarity;
                    if ( Sense == MSTA_SENSE_NEGATIVE )
                        Polarity = -Polarity;
                    else if ( Sense == MSTA_SENSE_NONUNATE )
                        Polarity = 0;
                    else if ( Sense == MSTA_SENSE_UNKNOWN )
                        Msta_WarnOnce( "clock path through cell \"%s\" has no timing_sense; "
                                       "clock polarity is assumed non-inverting",
                                       Msta_NameStr(pCell->Name) );
                    if ( !p->pfClockOfNet[ (size_t)c * (size_t)nNets + (size_t)nOut ] )
                    {
                        MarkClockNetOf( p, c, nOut, Polarity );
                        pQueue[nQueue++] = nOut;
                    }
                    else
                    {
                        int fStop = 0, Old = ClockArrAt(p,c,nOut)->Polarity;
                        int Force = Msta_SdcClockSense(p->pSdc,nOut,c,&fStop);
                        int Joined = Force ? Force : (Old == Polarity ? Old : 0);
                        if ( Old != Joined )
                        {
                            MarkClockNetOf(p,c,nOut,Joined);
                            pQueue[nQueue++] = nOut;
                        }
                    }
                }
            }
        }
    }
    free( pQueue );
}

/* =====================================================================
   3. 理想网络（set_ideal_network）
   ---------------------------------------------------------------------
   标成理想网络的网络在时序上不累计延迟：它的到达/摆率等于驱动它的组合
   单元输入脚上的那两份，弧延迟按 0 记，时钟树和普通数据路径一个算法。
   -no_propagate 的网络只标自己，理想属性不再往扇出传；不写这个选项时
   属性沿组合扇出一直传下去（FF 的输出是边界，不再往下传），所以把一个
   时钟端口标成理想网络就等于"整棵时钟树都不算插入延迟"。
   set_ideal_latency / set_ideal_transition 是理想网络上的两个显式值：
   给了就把该网络的到达时间/摆率直接换成这两个数，不再看传播结果。
   ===================================================================== */

/* 从 set_ideal_network 标过的网络出发，把理想属性沿组合扇出铺开。 */
static void MarkIdealNets( MstaTiming *p )
{
    MstaDesign *pDes = p->pDes;
    int nNets = pDes->vNets.nSize;
    int *pQueue = (int *)malloc( (size_t)(nNets > 0 ? nNets : 1) * sizeof(int) );
    int nQueue = 0, n;
    assert( pQueue );

    for ( n = 0; n < nNets; n++ )
    {
        p->pfIdealNet[n] = 0;
        if ( Msta_SdcIsIdealNet( p->pSdc, n ) )
        {
            p->pfIdealNet[n] = 1;
            pQueue[nQueue++] = n;
        }
    }

    MarkCombFanout( p, p->pfIdealNet, pQueue, nQueue, 1 );
    free( pQueue );
}

/* 理想网络上的显式延迟/摆率（set_ideal_latency / set_ideal_transition）：
   给了就把该网络上的结果直接换成它，不再看传播算出多少。 */
static void ApplyIdealClockValues( MstaTiming *p, int nNet, MstaClockArr *pArr )
{
    int m, e, s;
    for ( m = 0; m < 2; m++ )
        for ( e = 0; e < 2; e++ )
        {
            double v = Msta_SdcIdealLatency(p->pSdc,nNet,m,e);
            if ( Msta_IsSet(v) )
                for ( s = 0; s < 2; s++ )
                    if ( ClockTagValid(pArr,m,e,s) ) pArr->Arrival[m][e][s] = v;
            v = Msta_SdcIdealTransition(p->pSdc,nNet,m,e);
            if ( Msta_IsSet(v) )
                for ( s = 0; s < 2; s++ )
                    if ( ClockTagValid(pArr,m,e,s) ) pArr->Slew[m][e][s] = v;
        }
    MergeClockSummary(pArr);
}

/* =====================================================================
   5. 时钟传播（第 4 步拓扑序之后才调用：要按拓扑序走）
   ===================================================================== */

/* 生成时钟继承主时钟到源引脚的状态；时序输出还要加入对应 clk-to-Q。 */
static void SeedGeneratedClock( MstaTiming *p, int nClock, MstaClock *pClock )
{
    MstaNet *pNet;
    MstaInst *pDriver;
    MstaCell *pCell;
    MstaPin *pOut;
    int i, nMaster;
    if ( pClock->MasterClock == MSTA_NO_ID || pClock->SourceNet < 0 ) return;
    pNet = MstaNetArrayAt(&p->pDes->vNets,pClock->SourceNet);
    if ( pNet->Driver.InstId == MSTA_NO_ID ) return;
    pDriver = InstAt(p,pNet->Driver.InstId);
    pCell = pDriver->pCell;
    nMaster = Msta_SdcClockIndexOf(p->pSdc,pClock->MasterClock);
    if ( nMaster < 0 ) return;
    if ( !pCell->fSequential )
    {
        /* 生成时钟从输出引脚重新起算，先继承主时钟到该引脚的插入延迟和 slew。 */
        MstaClockArr *pSrc = ClockArrAt(p,nMaster,pClock->SourceNet);
        MstaClockArr *pDst = ClockArrAt(p,nClock,pClock->SourceNet);
        int m, e;
        if ( !NetHasClock(p,nMaster,pClock->SourceNet) || !pSrc->fReached )
        {
            pDst->fReached = 0;
            Msta_WarnOnce("generated clock \"%s\": master clock does not reach its source pin",
                          Msta_NameStr(pClock->Name));
            return;
        }
        for ( m = 0; m < 2; m++ )
            for ( e = 0; e < 2; e++ )
            {
                int Label = EffectiveClkRises(p,nClock,pClock->SourceNet,e);
                pDst->Tags[e] |= 1 << Label;
                pDst->Arrival[m][e][Label] = ClockArrival(pSrc,m,e,-1) + pClock->SourceLatency[m][Label];
                if ( pClock->fPropagated ) pDst->Slew[m][e][Label] = ClockSlew(pSrc,m,e,-1);
            }
        pDst->fReached = 1;
        pDst->nThroughGates = pSrc->nThroughGates;
        ApplyIdealClockValues(p,pClock->SourceNet,pDst);
        return;
    }
    pOut = MstaPinArrayAt(&pCell->vPins,pNet->Driver.PinId);
    for ( i = 0; i < pCell->vRegs.nSize; i++ )
    {
        MstaRegCheck *pReg = MstaRegCheckArrayAt(&pCell->vRegs,i);
        int nClkNet = InstPinNet(p,pNet->Driver.InstId,pReg->ClkPin);
        MstaClockArr *pDst, *pSrc;
        MstaArc *pArcMax, *pArcMin;
        double LoadMax, LoadMin;
        if ( nClkNet < 0 ||
             !NetHasClock(p,nMaster,nClkNet) ||
             !ClockArrAt(p,nMaster,nClkNet)->fReached ) continue;
        int fClkRises;
        pArcMax = RegClkToQArc(p,pCell,pReg,pOut->Name,&fClkRises,1,pNet->Driver.InstId);
        pArcMin = RegClkToQArc(p,pCell,pReg,pOut->Name,&fClkRises,0,pNet->Driver.InstId);
        if ( pArcMax == NULL || pArcMin == NULL ) continue;
        LoadMax = NetLoad(p,pClock->SourceNet,1);
        LoadMin = NetLoad(p,pClock->SourceNet,0);
        /* 生成时钟的源延迟按"主时钟在这根时钟网络上的到达 + clk-to-Q"算，
           结果记在生成时钟自己那一行。 */
        pDst = ClockArrAt(p,nClock,pClock->SourceNet);
        pSrc = ClockArrAt(p,nMaster,nClkNet);
        pDst->fReached = 1;
        int m, e;
        for ( m = 0; m < 2; m++ )
            for ( e = 0; e < 2; e++ )
            {
                MstaArc *pArc = m ? pArcMax : pArcMin;
                int fClkRise = ArcClockRises(pArc);
                double Slew = ClockSlew(pSrc,m,fClkRise,-1);
                int Label = EffectiveClkRises(p,nClock,pClock->SourceNet,e);
                pDst->Tags[e] |= 1 << Label;
                pDst->Arrival[m][e][Label] = ClockArrival(pSrc,m,fClkRise,-1)
                    + ArcDelayEdge(pArc,Slew,m ? LoadMax : LoadMin,e)
                      * Msta_SdcClockDerateClock(p->pSdc,nMaster,m)
                    + pClock->SourceLatency[m][Label];
                if ( pClock->fPropagated )
                    pDst->Slew[m][e][Label] = ArcSlewEdge(pArc,Slew,m ? LoadMax : LoadMin,e);
            }
        ApplyIdealClockValues(p,pClock->SourceNet,pDst);
        pDst->nThroughGates = pSrc->nThroughGates + 1;
        return;
    }
    /* 无有效主时钟路径时保留显式生成时钟定义；不凭空使用被禁用的物理弧。 */
    MstaClockArr *pDst = ClockArrAt(p,nClock,pClock->SourceNet);
    if ( pClock->fPropagated )
    {
        int m, e;
        for ( m = 0; m < 2; m++ )
            for ( e = 0; e < 2; e++ )
                pDst->Slew[m][e][EffectiveClkRises(p,nClock,pClock->SourceNet,e)] = 0.0;
        ApplyIdealClockValues(p,pClock->SourceNet,pDst);
    }
    Msta_WarnOnce("generated clock \"%s\": cannot derive sequential source latency; "
                  "using annotated source latency and zero propagated slew",
                  Msta_NameStr(pClock->Name));
}

/* 对每个时钟求它在各时钟网络上逐边沿（[角][本地沿][源沿]）的到达与摆率：
   先给源网络（理想时钟则是整棵树）设初值 = 源延迟和源 slew；理想时钟随后把
   network latency 铺到整棵树，传播时钟按拓扑序沿组合弧逐边沿累加弧延迟。
   生成时钟的源网络由 SeedGeneratedClock 从主时钟接上。 */
static void PropagateClocks( MstaTiming *p )
{
    MstaDesign *pDes = p->pDes;
    int nClocks = Msta_SdcClockCount(p->pSdc), n, c, m, e, s, q;
    for ( c = 0; c < nClocks; c++ )
    {
        MstaClock *pClock = Msta_SdcClockByIndex(p->pSdc,c);
        MstaNetCons *pCons = pClock->SourceNet >= 0
                          ? Msta_SdcNetCons(p->pSdc,pClock->SourceNet) : NULL;
        for ( n = 0; n < pDes->vNets.nSize; n++ )
        {
            MstaClockArr *pArr = ClockArrAt(p,c,n);
            pArr->fReached = pArr->nThroughGates = 0;
            pArr->Tags[0] = pArr->Tags[1] = 0;
            for ( m = 0; m < 2; m++ )
                for ( e = 0; e < 2; e++ )
                    for ( s = 0; s < 2; s++ )
                    {
                        double InputSlew = pCons ? (m ? (s ? pCons->InputSlewMaxRise : pCons->InputSlewMaxFall)
                                                                   : (s ? pCons->InputSlewMinRise : pCons->InputSlewMinFall)) : MSTA_UNSET;
                        /* 时钟源 slew 按 SDC 语义取值：理想时钟只看
                           set_clock_transition，写了 set_propagated_clock 的只看源端口的
                           set_input_transition，没给就是 0。没写 set_propagated_clock 的时钟 miniSTA
                           也做传播，而 SDC 里它本应是理想时钟，所以两种约束都可能是用户想要的源 slew：
                           先取输入 slew，再取时钟 slew，都没给才用 MSTA_DEFAULT_SLEW（不是 0）。 */
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
                        if ( NetHasClock(p,c,n) && (n == pClock->SourceNet || !pClock->fPropagated)
                             && (pArr->Polarity == 0 || s == EffectiveClkRises(p,c,n,e)) )
                        {
                            pArr->fReached = 1;
                            pArr->Tags[e] |= 1 << s;
                            pArr->Arrival[m][e][s] = pClock->SourceLatency[m][s];
                        }
                    }
            ApplyIdealClockValues(p,n,pArr);
        }
    }
    for ( c = 0; c < nClocks; c++ )
    {
        MstaClock *pClock = Msta_SdcClockByIndex(p->pSdc,c);
        SeedGeneratedClock(p,c,pClock);
        if ( !pClock->fPropagated )
        {
            /* 理想时钟（如给了 set_clock_latency 网络延迟）不累计单元延迟：树上每点 =
               源网络的到达 + network latency（理想网络上不加）。 */
            if ( pClock->SourceNet >= 0 )
            {
                MstaClockArr Source = *ClockArrAt(p,c,pClock->SourceNet);
                for ( n = 0; n < pDes->vNets.nSize; n++ )
                    if ( NetHasClock(p,c,n) )
                    {
                        MstaClockArr *pArr = ClockArrAt(p,c,n);
                        for ( m = 0; m < 2; m++ )
                            for ( e = 0; e < 2; e++ )
                                for ( s = 0; s < 2; s++ )
                                    if ( ClockTagValid(pArr,m,e,s) )
                                    {
                                        pArr->Arrival[m][e][s] = ClockArrival(&Source,m,s,s);
                                        if ( !p->pfIdealNet[n] ) pArr->Arrival[m][e][s] += pClock->NetworkLatency[m][s];
                                    }
                        ApplyIdealClockValues(p,n,pArr);
                    }
            }
            continue;
        }
        for ( q = 0; q < p->nTopoOrder; q++ )
        {
            int nNet = p->pTopoOrder[q], k, i, fAny = 0, nGates = 0;
            MstaNet *pNet = MstaNetArrayAt(&pDes->vNets,nNet);
            MstaClockArr *pArr = ClockArrAt(p,c,nNet);
            MstaInst *pDriver;
            MstaCell *pCell;
            if ( !NetHasClock(p,c,nNet) || nNet == pClock->SourceNet || pNet->fCaseValue || pNet->Driver.InstId < 0 ) continue;
            pDriver = InstAt(p,pNet->Driver.InstId);
            pCell = pDriver->pCell;
            if ( pCell->fSequential ) continue;
            for ( k = 0; k < pDriver->nPins && k < pCell->vPins.nSize; k++ )
            {
                MstaPin *pPin = MstaPinArrayAt(&pCell->vPins,k);
                int nIn = pDriver->pNets[k], Stop = 0;
                int Force = Msta_SdcClockSense(p->pSdc,nNet,c,&Stop);
                MstaClockArr *pInArr;
                if ( pPin->Dir == MSTA_DIR_OUTPUT || nIn < 0 || !NetHasClock(p,c,nIn) || !ClockArrAt(p,c,nIn)->fReached ) continue;
                pInArr = ClockArrAt(p,c,nIn);
                if ( Msta_SdcTimingDisabled(p->pSdc,pNet->Driver.InstId,pPin->Name,MstaPinArrayAt(&pCell->vPins,pNet->Driver.PinId)->Name) ) continue;
                for ( m = 0; m < 2; m++ )
                {
                    MstaArc *pArc = TimingCombArc(p,pCell,pPin->Name,MstaPinArrayAt(&pCell->vPins,pNet->Driver.PinId)->Name,m);
                    double Load = NetLoad(p,nNet,m);
                    if ( pArc == NULL ) continue;
                    int Sense = ClockArcSense(p,pDriver,pArc);
                    if ( Sense < 0 ) continue;
                    for ( e = 0; e < 2; e++ )
                        for ( i = 0; i < 2; i++ )
                        {
                            if ( Sense != MSTA_SENSE_NONUNATE && i != (Sense == MSTA_SENSE_NEGATIVE ? !e : e) ) continue;
                            for ( s = 0; s < 2; s++ )
                            {
                                /* 源沿标签通常沿用输入的 s；若 set_clock_sense 在这根网络上强制了
                                   极性，则按强制极性从本地沿 e 反推（positive 同沿，negative 反沿）。 */
                                int Label = Force ? (Force > 0 ? e : !e) : s;
                                double Cand, Slew;
                                if ( !ClockTagValid(pInArr,m,i,s) ) continue;
                                Cand = ClockArrival(pInArr,m,i,s);
                                Slew = ClockSlew(pInArr,m,i,s);
                                if ( !p->pfIdealNet[nNet] )
                                {
                                    Cand += ArcDelayEdge(pArc,Slew,Load,e) * Msta_SdcClockDerateClock(p->pSdc,c,m);
                                    Slew = ArcSlewEdge(pArc,Slew,Load,e);
                                }
                                if ( !ClockTagValid(pArr,m,e,Label) || (m ? Cand > pArr->Arrival[m][e][Label] : Cand < pArr->Arrival[m][e][Label]) )
                                {
                                    pArr->Arrival[m][e][Label] = Cand;
                                    pArr->Slew[m][e][Label] = Slew;
                                }
                                pArr->Tags[e] |= 1 << Label;
                                fAny = 1;
                            }
                        }
                    if ( pInArr->nThroughGates + 1 > nGates ) nGates = pInArr->nThroughGates + 1;
                }
            }
            if ( fAny )
            {
                pArr->fReached = 1;
                pArr->nThroughGates = nGates;
                ApplyIdealClockValues(p,nNet,pArr);
            }
        }
    }
}

/* =====================================================================
   4. 拓扑序
   ===================================================================== */

/* 网络 n 的"前件"= 驱动它的组合单元的所有输入网络。
   时序单元的输出是路径边界，所以它的前件数是 0。 */
static int NetInputCount( MstaTiming *p, int nNet )
{
    MstaNet *pNet = MstaNetArrayAt( &p->pDes->vNets, nNet );
    MstaCell *pCell;
    int n = 0;
    if ( pNet->Driver.InstId == MSTA_NO_ID )
        return 0;
    pCell = InstAt( p, pNet->Driver.InstId )->pCell;
    if ( pCell->fSequential )
        return 0;
    {
        MstaInst *pDriver = InstAt( p, pNet->Driver.InstId );
        int k;
        for ( k = 0; k < pDriver->nPins && k < pCell->vPins.nSize; k++ )
        {
            MstaPin *pPin = MstaPinArrayAt( &pCell->vPins, k );
            if ( pPin->Dir != MSTA_DIR_OUTPUT && pDriver->pNets[k] >= 0 )
                n++;
        }
    }
    return n;
}

static int NetInputAt( MstaTiming *p, int nNet, int nWhich )
{
    MstaNet *pNet = MstaNetArrayAt( &p->pDes->vNets, nNet );
    MstaInst *pDriver = InstAt( p, pNet->Driver.InstId );
    MstaCell *pCell = pDriver->pCell;
    int k, n = 0;
    for ( k = 0; k < pDriver->nPins && k < pCell->vPins.nSize; k++ )
    {
        MstaPin *pPin = MstaPinArrayAt( &pCell->vPins, k );
        if ( pPin->Dir == MSTA_DIR_OUTPUT || pDriver->pNets[k] < 0 )
            continue;
        if ( n == nWhich )
            return pDriver->pNets[k];
        n++;
    }
    return -1;
}

/* 显式栈的迭代 DFS：递归实现在十万级长链上会爆栈。
   状态 0=未访问, 1=在当前栈上, 2=已完成后序输出。 */
static void BuildTopoOrder( MstaTiming *p )
{
    MstaDesign *pDes = p->pDes;
    int nNets = pDes->vNets.nSize;
    int *pStackNet = (int *)malloc( (size_t)(nNets + 1) * sizeof(int) );
    int *pStackIdx = (int *)malloc( (size_t)(nNets + 1) * sizeof(int) );
    int nTopo = 0, start;
    assert( pStackNet && pStackIdx );

    for ( start = 0; start < nNets; start++ )
    {
        int nSp = 0;
        if ( p->pState[start] != 0 )
            continue;
        p->pState[start] = 1;
        pStackNet[0] = start;
        pStackIdx[0] = 0;
        while ( nSp >= 0 )
        {
            int nNet = pStackNet[nSp];
            int nInputs = NetInputCount( p, nNet );
            int fDescended = 0;

            while ( pStackIdx[nSp] < nInputs )
            {
                int nIn = NetInputAt( p, nNet, pStackIdx[nSp]++ );
                if ( nIn < 0 )
                    continue;
                if ( p->pState[nIn] == 0 )
                {
                    p->pState[nIn] = 1;
                    nSp++;
                    pStackNet[nSp] = nIn;
                    pStackIdx[nSp] = 0;
                    fDescended = 1;
                    break;
                }
                if ( p->pState[nIn] == 1 )
                {
                    p->nCombLoops++;
                    Msta_WarnOnce( "combinational loop found at net \"%s\" (analysis breaks it here)",
                                   Msta_NetName( pDes, nNet ) );
                }
            }
            if ( fDescended )
                continue;
            p->pState[nNet] = 2;
            p->pTopoOrder[nTopo++] = nNet;
            nSp--;
        }
    }
    free( pStackNet );
    free( pStackIdx );
    p->nTopoOrder = nTopo;
}

/* =====================================================================
   6a. 数据到达（前向传播）
   ===================================================================== */

/* 判断某个边沿的到达时间是否已经算出来。 */
static int ArrivalSet( double Arrival, int fMax )
{
    return fMax ? Arrival > -MSTA_NO_TIME / 2.0 : Arrival < MSTA_NO_TIME / 2.0;
}

/* timing_sense 决定哪种输入边沿能产生给定的输出边沿：
   positive_unate 同向，negative_unate 反向，其余返回 -1，表示两个输入边沿都要参与。 */
static int CoupledInputEdge( MstaSense Sense, int fOutRise )
{
    if ( Sense == MSTA_SENSE_POSITIVE )
        return fOutRise;
    if ( Sense == MSTA_SENSE_NEGATIVE )
        return !fOutRise;
    return -1;
}

/* 一个角的数据前向传播：按拓扑序求每根网络的升/降沿到达、摆率、前驱和起点信息。
   只传由 nOnlyClock 出发的数据；nOnlyStartClass >= 0 时只放这一类起点（-1 = 全部）；
   起点用到的时钟源沿还须等于 p->LaunchClockTag（-1 = 不限），否则该起点不出发。
   起点那拍：FF 的 Q 取时钟沿 + 时钟脚到达，输入端口取输入延迟参照的时钟沿（含时钟
   延迟），组合传递原样继承前驱；检查时据此决定 setup/hold 落在哪一拍。 */
static void PropagateData( MstaTiming *p, int fMax, int nOnlyClock, int nOnlyStartClass )
{
    MstaDesign *pDes = p->pDes;
    MstaCorner *pCorner = CornerOf( p, fMax );
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
    int q;

    for ( q = 0; q < p->nTopoOrder; q++ )
    {
        int nNet = p->pTopoOrder[q];
        MstaNet *pNet = MstaNetArrayAt( &pDes->vNets, nNet );
        double BestR = Unset, BestF = Unset;
        double SlewR = MSTA_DEFAULT_SLEW, SlewF = MSTA_DEFAULT_SLEW;
        int nPrevR = -1, nPrevF = -1;
        int nEdgeR = -1, nEdgeF = -1;
        int nDriverInst = pNet->Driver.InstId;

        pArrR[nNet] = pArrF[nNet] = Unset;
        pSlewR[nNet] = pSlewF[nNet] = MSTA_DEFAULT_SLEW;
        pPrevNet[nNet] = -1;
        pPrevR[nNet].Net = pPrevR[nNet].Edge = -1;
        pPrevF[nNet].Net = pPrevF[nNet].Edge = -1;
        pSrcClock[nNet] = -1;
        pSrcEdge[nNet] = 0.0;
        pSrcRises[nNet] = 1;

        if ( !pNet->fCaseValue && !p->pfClockNet[nNet] )
        {
            if ( nDriverInst == MSTA_NO_ID )
            {
                /* 情况 A：没有驱动 -> 顶层输入或悬空。两个边沿同时到达。 */
                if ( nOnlyStartClass < 0 || p->pStartClass[nNet] == nOnlyStartClass )
                {
                    MstaNetCons *pCons = Msta_SdcNetCons( p->pSdc, nNet );
                    double Delay = Msta_SdcPortDelay( p->pSdc, nNet, nOnlyClock, 0, fMax );
                    if ( Msta_IsSet( Delay ) )
                    {
                        MstaClock *pClock = Msta_SdcClockByIndex( p->pSdc, nOnlyClock );
                        /* 输入延迟参照的时钟边沿：-clock_fall 时是下降沿。 */
                        int fClkFall = Msta_SdcPortClockFall( p->pSdc, nNet, nOnlyClock, 0, fMax );
                        int nRefNet = Msta_SdcPortReferenceNet(p->pSdc,nNet,nOnlyClock,0,fMax);
                        int fSourceRise = !fClkFall;
                        double Edge;
                        if ( nRefNet >= 0 )
                        {
                            MstaClockArr *pRefArr = ClockArrAt(p,nOnlyClock,nRefNet);
                            if ( !pRefArr->fReached )
                                continue;
                            /* -clock_fall 指的是参考引脚上的边沿，要沿传播后的时钟树
                               把中间的反相折算回源头。 */
                            fSourceRise = ClockTagSelect(pRefArr,fMax,!fClkFall,p->LaunchClockTag);
                            if ( fSourceRise < 0 ) continue;
                            fClkFall = !fSourceRise;
                            Edge = (fSourceRise ? pClock->RiseEdge : pClock->FallEdge)
                                 + ClockArrival(pRefArr,fMax,!Msta_SdcPortClockFall(p->pSdc,nNet,nOnlyClock,0,fMax),fSourceRise);
                        }
                        else
                        {
                            int fSourceIncluded = Msta_SdcPortSourceLatencyIncluded(
                                p->pSdc,nNet,nOnlyClock,0,fMax);
                            int fNetworkIncluded = Msta_SdcPortNetworkLatencyIncluded(
                                p->pSdc,nNet,nOnlyClock,0,fMax);
                            if ( p->LaunchClockTag >= 0 && p->LaunchClockTag != fSourceRise ) continue;
                            Edge = fClkFall ? pClock->FallEdge : pClock->RiseEdge;
                            if ( !fSourceIncluded )
                                Edge += ClockSourceArrival(p,nOnlyClock,fMax,fSourceRise);
                            if ( !pClock->fPropagated && !fNetworkIncluded )
                                Edge += pClock->NetworkLatency[fMax][fSourceRise];
                        }
                        double SlewInR = fMax ? pCons->InputSlewMaxRise : pCons->InputSlewMinRise;
                        double SlewInF = fMax ? pCons->InputSlewMaxFall : pCons->InputSlewMinFall;
                        BestR = BestF = Edge + Delay;
                        SlewR = Msta_IsSet(SlewInR) ? SlewInR : MSTA_DEFAULT_SLEW;
                        SlewF = Msta_IsSet(SlewInF) ? SlewInF : MSTA_DEFAULT_SLEW;
                        /* set_driving_cell：把外部驱动单元当成一级，
                           按端口实际负载查它的延迟/摆率；延迟加到端口到达上。 */
                        if ( pCons->DrivingCell != MSTA_NO_ID )
                        {
                            const char *pDriveName = Msta_NameStr(pCons->DrivingCell);
                            MstaCell *pDrive = Msta_LibFindCell(p->pLib,pDriveName);
                            if ( pDrive != NULL && strchr(pDriveName,'/') == NULL )
                                pDrive = TimingCellCorner(p,pDrive,fMax);
                            MstaArc *pArc = ( pDrive != NULL )
                                          ? DriveCellArc( pDrive, pCons ) : NULL;
                            if ( pArc != NULL )
                            {
                                double Load = NetLoad( p, nNet, fMax );
                                double InR = Msta_IsSet(pCons->DriveInSlewRise)
                                           ? pCons->DriveInSlewRise : SlewR;
                                double InF = Msta_IsSet(pCons->DriveInSlewFall)
                                           ? pCons->DriveInSlewFall : SlewF;
                                BestR += ArcDelayEdge( pArc, InR, Load, 1 );
                                BestF += ArcDelayEdge( pArc, InF, Load, 0 );
                                SlewR = ArcSlewEdge( pArc, InR, Load, 1 )
                                      * pCons->DriveMultiply;
                                SlewF = ArcSlewEdge( pArc, InF, Load, 0 )
                                      * pCons->DriveMultiply;
                            }
                        }
                        pSrcClock[nNet] = nOnlyClock;
                        pSrcEdge[nNet]  = Edge;
                        pSrcRises[nNet] = (char)( fClkFall ? 0 : 1 );
                    }
                }
            }
            else
            {
                MstaInst *pDriver = InstAt( p, nDriverInst );
                MstaCell *pCell = pDriver->pCell;
                MstaPin *pOutPin = MstaPinArrayAt( &pCell->vPins, pNet->Driver.PinId );
                double Load = NetLoad( p, nNet, fMax );

                if ( pCell->fSequential )
                {
                    /* 情况 B：FF 的输出是路径起点，两个输出边沿都从同一个时钟沿出发。 */
                    int k;
                    if ( nOnlyStartClass < 0 || p->pStartClass[nNet] == nOnlyStartClass )
                    {
                        for ( k = 0; k < pCell->vRegs.nSize; k++ )
                        {
                            MstaRegCheck *pReg = MstaRegCheckArrayAt( &pCell->vRegs, k );
                            MstaArc *pArc;
                            int nClkNet, nClock = nOnlyClock;
                            MstaClockArr *pClkArr;
                            int fClkRises = pReg->fClkRises;
                            double ClkSlew, Edge;
                            nClkNet = InstPinNet( p, nDriverInst, pReg->ClkPin );
                            if ( nClkNet < 0 )
                                continue;
                            pArc = RegClkToQArc( p, pCell, pReg, pOutPin->Name,
                                                 &fClkRises, fMax, nDriverInst );
                            if ( pArc == NULL || !NetHasDataClock(p,nOnlyClock,nClkNet) )
                                continue;
                            pClkArr = ClockArrAt( p, nOnlyClock, nClkNet );
                            if ( !pClkArr->fReached )
                                continue;
                            /* 库给的触发沿是 CLK 脚上的本地沿；按当前 launch 标签选出
                               是哪个时钟源边沿传到这里的，出发时刻按那个源边沿算。 */
                            int fLocalRise = fClkRises;
                            fClkRises = ClockTagSelect(pClkArr,fMax,fLocalRise,p->LaunchClockTag);
                            if ( fClkRises < 0 ) continue;
                            ClkSlew = ClockSlew(pClkArr,fMax,fLocalRise,fClkRises);
                            Edge = ClockArrival(pClkArr,fMax,fLocalRise,fClkRises);
                            if ( nClock >= 0 )
                            {
                                MstaClock *pClock = Msta_SdcClockByIndex( p->pSdc, nClock );
                                Edge += fClkRises ? pClock->RiseEdge : pClock->FallEdge;
                            }
                            if ( p->pfIdealNet[nNet] )
                            {
                                /* 理想网络：Q 脚上的到达就是时钟沿，不累加 clk-to-Q。 */
                                BestR = BestF = Edge;
                                SlewR = SlewF = ClkSlew;
                            }
                            else
                            {
                                BestR = Edge + ArcDelayEdge( pArc, ClkSlew, Load, 1 )
                                              * Msta_SdcDataDerateInst(p->pSdc,nDriverInst,fMax,1);
                                BestF = Edge + ArcDelayEdge( pArc, ClkSlew, Load, 0 )
                                              * Msta_SdcDataDerateInst(p->pSdc,nDriverInst,fMax,0);
                                SlewR = ArcSlewEdge( pArc, ClkSlew, Load, 1 );
                                SlewF = ArcSlewEdge( pArc, ClkSlew, Load, 0 );
                            }
                            pSrcEdge[nNet]  = Edge;
                            pSrcClock[nNet] = nClock;
                            pSrcRises[nNet] = (char)fClkRises;
                            break;              /* 一个输出脚只有一条 clk2q */
                        }
                    }
                }
                else
                {
                    /* 情况 C：组合单元。按 timing_sense 把输入边沿耦合到输出边沿。 */
                    int i, e;
                    for ( i = 0; i < pDriver->nPins && i < pCell->vPins.nSize; i++ )
                    {
                        MstaPin *pIn = MstaPinArrayAt( &pCell->vPins, i );
                        MstaArc *pArc;
                        MstaSense Sense;
                        int nInNet = pDriver->pNets[i];
                        if ( pIn->Dir == MSTA_DIR_OUTPUT || nInNet < 0 )
                            continue;
                        if ( p->pfClockNet[nInNet] )
                            continue;       /* 时钟脚由寄存器模型负责，不是数据弧 */
                        if ( pSrcClock[nInNet] != nOnlyClock )
                            continue;
                        pArc = TimingCombArc( p,pCell,pIn->Name,pOutPin->Name,fMax );
                        if ( Msta_SdcTimingDisabled( p->pSdc, nDriverInst, pIn->Name, pOutPin->Name ) )
                            continue;
                        if ( pArc == NULL && !pCell->fBlackBox )
                            Msta_WarnOnce( "cell \"%s\" has no timing arc from pin \"%s\" to \"%s\"",
                                           Msta_NameStr(pCell->Name), Msta_NameStr(pIn->Name),
                                           Msta_NameStr(pOutPin->Name) );
                        Sense = pArc ? pArc->Sense : MSTA_SENSE_UNKNOWN;

                        for ( e = 0; e < 2; e++ )
                        {
                            /* e = 1 是上升沿，累加到 BestR；e = 0 是下降沿，累加到 BestF。 */
                            double *pBestE = e ? &BestR : &BestF;
                            double *pSlewBest = e ? &SlewR : &SlewF;
                            int *pPrevBest = e ? &nPrevR : &nPrevF;
                            int *pEdgeBest = e ? &nEdgeR : &nEdgeF;
                            int nInEdge = CoupledInputEdge( Sense, e );
                            double InArr, InSlew, Cand, CandSlew;

                            if ( nInEdge < 0 )
                            {
                                /* 非单调（non_unate）或未知：对该分析角取最保守的输入边沿。 */
                                int fR = ArrivalSet( pArrR[nInNet], fMax );
                                int fF = ArrivalSet( pArrF[nInNet], fMax );
                                if ( !fR && !fF )
                                    continue;
                                if ( !fF )      nInEdge = 1;
                                else if ( !fR ) nInEdge = 0;
                                else            nInEdge = ( fMax ? ( pArrR[nInNet] > pArrF[nInNet] )
                                                                 : ( pArrR[nInNet] < pArrF[nInNet] ) ) ? 1 : 0;
                            }
                            else if ( !ArrivalSet( nInEdge ? pArrR[nInNet] : pArrF[nInNet], fMax ) )
                                continue;

                            InArr  = nInEdge ? pArrR[nInNet] : pArrF[nInNet];
                            InSlew = nInEdge ? pSlewR[nInNet] : pSlewF[nInNet];
                            if ( pArc == NULL || p->pfIdealNet[nNet] )
                            {
                                Cand = InArr;               /* 没有弧或理想网络：当 0 延迟 */
                                CandSlew = InSlew;
                            }
                            else
                            {
                                Cand = InArr + ArcDelayEdge( pArc, InSlew, Load, e )
                                              * Msta_SdcDataDerateInst(p->pSdc,nDriverInst,fMax,e);
                                CandSlew = ArcSlewEdge( pArc, InSlew, Load, e );
                            }
                            if ( fMax ? ( Cand > *pBestE ) : ( Cand < *pBestE ) )
                            {
                                *pBestE = Cand;
                                *pSlewBest = CandSlew;
                                *pPrevBest = nInNet;
                                *pEdgeBest = nInEdge;
                            }
                        }
                    }
                }
            }
        }

        /* ---- 合并两个边沿：取该分析角更差的那个，并同步前驱与起点信息 ---- */
        {
            int nUseRise;
            double Best;
            /* 理想网络上的显式值：到达/摆率直接换成 set_ideal_latency /
               set_ideal_transition 给的那个数。 */
            {
                double v;
                v = Msta_SdcIdealLatency( p->pSdc, nNet, fMax, 1 );
                if ( Msta_IsSet(v) ) BestR = v;
                v = Msta_SdcIdealLatency( p->pSdc, nNet, fMax, 0 );
                if ( Msta_IsSet(v) ) BestF = v;
                v = Msta_SdcIdealTransition( p->pSdc, nNet, fMax, 1 );
                if ( Msta_IsSet(v) ) SlewR = v;
                v = Msta_SdcIdealTransition( p->pSdc, nNet, fMax, 0 );
                if ( Msta_IsSet(v) ) SlewF = v;
            }
            /* 找次优路径用的排除：把这些边沿的到达当成不存在，DP 会换一条路。 */
            {
                int i;
                for ( i = 0; i < p->nPathExclude; i++ )
                    if ( p->vPathExclude[i].Net == nNet )
                    {
                        if ( p->vPathExclude[i].EdgeMask & 1 )
                        { BestR = Unset; SlewR = MSTA_DEFAULT_SLEW; }
                        if ( p->vPathExclude[i].EdgeMask & 2 )
                        { BestF = Unset; SlewF = MSTA_DEFAULT_SLEW; }
                    }
            }
            nUseRise = fMax ? ( BestR >= BestF ) : ( BestR <= BestF );
            Best = nUseRise ? BestR : BestF;
            pPrevR[nNet].Net  = nPrevR;
            pPrevR[nNet].Edge = nEdgeR;
            pPrevF[nNet].Net  = nPrevF;
            pPrevF[nNet].Edge = nEdgeF;
            /* 两个边沿的结果要留给下游查表用，未算出的保持 Unset。 */
            pArrR[nNet]  = BestR;
            pArrF[nNet]  = BestF;
            pSlewR[nNet] = SlewR;
            pSlewF[nNet] = SlewF;
            if ( ArrivalSet( Best, fMax ) )
            {
                pArrM[nNet]  = Best;
                pSlewM[nNet] = nUseRise ? SlewR : SlewF;
                pPrevNet[nNet] = nUseRise ? nPrevR : nPrevF;
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
}

/* =====================================================================
   6b. 检查：捕获沿、要求时间与 slack
   ===================================================================== */

/* 两个整数的最大公约数（跨时钟搜索的周期用整数 ps 算）。 */
static int Gcd( int a, int b )
{
    while ( b != 0 ) { int t = a % b; a = b; b = t; }
    return a < 0 ? -a : a;
}

/* 跨时钟/跨相位的边沿对齐：出发沿/捕捉沿在第 0 拍里的相位给定，在"公共周期"
   里找出最差的那一对边沿——
     setup：最小的"捕捉 - 出发"（> 0），也就是最紧的建立关系；
     hold ：最小的"出发 - 捕捉"（>= 0），也就是最紧的保持关系。
   同一个时钟（周期相同）时这一对就是第 0 拍里的相邻边沿；周期不同的两个时钟
   要往后找（例：出发 50 ns、捕捉 20 ns，最紧的是出发第 1 拍 50 -> 捕捉第 3 拍
   60，差 10 ns）。 */
static void FindClockPairEdges( MstaTiming *p, int nLaunchClock, int fLaunchRises,
                                int nCaptureClock, int fCaptureRises, int fSetup,
                                int *pnLaunchCycle, int *pnCaptureCycle )
{
    MstaClock *pLaunch = Msta_SdcClockByIndex( p->pSdc, nLaunchClock );
    MstaClock *pCapture = Msta_SdcClockByIndex( p->pSdc, nCaptureClock );
    double LaunchPhase = pLaunch ? ( fLaunchRises ? pLaunch->RiseEdge : pLaunch->FallEdge ) : 0.0;
    double CapturePhase = pCapture ? ( fCaptureRises ? pCapture->RiseEdge : pCapture->FallEdge ) : 0.0;
    double LaunchPeriod = ClockPeriod( p, nLaunchClock );
    double CapturePeriod = ClockPeriod( p, nCaptureClock );
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
        return;                             /* 没有周期（虚拟/未定义时钟）：不搜索 */

    /* 搜索一个公共周期里的出发拍：周期四舍五入到整数 ps 后按 gcd 算
       （非整数周期因此是近似）；超过 1000 拍时只搜 1000 拍并告警一次。 */
    kMax = 1000;
    {
        int nLaunchPs = (int)( LaunchPeriod + 0.5 ), nCapturePs = (int)( CapturePeriod + 0.5 );
        int g = Gcd( nLaunchPs, nCapturePs );
        if ( g > 0 )
        {
            kMax = ( nCapturePs + g - 1 ) / g;      /* = 公共周期 / 出发周期 */
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
        Msta_WarnOnce( "clock period ratio is too large; the launch/capture edge "
                       "search is limited to 1000 launch cycles" );
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

/* 出发沿是数据出发的那个边沿：寄存器起点是它的有效触发沿，输入端口是它的
   输入延迟参照的时钟边沿（-clock_fall 时是下降沿）。
   setup 取出发沿之后最紧的捕捉沿；hold 取不晚于出发沿的最紧的捕捉沿；
   两者都由 FindClockPairEdges 在公共周期里搜出来（*pnLaunchCycle 是出发拍）。 */
static double CaptureEdgeTime( MstaTiming *p, int fToRegister, int nCaptureNet,
                               int nPortNet,
                               int nLaunchClock, int fLaunchRises,
                               int nCaptureClock, int fCaptureRises,
                               int nCycles, int fSetup, int *pnLaunchCycle, int fLocalCapture )
{
    double CapturePeriod = ClockPeriod( p, nCaptureClock );
    MstaClock *pCaptureClock = Msta_SdcClockByIndex(p->pSdc,nCaptureClock);
    double CapturePhase = pCaptureClock ? (fCaptureRises ? pCaptureClock->RiseEdge : pCaptureClock->FallEdge) : 0.0;
    double Arrival = 0.0, SourceEdge = CapturePhase;
    int k = 0, m = 0;

    FindClockPairEdges( p, nLaunchClock, fLaunchRises, nCaptureClock, fCaptureRises,
                        fSetup, &k, &m );
    SourceEdge += (double)m * CapturePeriod;                /* 捕捉沿在第 m 拍 */
    SourceEdge += (double)(nCycles - 1) * CapturePeriod;    /* multicycle 再往后推 */
    if ( pnLaunchCycle != NULL )
        *pnLaunchCycle = k;
    if ( fToRegister )
        Arrival = ( nCaptureClock >= 0 )
                ? ClockArrival(ClockArrAt(p,nCaptureClock,nCaptureNet),!fSetup,fLocalCapture,fCaptureRises)
                : 0.0;
    else if ( pCaptureClock )
    {
        if ( nCaptureNet >= 0 )
        {
            MstaClockArr *pRefArr = ClockArrAt(p,nCaptureClock,nCaptureNet);
            Arrival = ClockArrival(pRefArr,!fSetup,fLocalCapture,fCaptureRises);
        }
        else
        {
            int fSourceIncluded = Msta_SdcPortSourceLatencyIncluded(
                p->pSdc,nPortNet,nCaptureClock,1,fSetup);
            int fNetworkIncluded = Msta_SdcPortNetworkLatencyIncluded(
                p->pSdc,nPortNet,nCaptureClock,1,fSetup);
            if ( !fSourceIncluded )
                Arrival += ClockSourceArrival(p,nCaptureClock,!fSetup,fCaptureRises);
            if ( !pCaptureClock->fPropagated && !fNetworkIncluded )
                Arrival += pCaptureClock->NetworkLatency[!fSetup][fCaptureRises];
        }
    }
    return Arrival + SourceEdge;
}

/* 跨时钟搜索把出发沿推到第 k 拍时，到达时间和出发时间一起平移 k 个出发周期。 */
static double LaunchCycleShift( MstaTiming *p, int nClock, int nLaunchCycle )
{
    return ( nLaunchCycle > 0 ) ? (double)nLaunchCycle * ClockPeriod( p, nClock ) : 0.0;
}

/* 顺着指定角的前驱走到链头，返回该路径的真实起点网络。 */
static int TimingStartNet( MstaTiming *p, int nEndNet, int fMax )
{
    int *pPrev = CornerOf(p,fMax)->pPrevNet;
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

#define MSTA_PATH_OBJECTS 4096

/* 起点端点：输入端口就是端口名，寄存器起点是实例名 + 输出脚。 */
static void StartEndpoint( MstaTiming *p, int nNet, MstaSdcObject *pObj )
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

/* 终点端点：寄存器数据脚，或者顶层输出端口。nDataPin 为 vPins 下标。 */
static void EndEndpoint( MstaTiming *p, MstaCheck *pCheck, int nDataPin,
                         MstaSdcObject *pObj )
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
    pObj->fRises = -1;      /* 调用方填入该路径的数据边沿 */
}

/* 没有指定边沿的辅助检查按到达时间选择边沿。 */
static int PathEndRise( MstaTiming *p, int nNet, int fMax )
{
    double Rise = CornerOf(p,fMax)->pArrRise[nNet];
    double Fall = CornerOf(p,fMax)->pArrFall[nNet];
    return fMax ? ( Rise >= Fall ) : ( Rise <= Fall );
}

/* 逐边沿回溯一条路径：给出每个点的网络号和该点上的信号边沿（终点 -> 起点）。 */
static int CollectEdgePath( MstaTiming *p, int nEndNet, int fMax, int fEndRise,
                            int *pNets, char *pRises, int nCap )
{
    MstaPrev *pPrevR = CornerOf(p,fMax)->pPrevRise;
    MstaPrev *pPrevF = CornerOf(p,fMax)->pPrevFall;
    int n = nEndNet, e = fEndRise, Count = 0;
    if ( e < 0 )
        e = PathEndRise( p, nEndNet, fMax );
    while ( n >= 0 && Count < nCap )
    {
        MstaPrev *pPrev = e ? pPrevR : pPrevF;
        pNets[Count]  = n;
        pRises[Count] = (char)e;
        Count++;
        if ( pPrev[n].Net < 0 )
            break;
        e = pPrev[n].Edge;
        n = pPrev[n].Net;
    }
    return Count;
}

/* 按指定的数据边沿回溯，起点也必须跟随该边沿的前驱。返回起点网络；
   pfStartRise 非空时带回起点上的数据边沿（用于引脚/端口的 -rise_from/-fall_from）。 */
static int TimingStartNetEdge( MstaTiming *p, int nNet, int fMax, int fRise, int *pfStartRise )
{
    int Guard = 0;
    while ( nNet >= 0 && Guard++ < p->pDes->vNets.nSize )
    {
        MstaPrev Prev = (fRise ? CornerOf(p,fMax)->pPrevRise
                              : CornerOf(p,fMax)->pPrevFall)[nNet];
        if ( Prev.Net < 0 ) break;
        nNet = Prev.Net;
        fRise = Prev.Edge;
    }
    if ( pfStartRise )
        *pfStartRise = fRise;
    return nNet;
}

/* 收集候选路径上的对象（起点 -> 终点方向）：
   每个网络一个对象，它的驱动实例再一个对象，带上驱动脚的 "实例/引脚" 形式和
   该点上的信号边沿（-rise_through/-fall_through 要用）。 */
static int CollectPathObjects( MstaTiming *p, int nEndNet, int fMax, int fEndRise,
                               MstaSdcObject *pObjects, int nCap )
{
    int nNets[MSTA_PATH_OBJECTS];
    char vRises[MSTA_PATH_OBJECTS];
    int nCount, i, Count = 0;
    nCount = CollectEdgePath( p, nEndNet, fMax, fEndRise, nNets, vRises, MSTA_PATH_OBJECTS );
    for ( i = nCount - 1; i >= 0 && Count + 2 <= nCap; i-- )
    {
        MstaNet *pNet = MstaNetArrayAt( &p->pDes->vNets, nNets[i] );
        pObjects[Count].pText  = Msta_NetName( p->pDes, nNets[i] );
        pObjects[Count].nInst  = -1;
        pObjects[Count].nPin   = -1;
        pObjects[Count].nNet   = nNets[i];
        pObjects[Count].fRises = vRises[i];
        Count++;
        if ( pNet->Driver.InstId != MSTA_NO_ID )
        {
            pObjects[Count].pText  = Msta_InstName( p->pDes, pNet->Driver.InstId );
            pObjects[Count].nInst  = pNet->Driver.InstId;
            pObjects[Count].nPin   = pNet->Driver.PinId;
            pObjects[Count].nNet   = nNets[i];
            pObjects[Count].fRises = vRises[i];
            Count++;
        }
    }
    return Count;
}

/* 终点参考时钟的边沿；数据边沿另存于端点对象，不能用于时钟集合匹配。 */
static int EndpointRises( MstaTiming *p, MstaCheck *pCheck, MstaRegCheck *pReg,
                          int nCaptureClock, int fSetup )
{
    if ( pCheck->fToRegister )
    {
        int nClkNet;
        if ( pReg == NULL )
            return -1;
        nClkNet = InstPinNet( p, pCheck->InstId, pReg->ClkPin );
        return p->CaptureClockTag >= 0 ? p->CaptureClockTag : EffectiveClkRises(p,nCaptureClock,nClkNet,pReg->fClkRises);
    }
    int fRise = !Msta_SdcPortClockFall(p->pSdc,pCheck->nEndNet,nCaptureClock,1,fSetup);
    int nRef = Msta_SdcPortReferenceNet(p->pSdc,pCheck->nEndNet,nCaptureClock,1,fSetup);
    if ( nRef >= 0 && p->CaptureClockTag >= 0 ) return p->CaptureClockTag;
    return fRise;
}

/* 组装起点端点的全部匹配信息（调用前 launch clock 必须已经确定）。 */
static void BuildStartEndpoint( MstaTiming *p, int nNet, int fMax,
                                MstaSdcEndpoint *pEnd, MstaSdcObject *pObj )
{
    StartEndpoint( p, nNet, pObj );
    pEnd->pObj   = pObj;
    pEnd->pClock = Msta_NameStr( Msta_SdcClockByIndex( p->pSdc,
                       CornerOf(p,fMax)->pnLaunchClock[nNet] )->Name );
    pEnd->fRises = CornerOf(p,fMax)->pfLaunchRises[nNet];
}

/* 组装终点端点的全部匹配信息。 */
static void BuildEndEndpoint( MstaTiming *p, MstaCheck *pCheck, MstaRegCheck *pReg,
                              int nCaptureClock, int fSetup,
                              MstaSdcEndpoint *pEnd, MstaSdcObject *pObj )
{
    int nDataPin = -1;
    if ( pCheck->fToRegister && pReg )
    {
        MstaInst *pInst = MstaInstArrayAt( &p->pDes->vInsts, pCheck->InstId );
        nDataPin = Msta_CellPinIndexOf( pInst->pCell, pReg->DataPin );
    }
    EndEndpoint( p, pCheck, nDataPin, pObj );
    pEnd->pObj   = pObj;
    pEnd->pClock = Msta_NameStr( Msta_SdcClockByIndex(p->pSdc,nCaptureClock)->Name );
    pEnd->fRises = EndpointRises( p, pCheck, pReg, nCaptureClock, fSetup );
}

/* 这条路径归在哪个分组：先按 SDC 里的 group_path 匹配，都不命中就按捕获时钟归组。 */
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

static void SaveCheckCorner( MstaTiming *p, MstaCheck *pBest,
                             const MstaCheck *pCandidate, int fMax );

static void FreeCheckCornerPath( MstaCheckCorner *pC )
{
    free( pC->pPath );
    free( pC->pPathArrival );
    pC->pPath = NULL;
    pC->pPathArrival = NULL;
    pC->nPath = 0;
}

static void FreeCheckPaths( MstaCheck *pCheck )
{
    FreeCheckCornerPath( &pCheck->Setup );
    FreeCheckCornerPath( &pCheck->Hold );
}

/* 把端点的标识字段拷进一次尝试用的候选（结果字段由这次尝试自己填）。 */
static void CopyCheckIdentity( MstaCheck *pDst, const MstaCheck *pSrc )
{
    pDst->InstId      = pSrc->InstId;
    pDst->nCheck      = pSrc->nCheck;
    pDst->nDataPin    = pSrc->nDataPin;
    pDst->nEndNet     = pSrc->nEndNet;
    pDst->fToRegister = pSrc->fToRegister;
    /* 异步端点靠下面这几个标志识别，而不是 nCheck。 */
    pDst->fAsync      = pSrc->fAsync;
    pDst->fRecovery   = pSrc->fRecovery;
    pDst->fRemoval    = pSrc->fRemoval;
    pDst->nAsyncCheck = pSrc->nAsyncCheck;
    pDst->Setup.Group  = MSTA_NO_ID;
    pDst->Hold.Group   = MSTA_NO_ID;
}

static void KeepWorseCandidate( MstaTiming *p, MstaCheck *pBest, MstaCheck *pCandidate )
{
    if ( pCandidate->fCutByException )
        pBest->fCutByException = 1;
    if ( pCandidate->Setup.fChecked &&
         (!pBest->Setup.fChecked || pCandidate->Setup.Slack < pBest->Setup.Slack) )
        SaveCheckCorner( p, pBest, pCandidate, 1 );
    if ( pCandidate->Hold.fChecked &&
         (!pBest->Hold.fChecked || pCandidate->Hold.Slack < pBest->Hold.Slack) )
        SaveCheckCorner( p, pBest, pCandidate, 0 );
    FreeCheckPaths( pCandidate );
}

/* 检查一个异步控制脚的 recovery 或 removal 端点。
   nCaptureClock 是这个时钟脚所在的时钟（同一根网络可以挂多个时钟）。 */
static void CheckAsyncEndpointEdge( MstaTiming *p, MstaCheck *pCheck, int nCaptureClock, int fDataRise )
{
    MstaInst *pInst = InstAt(p,pCheck->InstId);
    MstaCell *pCell = pInst->pCell;
    MstaAsyncCheck *pAsync = MstaAsyncCheckArrayAt(&pCell->vAsync,pCheck->nAsyncCheck);
    int nClkNet = InstPinNet(p,pCheck->InstId,pAsync->ClkPin);
    int nClock, nEndNet = pCheck->nEndNet;
    int fStartRise;
    int nStart = TimingStartNetEdge(p,nEndNet,pCheck->fRecovery,fDataRise,&fStartRise);
    MstaCorner *pCorner = CornerOf(p,pCheck->fRecovery);
    int *pLaunchClock = CornerOf(p,pCheck->fRecovery)->pnLaunchClock;
    double *pArrival = fDataRise ? CornerOf(p,pCheck->fRecovery)->pArrRise
                               : CornerOf(p,pCheck->fRecovery)->pArrFall;
    MstaSdcObject FromObj, ToObj, PathObjects[MSTA_PATH_OBJECTS];
    MstaSdcEndpoint From, To;
    int nPathObjects, nCycles = 1, fFalse = 0;
    MstaId nArcId;
    MstaArc *pArc;
    double Check, Capture, Required, Shift;
    int fClkRises, nLaunchCycle = 0;
    nClock = nCaptureClock;
    if ( nClkNet < 0 || nClock < 0 || pLaunchClock[nStart] < 0 ||
         !ArrivalSet(pArrival[nEndNet],pCheck->fRecovery) ) return;
    /* 捕获沿按当前源边沿标签取；它传不到库定义的本地沿时不查。 */
    fClkRises = p->CaptureClockTag;
    if ( !ClockTagValid(ClockArrAt(p,nClock,nClkNet),!pCheck->fRecovery,pAsync->fClkRises,fClkRises) ) return;

    /* 异步控制脚同样接受 set_false_path / set_multicycle_path / set_max_delay：
       recovery 按 setup 处理，removal 按 hold 处理。 */
    StartEndpoint( p, nStart, &FromObj );
    EndEndpoint( p, pCheck, Msta_CellPinIndexOf(pCell,pAsync->AsyncPin), &ToObj );
    From.pObj   = &FromObj;
    From.pClock = Msta_NameStr( Msta_SdcClockByIndex(p->pSdc,pLaunchClock[nStart])->Name );
    From.fRises = pCorner->pfLaunchRises[nStart];
    FromObj.fRises = fStartRise;
    ToObj.fRises = fDataRise;
    To.pObj     = &ToObj;
    To.pClock   = Msta_NameStr( Msta_SdcClockByIndex(p->pSdc,nClock)->Name );
    To.fRises   = fClkRises;
    nPathObjects = CollectPathObjects( p, nEndNet, pCheck->fRecovery, fDataRise,
                                       PathObjects, MSTA_PATH_OBJECTS );
    if ( nPathObjects < MSTA_PATH_OBJECTS )
        PathObjects[nPathObjects++] = ToObj;   /* 终点脚本身也是路径上的一个点 */
    /* 异步检查固定归入 MSTA_SDC_GROUP_ASYNC 组，不看 group_path，也不按捕获时钟分组。 */
    if ( pCheck->fRecovery ) pCheck->Setup.Group = Msta_NameId( MSTA_SDC_GROUP_ASYNC );
    else                     pCheck->Hold.Group  = Msta_NameId( MSTA_SDC_GROUP_ASYNC );
    Msta_SdcFindExceptionPath( p->pSdc, p->pDes, &From, &To, pCheck->fRecovery,
                               PathObjects, nPathObjects, &nCycles, &fFalse );
    if ( fFalse )
    {
        pCheck->fCutByException = 1;
        return;
    }

    nArcId = pCheck->fRecovery
           ? (pAsync->RecoveryArc != MSTA_NO_ID ? pAsync->RecoveryArc : pAsync->RecoveryFallArc)
           : (pAsync->RemovalArc != MSTA_NO_ID ? pAsync->RemovalArc : pAsync->RemovalFallArc);
    pArc = Msta_CellArcById(pCell,nArcId);
    if ( pArc == NULL || !CheckHasEdge(p,pArc,pCheck->InstId,pCheck->fRecovery,fDataRise) ) return;
    Check = CheckTime(p,pArc,pCheck->InstId,nClock,nClkNet,nEndNet,pCheck->fRecovery,fDataRise);
    Capture = CaptureEdgeTime(p,1,nClkNet,-1,pLaunchClock[nStart],pCorner->pfLaunchRises[nStart],
                              nClock,fClkRises,nCycles,pCheck->fRecovery,&nLaunchCycle,pAsync->fClkRises);
    Required = pCheck->fRecovery
             ? Capture - ClockUncertainty(p,pLaunchClock[nStart],nClock,1,
                                          pCorner->pfLaunchRises[nStart],fClkRises) - Check
             : Capture + ClockUncertainty(p,pLaunchClock[nStart],nClock,0,
                                          pCorner->pfLaunchRises[nStart],fClkRises) + Check;
    pCheck->CaptureClock = Msta_SdcClockByIndex(p->pSdc,nClock)->Name;
    if ( pCheck->fRecovery )
    {
        Shift = LaunchCycleShift( p, pLaunchClock[nStart], nLaunchCycle );
        pCheck->Setup.LaunchSourceRise = pCorner->pfLaunchRises[nStart];
        pCheck->Setup.CaptureSourceRise = fClkRises;
        pCheck->Setup.fDataRise = fDataRise;
        pCheck->Setup.Arrival = pArrival[nEndNet] + Shift;
        pCheck->Setup.Required = Required;
        pCheck->Setup.Slack = Required - pCheck->Setup.Arrival;
        pCheck->Setup.CheckTime = Check;
        pCheck->Setup.Uncertainty = ClockUncertainty(p,pLaunchClock[nStart],nClock,1,
                                                    pCorner->pfLaunchRises[nStart],fClkRises);
        pCheck->Setup.LaunchClock = Msta_SdcClockByIndex(p->pSdc,pLaunchClock[nStart])->Name;
        pCheck->Setup.LaunchTime = p->CornerMax.pdLaunchEdge[nStart] + Shift;
        pCheck->Setup.CaptureTime = Capture;
        pCheck->Setup.LaunchShift = Shift;
        pCheck->Setup.fChecked = 1;
    }
    else
    {
        Shift = LaunchCycleShift( p, pLaunchClock[nStart], nLaunchCycle );
        pCheck->Hold.LaunchSourceRise = pCorner->pfLaunchRises[nStart];
        pCheck->Hold.CaptureSourceRise = fClkRises;
        pCheck->Hold.fDataRise = fDataRise;
        pCheck->Hold.Arrival = pArrival[nEndNet] + Shift;
        pCheck->Hold.Required = Required;
        pCheck->Hold.Slack = pCheck->Hold.Arrival - Required;
        pCheck->Hold.CheckTime = Check;
        pCheck->Hold.Uncertainty = ClockUncertainty(p,pLaunchClock[nStart],nClock,0,
                                                   pCorner->pfLaunchRises[nStart],fClkRises);
        pCheck->Hold.LaunchClock = Msta_SdcClockByIndex(p->pSdc,pLaunchClock[nStart])->Name;
        pCheck->Hold.LaunchTime = p->CornerMin.pdLaunchEdge[nStart] + Shift;
        pCheck->Hold.CaptureTime = Capture;
        pCheck->Hold.LaunchShift = Shift;
        pCheck->Hold.fChecked = 1;
    }
}

static void CheckAsyncEndpointOne( MstaTiming *p, MstaCheck *pCheck, int nCaptureClock )
{
    int Edge, Tag;
    for ( Tag = 0; Tag < 2; Tag++ )
    for ( Edge = 1; Edge >= 0; Edge-- )
    {
        p->CaptureClockTag = Tag;
        MstaCheck Candidate;
        memset(&Candidate, 0, sizeof(Candidate));
        CopyCheckIdentity(&Candidate, pCheck);
        CheckAsyncEndpointEdge(p, &Candidate, nCaptureClock, Edge);
        KeepWorseCandidate(p, pCheck, &Candidate);
    }
    p->CaptureClockTag = -1;
    pCheck->fPathSaved = pCheck->Setup.fChecked || pCheck->Hold.fChecked;
}

/* 异步控制脚的时钟网络上有几个时钟就按几个时钟各查一遍，取最差。 */
static void CheckAsyncEndpoint( MstaTiming *p, MstaCheck *pCheck )
{
    MstaInst *pInst = InstAt(p,pCheck->InstId);
    MstaAsyncCheck *pAsync = MstaAsyncCheckArrayAt(&pInst->pCell->vAsync,pCheck->nAsyncCheck);
    int nClkNet = InstPinNet(p,pCheck->InstId,pAsync->ClkPin);
    int c;

    if ( nClkNet < 0 )
        return;
    for ( c = 0; c < Msta_SdcClockCount(p->pSdc); c++ )
    {
        MstaCheck Candidate;
        if ( !NetHasDataClock(p,c,nClkNet) )
            continue;
        memset( &Candidate, 0, sizeof(Candidate) );
        CopyCheckIdentity( &Candidate, pCheck );
        CheckAsyncEndpointOne( p, &Candidate, c );
        KeepWorseCandidate( p, pCheck, &Candidate );
    }
}

/* 锁存器的开窗宽度：从关闭沿往回退到开沿。
   高电平锁存的关闭沿是下降沿，窗口就是时钟的高电平宽度；低电平锁存反过来。 */
static double LatchOpenWindow( MstaClock *pClock, int fCloseRises )
{
    double d = fCloseRises ? ( pClock->RiseEdge - pClock->FallEdge )
                           : ( pClock->FallEdge - pClock->RiseEdge );
    while ( d <= 0.0 ) d += pClock->Period;
    return d;
}

/* 锁存器 D 脚的要求时间。没写 set_max_time_borrow 时 Required 里已经是
   "关闭沿减 setup"；写了 v 时要求时间取"开沿 + v"（v 越小越紧）。
   返回用到的借时值，没写返回 MSTA_UNSET。 */
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

/* 给一个角套用路径例外。返回 1 = 路径保留，0 = 被 false path 切掉（命中的 -through
   段追加到 pExclude，供找次优路径）。*pnCycles / *pPathDelay 带回 multicycle 与
   set_max_delay/set_min_delay；顺带设置该角的路径分组 Group。 */
static int ApplyCornerExceptions( MstaTiming *p, MstaCheck *pCheck, MstaRegCheck *pReg,
                                  int nCaptureClock, int nStart, int fSetup, int fDataRise,
                                  int *pnCycles, double *pPathDelay,
                                  MstaPathExclude *pExclude, int *pnExclude )
{
    int nEndNet = pCheck->nEndNet;
    MstaSdcObject PathObjects[MSTA_PATH_OBJECTS];
    MstaSdcObject FromObj, ToObj;
    MstaSdcEndpoint From, To;
    int nPathObjects, fFalse = 0, fKeep = 1;
    BuildStartEndpoint( p, nStart, fSetup, &From, &FromObj );
    BuildEndEndpoint( p, pCheck, pReg, nCaptureClock, fSetup, &To, &ToObj );
    TimingStartNetEdge( p, nEndNet, fSetup, fDataRise, &FromObj.fRises );
    ToObj.fRises = fDataRise;
    nPathObjects = CollectPathObjects(p, nEndNet, fSetup, fDataRise,
                                      PathObjects, MSTA_PATH_OBJECTS);
    if ( nPathObjects < MSTA_PATH_OBJECTS )
        PathObjects[nPathObjects++] = ToObj;
    Msta_CheckCorner( pCheck, fSetup )->Group = PathGroupOf( p, nCaptureClock, &From, &To,
                                                             PathObjects, nPathObjects );
    Msta_SdcFindExceptionPath( p->pSdc, p->pDes, &From, &To, fSetup,
                               PathObjects, nPathObjects, pnCycles, &fFalse );
    if ( fFalse )
    {
        fKeep = 0;
        pCheck->fCutByException = 1;
        /* 记下这条例外命中路径上的 -through 段，供调用方找次优路径。 */
        *pnExclude += Msta_SdcPathExclusions( p->pSdc, p->pDes, &From, &To, fSetup,
                                              PathObjects, nPathObjects,
                                              pExclude + *pnExclude,
                                              MSTA_MAX_PATH_EXCLUDE - *pnExclude );
    }
    *pPathDelay = Msta_SdcFindPathDelay( p->pSdc, p->pDes, &From, &To,
                                         PathObjects, nPathObjects, fSetup );
    return fKeep;
}

static void EvaluateCheckCorner( MstaTiming *p, MstaCheck *pCheck, int fSetup,
                                 MstaCell *pCell, MstaRegCheck *pReg,
                                 int nCaptureClock, int nCaptureNet, int nStart,
                                 int nOutputRef, double OutDelay,
                                 int nCycles, double PathDelay, int fDataRise )
{
    MstaCorner *pCorner = CornerOf( p, fSetup );
    MstaCheckCorner *pResult = Msta_CheckCorner( pCheck, fSetup );
    int nEndNet = pCheck->nEndNet;
    int nLaunchClock = pCorner->pnLaunchClock[nStart];
    int nPortRef = pCheck->fToRegister ? -1 : nOutputRef;
    int fLocalCapture = pCheck->fToRegister ? pReg->fClkRises
        : !Msta_SdcPortClockFall(p->pSdc,nEndNet,nCaptureClock,1,fSetup);
    int fCaptureRises = p->CaptureClockTag;
    int fLaunchRises = pCorner->pfLaunchRises[nStart];
    int nLaunchCycle = 0;
    double Capture, Check, Uncertainty, Required, Shift;
    Capture = CaptureEdgeTime( p, pCheck->fToRegister,
                              pCheck->fToRegister ? nCaptureNet : nPortRef,
                              nEndNet,
                              nLaunchClock, fLaunchRises,
                              nCaptureClock, fCaptureRises,
                              nCycles, fSetup, &nLaunchCycle, fLocalCapture );
    Shift = LaunchCycleShift( p, nLaunchClock, nLaunchCycle );
    if ( pCheck->fToRegister )
    {
        MstaId nArc = fSetup
                    ? ( ( pReg->SetupArc != MSTA_NO_ID ) ? pReg->SetupArc : pReg->SetupFallArc )
                    : ( ( pReg->HoldArc != MSTA_NO_ID ) ? pReg->HoldArc : pReg->HoldFallArc );
        MstaArc *pArc = Msta_CellArcById( pCell, nArc );
        if ( !CheckHasEdge(p,pArc,pCheck->InstId,fSetup,fDataRise) ) return;
        Check = ( pArc != NULL ) ? CheckTime( p, pArc, pCheck->InstId, nCaptureClock, nCaptureNet, nEndNet, fSetup, fDataRise ) : 0.0;
    }
    else
        Check = fSetup ? OutDelay : -OutDelay;
    Uncertainty = ClockUncertainty( p, nLaunchClock, nCaptureClock, fSetup,
                                    fLaunchRises, fCaptureRises );
    if ( fSetup )
    {
        Required = Capture - Uncertainty - Check;
        /* 锁存器：写了 set_max_time_borrow 时，要求时间换成"开沿 + 借时"。 */
        pResult->Borrow = LatchBorrow( p, pCheck, pCell, nCaptureClock, fCaptureRises,
                                       Capture, &Required );
    }
    else
        Required = Capture + Uncertainty + Check;
    pResult->LaunchSourceRise = fLaunchRises;
    pResult->CaptureSourceRise = fCaptureRises;
    pResult->fDataRise   = fDataRise;
    pResult->Arrival     = (fDataRise ? pCorner->pArrRise[nEndNet] : pCorner->pArrFall[nEndNet]) + Shift;
    pResult->Required    = Required;
    pResult->Slack       = fSetup ? Required - pResult->Arrival : pResult->Arrival - Required;
    pResult->CheckTime   = Check;
    pResult->Uncertainty = Uncertainty;
    pResult->LaunchTime  = pCorner->pdLaunchEdge[nStart] + Shift;
    pResult->CaptureTime = Capture;
    pResult->LaunchClock = Msta_SdcClockByIndex(p->pSdc,nLaunchClock)->Name;
    pResult->LaunchShift = Shift;
    pResult->fChecked    = 1;
    if ( Msta_IsSet(PathDelay) )
    {
        pResult->CheckTime = PathDelay;
        pResult->Required = pResult->LaunchTime + PathDelay;
        pResult->Slack = fSetup ? pResult->Required - pResult->Arrival
                                : pResult->Arrival - pResult->Required;
    }
}

/* 按当前到达时间把一个端点检查一遍：路径被例外切掉时，把命中的那一段
   (网络, 边沿) 记到 pExclude 里返回给调用方。 */
static void CheckEndpointAttempt( MstaTiming *p, MstaCheck *pCheck, int nCaptureClock,
                                  MstaPathExclude *pExclude, int *pnExclude, int fDataRise )
{
    int nEndNet = pCheck->nEndNet;
    int nStartMax = TimingStartNetEdge( p, nEndNet, 1, fDataRise, NULL );
    int nStartMin = TimingStartNetEdge( p, nEndNet, 0, fDataRise, NULL );
    int nCaptureNet = -1;
    int nOutputRefMax = -1, nOutputRefMin = -1;
    int fSetup = ArrivalSet(fDataRise ? p->CornerMax.pArrRise[nEndNet]
                                         : p->CornerMax.pArrFall[nEndNet], 1)
                 && nStartMax >= 0 && p->CornerMax.pnLaunchClock[nStartMax] >= 0;
    int fHold = ArrivalSet(fDataRise ? p->CornerMin.pArrRise[nEndNet]
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
        MstaInst *pInst = InstAt( p, pCheck->InstId );
        pCell = pInst->pCell;
        pReg  = MstaRegCheckArrayAt( &pCell->vRegs, pCheck->nCheck );
        nCaptureNet = InstPinNet( p, pCheck->InstId, pReg->ClkPin );
        if ( nCaptureNet < 0 )
            return;
    }
    else
    {
        pCons = Msta_SdcNetCons( p->pSdc, nEndNet );
        if ( pCons == NULL )
            return;
        OutMax = Msta_SdcPortDelay(p->pSdc,nEndNet,nCaptureClock,1,1);
        OutMin = Msta_SdcPortDelay(p->pSdc,nEndNet,nCaptureClock,1,0);
        nOutputRefMax = Msta_SdcPortReferenceNet(p->pSdc,nEndNet,nCaptureClock,1,1);
        nOutputRefMin = Msta_SdcPortReferenceNet(p->pSdc,nEndNet,nCaptureClock,1,0);
        if ( fSetup && !Msta_IsSet(OutMax) ) fSetup = 0;
        if ( fHold  && !Msta_IsSet(OutMin) ) fHold  = 0;
        if ( fSetup && nOutputRefMax >= 0 &&
             !ClockArrAt(p,nCaptureClock,nOutputRefMax)->fReached ) fSetup = 0;
        if ( fHold && nOutputRefMin >= 0 &&
             !ClockArrAt(p,nCaptureClock,nOutputRefMin)->fReached ) fHold = 0;
    }
    if ( nCaptureClock < 0 )
        return;

    if ( pCheck->fToRegister )
    {
        MstaClockArr *pClk = ClockArrAt(p,nCaptureClock,nCaptureNet);
        if ( fSetup && !ClockTagValid(pClk,0,pReg->fClkRises,p->CaptureClockTag) ) fSetup = 0;
        if ( fHold && !ClockTagValid(pClk,1,pReg->fClkRises,p->CaptureClockTag) ) fHold = 0;
    }
    else
    {
        int RiseMax = !Msta_SdcPortClockFall(p->pSdc,nEndNet,nCaptureClock,1,1);
        int RiseMin = !Msta_SdcPortClockFall(p->pSdc,nEndNet,nCaptureClock,1,0);
        if ( fSetup && (nOutputRefMax >= 0 ? !ClockTagValid(ClockArrAt(p,nCaptureClock,nOutputRefMax),0,RiseMax,p->CaptureClockTag)
                                          : p->CaptureClockTag != RiseMax) ) fSetup = 0;
        if ( fHold && (nOutputRefMin >= 0 ? !ClockTagValid(ClockArrAt(p,nCaptureClock,nOutputRefMin),1,RiseMin,p->CaptureClockTag)
                                        : p->CaptureClockTag != RiseMin) ) fHold = 0;
    }
    /* setup 与 hold 相互独立：缺了 min/max 某一侧的约束，不能让另一侧的检查
       也跟着消失。缺检查表时怎么办见 CheckHasEdge。 */
    if ( !fSetup && !fHold )
        return;

    if ( fSetup )
        fSetup = ApplyCornerExceptions( p, pCheck, pReg, nCaptureClock, nStartMax, 1, fDataRise,
                                        &nSetupCycles, &PathMax, pExclude, pnExclude );
    if ( fHold )
        fHold = ApplyCornerExceptions( p, pCheck, pReg, nCaptureClock, nStartMin, 0, fDataRise,
                                       &nHoldCycles, &PathMin, pExclude, pnExclude );
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

/* =====================================================================
   6c. 端点检查入口与次优路径（路径打印在 msta_report.c）
   ===================================================================== */

/* 把新的排除项并进排除表（同一根网络的边沿掩码取"或"）。
   返回 1 = 有新的到达时间被排除（值得重算），0 = 没有进展或表满了。 */
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
            if ( p->vPathExclude[j].Net != pNew[i].Net )
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

/* 一个捕获时钟下把这个端点检查一遍：先按当前到达时间查一次；某个角被路径例外
   切掉时，把命中的那一段 (网络, 边沿) 排除掉重算，换到下一条次优路径，
   直到每个角都拿到一条没被切掉的路径（或没有候选了）。
   这样做是因为 -fall_through 这类例外只切掉命中的路径，而不是整个端点。 */
static void CheckEndpointEdge( MstaTiming *p, MstaCheck *pCheck, int nCaptureClock,
                              int nPropClock, int nStartClass, int fDataRise )
{
    int nIter;
    pCheck->Setup.fChecked = pCheck->Hold.fChecked = 0;
    pCheck->fCutByException = 0;
    for ( nIter = 0; nIter <= MSTA_MAX_PATH_EXCLUDE; nIter++ )
    {
        MstaPathExclude vNew[MSTA_MAX_PATH_EXCLUDE];
        MstaCheck Try;
        int nNew = 0;
        memset( &Try, 0, sizeof(Try) );
        CopyCheckIdentity( &Try, pCheck );
        CheckEndpointAttempt( p, &Try, nCaptureClock, vNew, &nNew, fDataRise );
        /* 每个角取第一条没被切掉的路径（尝试是从最差路径往后走的）。 */
        if ( Try.Setup.fChecked && !pCheck->Setup.fChecked )
            SaveCheckCorner( p, pCheck, &Try, 1 );
        if ( Try.Hold.fChecked && !pCheck->Hold.fChecked )
            SaveCheckCorner( p, pCheck, &Try, 0 );
        if ( pCheck->Setup.fChecked || pCheck->Hold.fChecked )
            pCheck->fPathSaved = 1;             /* 路径快照已经存好，调用方别再回溯 */
        if ( Try.fCutByException )
            pCheck->fCutByException = 1;
        FreeCheckPaths( &Try );
        if ( pCheck->Setup.fChecked && pCheck->Hold.fChecked )
            return;                             /* 两个角都有着落了 */
        if ( nNew == 0 || !AddPathExclusions(p,vNew,nNew) )
            return;                             /* 例外没有可细分的点 */
        PropagateData( p, 1, nPropClock, nStartClass );
        PropagateData( p, 0, nPropClock, nStartClass );
    }
}

/* 清掉临时排除的到达时间，恢复干净的传播状态（找次优路径时用过）。 */
static void ClearPathExclusions( MstaTiming *p, int nPropClock, int nStartClass )
{
    if ( p->nPathExclude == 0 )
        return;
    p->nPathExclude = 0;
    PropagateData( p, 1, nPropClock, nStartClass );
    PropagateData( p, 0, nPropClock, nStartClass );
}

/* 每个数据边沿独立应用例外、查约束并保存路径，端点只统计最差 slack。 */
static void CheckEndpointOne( MstaTiming *p, MstaCheck *pCheck, int nCaptureClock,
                              int nPropClock, int nStartClass )
{
    int Edge, Tag;
    pCheck->Setup.fChecked = pCheck->Hold.fChecked = 0;
    for ( Tag = 0; Tag < 2; Tag++ )
    for ( Edge = 1; Edge >= 0; Edge-- )
    {
        p->CaptureClockTag = Tag;
        MstaCheck Candidate;
        memset(&Candidate, 0, sizeof(Candidate));
        CopyCheckIdentity(&Candidate, pCheck);
        CheckEndpointEdge(p, &Candidate, nCaptureClock, nPropClock, nStartClass, Edge);
        KeepWorseCandidate(p, pCheck, &Candidate);
        ClearPathExclusions(p, nPropClock, nStartClass);
    }
    p->CaptureClockTag = -1;
    pCheck->fPathSaved = pCheck->Setup.fChecked || pCheck->Hold.fChecked;
}

/* 端点检查的总入口：一个端点的捕获网络上有几个时钟就按几个时钟各查一遍，
   取最差的那次（create_clock -add 允许同一根网络挂多个时钟）。
   nPropClock / nStartClass 是这次传播用的 (时钟, 起点分类)，找次优路径时要拿它重算。 */
static void CheckEndpoint( MstaTiming *p, MstaCheck *pCheck, int nOutputClock,
                           int nPropClock, int nStartClass )
{
    int c, nClkNet;

    if ( pCheck->fAsync )
    {
        CheckAsyncEndpoint(p,pCheck);
        return;
    }
    if ( !pCheck->fToRegister )
    {
        CheckEndpointOne( p, pCheck, nOutputClock, nPropClock, nStartClass );
        ClearPathExclusions( p, nPropClock, nStartClass );
        return;
    }
    {
        MstaInst *pInst = InstAt( p, pCheck->InstId );
        MstaRegCheck *pReg = MstaRegCheckArrayAt( &pInst->pCell->vRegs, pCheck->nCheck );
        nClkNet = InstPinNet( p, pCheck->InstId, pReg->ClkPin );
    }
    for ( c = 0; c < Msta_SdcClockCount(p->pSdc); c++ )
    {
        MstaCheck Candidate;
        if ( !NetHasDataClock(p,c,nClkNet) )
            continue;
        memset( &Candidate, 0, sizeof(Candidate) );
        CopyCheckIdentity( &Candidate, pCheck );
        CheckEndpointOne( p, &Candidate, c, nPropClock, nStartClass );
        KeepWorseCandidate( p, pCheck, &Candidate );
        ClearPathExclusions( p, nPropClock, nStartClass );
    }
}

/* 起点/终点的可打印名字：
     起点 = 驱动这根网络的那个实例名（FF 的话就是那个 FF）
     终点 = FF 实例名（reg2reg）或输出端口名（reg2out） */
const char *Msta_TimingEndpointName( MstaTiming *p, MstaCheck *pCheck,
                                     int fStart, int fMax )
{
    const MstaCheckCorner *pCorner = Msta_CheckCorner( pCheck, fMax );
    if ( !fStart )
        return pCheck->fToRegister ? Msta_InstName(p->pDes,pCheck->InstId)
                                   : Msta_NetName(p->pDes,pCheck->nEndNet);
    if ( pCorner->pPath && pCorner->nPath > 0 )
        return TimingStartName(p,pCorner->pPath[pCorner->nPath-1]);
    return TimingStartName( p, TimingStartNet(p,pCheck->nEndNet,fMax) );
}

/* =====================================================================
   7. 汇总与总入口
   ---------------------------------------------------------------------
   这一节还放着生命周期、查询接口，以及总入口直接调用的几个步骤函数
   （WarnUntimedLatches、起点分类、数据/门控检查、DRC）。
   ===================================================================== */

static void AllocCorner( MstaCorner *pC, int n )
{
    int n4 = n > 0 ? n : 1;
    pC->pArr          = (double *)calloc( (size_t)n, sizeof(double) );
    pC->pArrRise      = (double *)calloc( (size_t)n, sizeof(double) );
    pC->pArrFall      = (double *)calloc( (size_t)n, sizeof(double) );
    pC->pSlew         = (double *)calloc( (size_t)n, sizeof(double) );
    pC->pSlewRise     = (double *)calloc( (size_t)n, sizeof(double) );
    pC->pSlewFall     = (double *)calloc( (size_t)n, sizeof(double) );
    pC->pnLaunchClock = (int *)malloc( (size_t)n * sizeof(int) );
    pC->pdLaunchEdge  = (double *)calloc( (size_t)n, sizeof(double) );
    pC->pfLaunchRises = (char *)calloc( (size_t)n, sizeof(char) );
    pC->pPrevNet      = (int *)malloc( (size_t)n * sizeof(int) );
    pC->pPrevRise     = (MstaPrev *)malloc( (size_t)n4 * sizeof(MstaPrev) );
    pC->pPrevFall     = (MstaPrev *)malloc( (size_t)n4 * sizeof(MstaPrev) );
}

static void FreeCorner( MstaCorner *pC )
{
    free( pC->pArr );
    free( pC->pArrRise );
    free( pC->pArrFall );
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
    p->pfClockNet        = (char *)calloc( (size_t)n, sizeof(char) );
    p->pfIdealNet        = (char *)calloc( (size_t)n, sizeof(char) );
    p->pClockArr         = (MstaClockArr *)calloc( (size_t)n * (size_t)nClocks,
                                                   sizeof(MstaClockArr) );
    p->pfClockOfNet      = (char *)calloc( (size_t)n * (size_t)nClocks, sizeof(char) );
    AllocCorner( &p->CornerMax, n );
    AllocCorner( &p->CornerMin, n );
    p->nPathExclude      = 0;
    p->nClockPairs       = 0;      /* 时钟对缓存：SDC 变了要重算 */
    p->pTopoOrder        = (int *)malloc( (size_t)n * sizeof(int) );
    p->pState            = (int *)calloc( (size_t)n, sizeof(int) );
    p->pStartClass       = (int *)malloc( (size_t)n4 * sizeof(int) );
    assert( p->pfClockNet && p->pClockArr && p->pfClockOfNet && p->pTopoOrder );
}

static void FreeNetArrays( MstaTiming *p )
{
    free( p->pfClockNet );        p->pfClockNet = NULL;
    free( p->pfIdealNet );        p->pfIdealNet = NULL;
    free( p->pClockArr );         p->pClockArr = NULL;
    free( p->pfClockOfNet );      p->pfClockOfNet = NULL;
    FreeCorner( &p->CornerMax );
    FreeCorner( &p->CornerMin );
    free( p->pTopoOrder );   p->pTopoOrder = NULL;
    free( p->pState );       p->pState = NULL;
    free( p->pStartClass );  p->pStartClass = NULL;
}

static void FreeChecks( MstaTiming *p )
{
    int i;
    for ( i = 0; i < p->vChecks.nSize; i++ )
        FreeCheckPaths( MstaCheckArrayAt(&p->vChecks,i) );
    MstaCheckArrayFree(&p->vChecks);
}

static void SaveCheckCorner( MstaTiming *p, MstaCheck *pBest,
                             const MstaCheck *pCandidate, int fMax )
{
    MstaCorner *pCorner = CornerOf( p, fMax );
    MstaCheckCorner *pDst = fMax ? &pBest->Setup : &pBest->Hold;
    const MstaCheckCorner *pSrc = fMax ? &pCandidate->Setup : &pCandidate->Hold;
    int n, Count = 0, i;

    free( pDst->pPath );
    free( pDst->pPathArrival );
    *pDst = *pSrc;
    pDst->fChecked = 1;
    pDst->pPath = NULL;
    pDst->pPathArrival = NULL;
    pDst->nPath = 0;
    pBest->CaptureClock = pCandidate->CaptureClock;
    /* 候选自己带路径快照（找次优路径时逐次尝试算出来的）就直接搬过来，
       否则按当前的前驱重新回溯一条。 */
    if ( pCandidate->fPathSaved )
    {
        if ( pSrc->nPath > 0 && pSrc->pPath != NULL && pSrc->pPathArrival != NULL )
        {
            pDst->pPath = (int *)malloc( (size_t)pSrc->nPath * sizeof(int) );
            pDst->pPathArrival = (double *)malloc( (size_t)pSrc->nPath * sizeof(double) );
            assert( pDst->pPath && pDst->pPathArrival );
            memcpy( pDst->pPath, pSrc->pPath, (size_t)pSrc->nPath * sizeof(int) );
            memcpy( pDst->pPathArrival, pSrc->pPathArrival, (size_t)pSrc->nPath * sizeof(double) );
            pDst->nPath = pSrc->nPath;
        }
        pBest->fPathSaved = 1;
        return;
    }
    int Edge = pSrc->fDataRise;
    for ( n = pBest->nEndNet; n >= 0 && Count < p->pDes->vNets.nSize; )
    {
        MstaPrev Prev = (Edge ? pCorner->pPrevRise : pCorner->pPrevFall)[n];
        Count++;
        n = Prev.Net;
        Edge = Prev.Edge;
    }
    pDst->pPath = (int *)malloc( (size_t)Count * sizeof(int) );
    pDst->pPathArrival = (double *)malloc( (size_t)Count * sizeof(double) );
    assert( pDst->pPath && pDst->pPathArrival );
    pDst->nPath = Count;
    Edge = pSrc->fDataRise;
    for ( n = pBest->nEndNet, i = 0; i < Count; i++ )
    {
        MstaPrev Prev = (Edge ? pCorner->pPrevRise : pCorner->pPrevFall)[n];
        pDst->pPath[i] = n;
        pDst->pPathArrival[i] = (Edge ? pCorner->pArrRise : pCorner->pArrFall)[n];
        n = Prev.Net;
        Edge = Prev.Edge;
    }
}

/* 使能脚不是时钟的锁存器不建模（端点会落到"未约束"里），把条数说出来。 */
static void WarnUntimedLatches( MstaTiming *p )
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
        pInst = InstAt( p, pCheck->InstId );
        if ( !pInst->pCell->fLatch )
            continue;
        pReg = MstaRegCheckArrayAt( &pInst->pCell->vRegs, pCheck->nCheck );
        nClkNet = InstPinNet( p, pCheck->InstId, pReg->ClkPin );
        if ( nClkNet >= 0 && !p->pfClockNet[nClkNet] )
            n++;
    }
    if ( n > 0 )
        Msta_WarnOnce( "%d latch(es) have an enable that is not a clock net; "
                       "they are reported as unconstrained", n );
}

static int IsStartForClock( MstaTiming *p, int nNet, int nClock )
{
    MstaNet *pNet = MstaNetArrayAt(&p->pDes->vNets,nNet);
    if ( p->pfClockNet[nNet] ) return 0;
    if ( pNet->Driver.InstId == MSTA_NO_ID )
    {
        return Msta_IsSet(Msta_SdcPortDelay(p->pSdc,nNet,nClock,0,1)) ||
               Msta_IsSet(Msta_SdcPortDelay(p->pSdc,nNet,nClock,0,0));
    }
    {
        MstaInst *pInst = InstAt(p,pNet->Driver.InstId);
        MstaPin *pPin;
        int i;
        if ( !pInst->pCell->fSequential ) return 0;
        pPin = MstaPinArrayAt(&pInst->pCell->vPins,pNet->Driver.PinId);
        for ( i = 0; i < pInst->pCell->vRegs.nSize; i++ )
        {
            MstaRegCheck *pReg = MstaRegCheckArrayAt(&pInst->pCell->vRegs,i);
            int nClkNet, fClkRises;
            if ( RegClkToQArc(p,pInst->pCell,pReg,pPin->Name,&fClkRises,1,pNet->Driver.InstId) == NULL &&
                 RegClkToQArc(p,pInst->pCell,pReg,pPin->Name,&fClkRises,0,pNet->Driver.InstId) == NULL ) continue;
            nClkNet = InstPinNet(p,pNet->Driver.InstId,pReg->ClkPin);
            if ( nClkNet >= 0 && NetHasDataClock(p,nClock,nClkNet) &&
                 ClockArrAt(p,nClock,nClkNet)->fReached ) return 1;
        }
    }
    return 0;
}

/* 设计规则检查：max_transition / max_capacitance / max_fanout。
   只有 SDC 里设过限制才统计；时钟网络用时钟传播得到的摆率。 */
static void CheckDesignRules( MstaTiming *p )
{
    MstaSdc *pSdc = p->pSdc;
    int n;
    p->nDrcTransitionViol = p->nDrcCapacitanceViol = p->nDrcFanoutViol = 0;
    p->WorstDrcTransition = p->WorstDrcCapacitance = 0.0;
    p->WorstDrcMinCapacitance = 0.0;
    p->WorstDrcFanout = 0;
    if ( !Msta_SdcHasDrcLimits(pSdc) )
        return;
    for ( n = 0; n < p->pDes->vNets.nSize; n++ )
    {
        MstaNet *pNet = MstaNetArrayAt( &p->pDes->vNets, n );
        MstaNetCons *pCons = Msta_SdcNetCons( pSdc, n );
        double Limit, Slew, Cap, Margin;
        int Fanout;
        if ( pNet->fConst )
            continue;
        /* 分对象 SDC 覆盖全局 SDC，再与库里驱动脚上的限制取更紧的。 */
        Limit = ( pCons && Msta_IsSet(pCons->DrcMaxTransition) )
              ? pCons->DrcMaxTransition : pSdc->MaxTransition;
        if ( pNet->Driver.InstId != MSTA_NO_ID )
        {
            MstaPin *pPin = NetDriverPin( p, pNet, 1 );
            if ( pPin && Msta_IsSet(pPin->MaxSlew) &&
                 ( !Msta_IsSet(Limit) || pPin->MaxSlew < Limit ) )
                Limit = pPin->MaxSlew;
        }
        Slew = p->CornerMax.pSlew[n];
        /* 时钟网络上的摆率来自时钟传播；一根网络挂多个时钟时取最差的那个。 */
        if ( p->pfClockNet[n] )
        {
            int c;
            for ( c = 0; c < Msta_SdcClockCount(pSdc); c++ )
            {
                double ClkSlew = NetHasClock(p,c,n) ? ClockArrAt(p,c,n)->MaxSlew : 0.0;
                if ( ClkSlew > Slew ) Slew = ClkSlew;
            }
        }
        if ( Msta_IsSet(Limit) && Slew > Limit )
        {
            Margin = Slew - Limit;
            p->nDrcTransitionViol++;
            if ( Margin > p->WorstDrcTransition ) p->WorstDrcTransition = Margin;
        }
        Limit = ( pCons && Msta_IsSet(pCons->DrcMaxCapacitance) )
              ? pCons->DrcMaxCapacitance : pSdc->MaxCapacitance;
        if ( pNet->Driver.InstId != MSTA_NO_ID )
        {
            MstaPin *pPin = NetDriverPin( p, pNet, 1 );
            if ( pPin && Msta_IsSet(pPin->MaxCap) &&
                 ( !Msta_IsSet(Limit) || pPin->MaxCap < Limit ) )
                Limit = pPin->MaxCap;
        }
        Cap = NetLoadRaw( p, n, 1, 0 );
        if ( Msta_IsSet(Limit) && Cap > Limit )
        {
            Margin = Cap - Limit;
            p->nDrcCapacitanceViol++;
            if ( Margin > p->WorstDrcCapacitance ) p->WorstDrcCapacitance = Margin;
        }
        /* 最小电容：负载低于限制就是违例。限制同上，"更紧"在这里是更大。 */
        Limit = ( pCons && Msta_IsSet(pCons->DrcMinCapacitance) )
              ? pCons->DrcMinCapacitance : pSdc->MinCapacitance;
        if ( pNet->Driver.InstId != MSTA_NO_ID )
        {
            MstaPin *pPin = NetDriverPin( p, pNet, 0 );
            if ( pPin && Msta_IsSet(pPin->MinCap) &&
                 ( !Msta_IsSet(Limit) || pPin->MinCap > Limit ) )
                Limit = pPin->MinCap;
        }
        if ( Msta_IsSet(Limit) && Cap < Limit )
        {
            Margin = Limit - Cap;
            p->nDrcMinCapacitanceViol++;
            if ( Margin > p->WorstDrcMinCapacitance ) p->WorstDrcMinCapacitance = Margin;
        }
        Limit = ( pCons && Msta_IsSet(pCons->DrcMaxFanout) )
              ? pCons->DrcMaxFanout : pSdc->MaxFanout;
        Fanout = pNet->vLoads.nSize;
        if ( Msta_IsSet(Limit) && (double)Fanout > Limit )
        {
            p->nDrcFanoutViol++;
            if ( Fanout > p->WorstDrcFanout ) p->WorstDrcFanout = Fanout;
        }
    }
}

/* set_data_check：两条数据路径之间的检查。-from 的到达减去它自己的出发沿就是
   "时钟沿到数据"的延迟，加到 -to 那条路径的出发沿上作为要求时间；
   两条路径必须由同一个时钟出发（跨时钟的数据检查未建模，直接跳过）。 */
static void CheckDataChecks( MstaTiming *p, int nClock )
{
    int i;
    if ( Msta_SdcDataCheckCount(p->pSdc) == 0 )
        return;
    PropagateData( p, 1, nClock, -1 );
    PropagateData( p, 0, nClock, -1 );
    for ( i = 0; i < Msta_SdcDataCheckCount(p->pSdc); i++ )
    {
        MstaDataCheck *pCheck = Msta_SdcDataCheckByIndex( p->pSdc, i );
        int nFrom = pCheck->FromNet, nTo = pCheck->ToNet;
        double FromDelay, Required, Slack;
        if ( !ArrivalSet(p->CornerMax.pArr[nFrom],1) || !ArrivalSet(p->CornerMax.pArr[nTo],1) ||
             !ArrivalSet(p->CornerMin.pArr[nFrom],0) || !ArrivalSet(p->CornerMin.pArr[nTo],0) )
            continue;
        if ( p->CornerMax.pnLaunchClock[nFrom] != nClock || p->CornerMax.pnLaunchClock[nTo] != nClock ||
             p->CornerMin.pnLaunchClock[nFrom] != nClock || p->CornerMin.pnLaunchClock[nTo] != nClock )
            continue;                   /* 这个 pass 不是这两条路径的出发时钟 */
        p->nDataChecks++;
        if ( pCheck->fSetup )
        {
            FromDelay = p->CornerMax.pArr[nFrom] - p->CornerMax.pdLaunchEdge[nFrom];
            Required  = p->CornerMax.pdLaunchEdge[nTo] + FromDelay - pCheck->Value
                      - ClockUncertainty( p, p->CornerMax.pnLaunchClock[nFrom],
                                          p->CornerMax.pnLaunchClock[nTo], 1,
                                          p->CornerMax.pfLaunchRises[nFrom],
                                          p->CornerMax.pfLaunchRises[nTo] );
            Slack     = Required - p->CornerMax.pArr[nTo];
            if ( Slack < p->WorstDataCheckSetupSlack )
            {
                p->WorstDataCheckSetupSlack = Slack;
                p->DataCheckSetupFrom  = pCheck->FromText;
                p->DataCheckSetupTo    = pCheck->ToText;
                p->DataCheckSetupValue = pCheck->Value;
            }
        }
        if ( pCheck->fHold )
        {
            /* set_data_check 只建模 setup（max 角）；-hold 一侧未实现，遇到时只告警一次、
               不计入结果。 */
            Msta_WarnOnce( "set_data_check -hold is not modeled; only the setup "
                           "corner of a data check is checked" );
        }
    }

}

/* 时钟门控检查：门控单元（ICG）的使能脚要相对时钟的有效沿稳定。
   数据路径 = 使能脚，时钟路径 = 门控单元的时钟脚。检查值优先取 SDC
   （set_clock_gating_check），没有就查库里使能脚的约束弧，两者都没有不查。 */
static int CheckClockGatingOne( MstaTiming *p, int nInst, MstaGateCheck *pGate, int nClock )
{
    MstaInst *pInst = InstAt( p, nInst );
    int nClkNet  = InstPinNet( p, nInst, pGate->ClkPin );
    int nEnNet   = InstPinNet( p, nInst, pGate->EnablePin );
    MstaClockArr *pClkArr;
    double Value, Check, Capture, Required, Slack, Uncertainty;
    int fSet, nLaunchClock, fClkRises, nLaunchCycle = 0, Edge, nStartMax, nStartMin;

    if ( nClkNet < 0 || nEnNet < 0 || !NetHasDataClock(p,nClock,nClkNet) )
        return 0;
    pClkArr = ClockArrAt( p, nClock, nClkNet );
    if ( !pClkArr->fReached )
        return 0;
    /* 使能必须也是这个时钟域的路径（跨域的门控检查不建模）。 */
    if ( p->CornerMax.pnLaunchClock[nEnNet] != nClock || p->CornerMin.pnLaunchClock[nEnNet] != nClock )
        return 0;

    /* 捕获沿按当前源边沿标签取；它传不到库定义的本地沿（pGate->fClkRises）时不查。 */
    fClkRises = p->CaptureClockTag;
    nLaunchClock = nClock;

    for ( Edge = 1; Edge >= 0; Edge-- )
    {
        nStartMax = TimingStartNetEdge(p,nEnNet,1,Edge,NULL);
        nStartMin = TimingStartNetEdge(p,nEnNet,0,Edge,NULL);
        Msta_SdcClockGatingValue( p->pSdc, nInst, 1, &Value, &fSet );
        if ( (fSet || (pGate->SetupArc != MSTA_NO_ID &&
             CheckHasEdge(p, Msta_CellArcById(pInst->pCell,pGate->SetupArc),
                          nInst, 1, Edge))) &&
             ClockTagValid(pClkArr,0,pGate->fClkRises,fClkRises) &&
             ArrivalSet(Edge ? p->CornerMax.pArrRise[nEnNet] : p->CornerMax.pArrFall[nEnNet],1) )
        {
            MstaArc *pArc = ( pGate->SetupArc != MSTA_NO_ID )
                          ? Msta_CellArcById( pInst->pCell, pGate->SetupArc ) : NULL;
            Check = fSet ? Value : CheckTime( p, pArc, nInst, nClock, nClkNet, nEnNet, 1, Edge );
            Capture = CaptureEdgeTime( p, 1, nClkNet, -1, nLaunchClock, p->CornerMax.pfLaunchRises[nStartMax],
                                       nClock, fClkRises, 1, 1, &nLaunchCycle, pGate->fClkRises );
            Uncertainty = ClockUncertainty( p, nLaunchClock, nClock, 1,
                                            p->CornerMax.pfLaunchRises[nStartMax], fClkRises );
            Required = Capture - Uncertainty - Check;
            Slack = Required - (Edge ? p->CornerMax.pArrRise[nEnNet] : p->CornerMax.pArrFall[nEnNet]);
            if ( Slack < p->WorstClkGatingSetupSlack )
            {
                p->WorstClkGatingSetupSlack = Slack;
                p->ClkGatingSetupInst = nInst;
                p->ClkGatingSetupPin  = pGate->EnablePin;
                p->ClkGatingSetupValue= Check;
            }
        }
        Msta_SdcClockGatingValue( p->pSdc, nInst, 0, &Value, &fSet );
        if ( (fSet || (pGate->HoldArc != MSTA_NO_ID &&
             CheckHasEdge(p, Msta_CellArcById(pInst->pCell,pGate->HoldArc),
                          nInst, 0, Edge))) &&
             ClockTagValid(pClkArr,1,pGate->fClkRises,fClkRises) &&
             ArrivalSet(Edge ? p->CornerMin.pArrRise[nEnNet] : p->CornerMin.pArrFall[nEnNet],0) )
        {
            MstaArc *pArc = ( pGate->HoldArc != MSTA_NO_ID )
                          ? Msta_CellArcById( pInst->pCell, pGate->HoldArc ) : NULL;
            Check = fSet ? Value : CheckTime( p, pArc, nInst, nClock, nClkNet, nEnNet, 0, Edge );
            Capture = CaptureEdgeTime( p, 1, nClkNet, -1, nLaunchClock, p->CornerMin.pfLaunchRises[nStartMin],
                                       nClock, fClkRises, 1, 0, &nLaunchCycle, pGate->fClkRises );
            Uncertainty = ClockUncertainty( p, nLaunchClock, nClock, 0,
                                            p->CornerMin.pfLaunchRises[nStartMin], fClkRises );
            Required = Capture + Uncertainty + Check;
            Slack = (Edge ? p->CornerMin.pArrRise[nEnNet] : p->CornerMin.pArrFall[nEnNet]) - Required;
            if ( Slack < p->WorstClkGatingHoldSlack )
            {
                p->WorstClkGatingHoldSlack = Slack;
                p->ClkGatingHoldInst = nInst;
                p->ClkGatingHoldPin  = pGate->EnablePin;
                p->ClkGatingHoldValue= Check;
            }
        }
    }
    return 1;
}

/* 每个 launch 相位分别传播，门控检查计数仍按单元/捕获时钟统计。 */
static void CheckClockGating( MstaTiming *p, int nClock, int nStartClasses )
{
    int i, j, c, k, Tag, CaptureTag, nGates = 0;
    char *pSeen;
    for ( i = 0; i < p->pDes->vInsts.nSize; i++ )
        nGates += InstAt(p,i)->pCell->vGates.nSize;
    if ( nGates == 0 )
    {
        PropagateData(p,1,nClock,-1);
        PropagateData(p,0,nClock,-1);
        return;
    }
    pSeen = (char *)calloc((size_t)nGates, 1);
    assert(pSeen);
    for ( Tag = 0; Tag < 2; Tag++ )
    for ( c = 0; c < nStartClasses; c++ )
    {
        p->LaunchClockTag = Tag;
        PropagateData(p,1,nClock,c);
        PropagateData(p,0,nClock,c);
        for ( CaptureTag = 0; CaptureTag < 2; CaptureTag++ )
        {
            p->CaptureClockTag = CaptureTag;
            for ( i = 0, k = 0; i < p->pDes->vInsts.nSize; i++ )
            {
                MstaInst *pInst = InstAt(p,i);
                for ( j = 0; j < pInst->pCell->vGates.nSize; j++, k++ )
                    if ( CheckClockGatingOne(p,i,MstaGateCheckArrayAt(&pInst->pCell->vGates,j),nClock)
                         && !pSeen[k] )
                    {
                        pSeen[k] = 1;
                        p->nClkGatingChecks++;
                    }
            }
        }
    }
    p->LaunchClockTag = p->CaptureClockTag = -1;
    free(pSeen);
    /* DRC 和网络查询需要所有起点的传播结果，不能留在最后一个分组。 */
    PropagateData(p,1,nClock,-1);
    PropagateData(p,0,nClock,-1);
}

/* qsort 的比较函数没有上下文参数，起点签名经这几个静态变量传进去。 */
static const int *s_pSortSig, *s_pSortSigBeg, *s_pSortSigLen, *s_pSortPhase;
static int s_nSortClocks;

/* 比较两个起点的签名：先比每个时钟下的出发相位，再比命中的 -from 例外组列表。 */
static int CompareStartSignature( const void *pA, const void *pB )
{
    int a = *(const int *)pA, b = *(const int *)pB, k;
    for ( k = 0; k < s_nSortClocks; k++ )
    {
        int x = s_pSortPhase[(size_t)a * s_nSortClocks + k];
        int y = s_pSortPhase[(size_t)b * s_nSortClocks + k];
        if ( x != y ) return x < y ? -1 : 1;
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

/* 起点分类：给每个起点算签名（各时钟下的出发相位 + 命中的 -from 例外组），签名相同
   的归一类，返回类数。前向传播在汇聚点只留最差到达；出发沿或 -from 例外不同的起点
   混在一起传，留下的那条可能被例外切掉、或因出发沿不同而 slack 并非最差，从而盖住
   别的候选。按类分开传可避免这点，同类起点又共用一遍传播，不必每个起点各传一遍。 */
static int BuildStartClasses( MstaTiming *p )
{
    int nNets = p->pDes->vNets.nSize;
    int nClocks = Msta_SdcClockCount( p->pSdc );
    int nEx = Msta_SdcExceptionCount( p->pSdc );
    int *pGroups = (int *)malloc( (size_t)(nEx + 1) * sizeof(int) );
    int *pBeg = (int *)calloc( (size_t)(nNets + 1), sizeof(int) );
    int *pLen = (int *)calloc( (size_t)(nNets + 1), sizeof(int) );
    int *pOrder = (int *)malloc( (size_t)(nNets + 1) * sizeof(int) );
    int *pSig = NULL;
    int *pPhase = (int *)calloc((size_t)nNets * nClocks, sizeof(int));
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
            fStart = IsStartForClock( p, n, c );
        if ( !fStart )
            continue;
        StartEndpoint( p, n, &Obj );
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
    /* 只给真正的起点取签名。min/max 可各自参照不同的时钟边沿，所以相位编码为
       max 角值 + 3 × min 角值，每个角取 0 = 不由时钟 c 出发，1 = 下降沿出发，2 = 上升沿出发。 */
    for ( c = 0; c < nClocks; c++ )
    {
        PropagateData(p,1,c,-1);
        PropagateData(p,0,c,-1);
        for ( i = 0; i < nStarts; i++ )
        {
            n = pOrder[i];
            pPhase[(size_t)n * nClocks + c] =
                (p->CornerMax.pnLaunchClock[n] == c ? 1 + p->CornerMax.pfLaunchRises[n] : 0)
                + 3 * (p->CornerMin.pnLaunchClock[n] == c ? 1 + p->CornerMin.pfLaunchRises[n] : 0);
        }
    }
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
    free( pPhase );
    free( pSig );
    free( pOrder );
    free( pLen );
    free( pBeg );
    free( pGroups );
    return nClasses;
}

/* 按 (出发时钟 nClock, 起点分类 nStartClass, 当前 LaunchClockTag) 传播一遍，再把每个
   端点检查一遍，与之前各遍的结果比较保留最差。输出端口端点按每个时钟各当一次捕获时钟试。 */
static void EvaluateCandidatePaths( MstaTiming *p, int nClock, int nStartClass )
{
    int j;
    PropagateData(p,1,nClock,nStartClass);
    PropagateData(p,0,nClock,nStartClass);
    for ( j = 0; j < p->vChecks.nSize; j++ )
    {
        MstaCheck *pBest = MstaCheckArrayAt(&p->vChecks,j);
        int k, nVariants = pBest->fToRegister ? 1 : Msta_SdcClockCount(p->pSdc);
        for ( k = 0; k < nVariants; k++ )
        {
            MstaCheck Candidate;
            memset(&Candidate,0,sizeof(Candidate));
            CopyCheckIdentity( &Candidate, pBest );
            CheckEndpoint(p,&Candidate,pBest->fToRegister ? -1 : k,nClock,nStartClass);
            KeepWorseCandidate( p, pBest, &Candidate );
        }
    }
}

MstaTiming *Msta_TimingStart( MstaDesign *pDes, MstaLib *pLib, MstaSdc *pSdc )
{
    MstaTiming *p = (MstaTiming *)calloc( 1, sizeof(MstaTiming) );
    assert( p );
    p->pDes = pDes;
    p->pLib = pLib;
    p->pSdc = pSdc;
    p->LaunchClockTag = p->CaptureClockTag = -1;
    MstaCheckArrayInit( &p->vChecks );
    return p;
}

void Msta_TimingFree( MstaTiming *p )
{
    if ( p == NULL )
        return;
    FreeNetArrays( p );
    FreeChecks( p );
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

int Msta_TimingAnalyze( MstaTiming *p, int fVerbose )
{
    int i, nStartClasses;
    char *pfClassUsed = NULL;

    if ( p->pDes->vNets.nSize == 0 )
    {
        Msta_Error( "analyze: the design is not flattened (say current_design).\n" );
        return 0;
    }
    if ( Msta_SdcClockCount( p->pSdc ) == 0 )
    {
        Msta_Error( "analyze: no clock defined. Say create_clock first.\n" );
        return 0;
    }

    FreeNetArrays( p );
    AllocNetArrays( p );
    FreeChecks( p );
    MstaCheckArrayInit( &p->vChecks );
    p->nRegisters = 0;
    p->nLatches = 0;

    BuildChecks( p );                  /* 1. 端点 */
    MarkClockNets( p );                /* 2. 时钟树 */
    AttachClockIds( p );
    WarnUntimedLatches( p );
    MarkIdealNets( p );                /* 3. 理想网络（set_ideal_network） */
    BuildTopoOrder( p );               /* 4. 拓扑序 */
    PropagateClocks( p );              /* 5. 时钟传播 */
    /* 6. 数据到达与检查 */
    p->nDataChecks = 0;
    p->WorstDataCheckSetupSlack = MSTA_NO_TIME;
    p->nClkGatingChecks = 0;
    p->WorstClkGatingSetupSlack = p->WorstClkGatingHoldSlack = MSTA_NO_TIME;
    p->ClkGatingSetupInst = p->ClkGatingHoldInst = -1;
    p->LaunchClockTag = p->CaptureClockTag = -1;
    /* 各时钟、launch 相位和起点例外分类分别传播，汇聚点不会隐藏其他候选。 */
    nStartClasses = BuildStartClasses(p);
    pfClassUsed = (char *)malloc((size_t)(nStartClasses + 1));
    assert(pfClassUsed);
    for ( i = 0; i < Msta_SdcClockCount(p->pSdc); i++ )
    {
        int n, c;
        memset(pfClassUsed,0,(size_t)(nStartClasses + 1));
        for ( n = 0; n < p->pDes->vNets.nSize; n++ )
            if ( p->pStartClass[n] >= 0 && !pfClassUsed[p->pStartClass[n]] &&
                 IsStartForClock(p,n,i) )
                pfClassUsed[p->pStartClass[n]] = 1;
        for ( c = 0; c < nStartClasses; c++ )
            if ( pfClassUsed[c] )
                {
                    int Tag;
                    for ( Tag = 0; Tag < 2; Tag++ )
                    {
                        p->LaunchClockTag = Tag;
                        EvaluateCandidatePaths(p,i,c);
                    }
                }
        p->LaunchClockTag = -1;
        CheckDataChecks(p,i);
        CheckClockGating(p,i,nStartClasses);
    }
    free( pfClassUsed );

    /* 7. 汇总：WNS/TNS（含数据检查、门控检查）、面积、DRC */
    p->WorstSetupSlack = p->WorstHoldSlack = MSTA_NO_TIME;
    p->TotalSetupSlack = p->TotalHoldSlack = 0.0;
    p->nSetupViolations = p->nHoldViolations = 0;
    p->nUnconstrainedEnds = p->nExcludedEnds = 0;
    if ( Msta_IsSet(p->WorstDataCheckSetupSlack) &&
         p->WorstDataCheckSetupSlack < p->WorstSetupSlack )
        AddCheckSlack( p->WorstDataCheckSetupSlack, &p->WorstSetupSlack,
                       &p->TotalSetupSlack, &p->nSetupViolations );
    /* 时钟门控检查也是设计里的时序检查，最差的那条同样计入 WNS/TNS。 */
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
    if ( fVerbose )
        Msta_Info( "analyze: %d registers, %d endpoints, %d nets in topo order, %d clock nets, %d loops\n",
                   p->nRegisters, p->vChecks.nSize, p->nTopoOrder,
                   Msta_TimingClockNetCount( p ), p->nCombLoops );
    /* 设计面积：所有实例的单元面积之和，set_max_area 拿它做检查。 */
    p->DesignArea = 0.0;
    for ( i = 0; i < p->pDes->vInsts.nSize; i++ )
        p->DesignArea += MstaInstArrayAt( &p->pDes->vInsts, i )->pCell->Area;
    CheckDesignRules( p );
    if ( fVerbose && Msta_SdcHasDrcLimits(p->pSdc) )
        Msta_Info( "drc: %d max_transition, %d max_capacitance, %d max_fanout violations\n",
                   p->nDrcTransitionViol, p->nDrcCapacitanceViol, p->nDrcFanoutViol );
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
        qsort( p->vChecks.pData, (size_t)p->vChecks.nSize, sizeof(MstaCheck), CompareSlack );
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
    return NetHasClock( p, nClock, nNet );
}

const MstaClockArr *Msta_TimingClockArr( MstaTiming *p, int nClock, int nNet )
{
    return ClockArrAt( p, nClock, nNet );
}

/* 回溯：从终点网络一路走到起点，返回链上的网络数（数组由调用方给）。 */
int Msta_TimingTracePath( MstaTiming *p, MstaCheck *pCheck, int fMax, int *pnNets, int nCap )
{
    const MstaCheckCorner *pCorner = Msta_CheckCorner( pCheck, fMax );
    int *pSaved = pCorner->pPath;
    int nSaved = pCorner->nPath;
    int n = pCheck->nEndNet, k = 0, nGuard = 0;
    int *pPrev = CornerOf(p,fMax)->pPrevNet;
    if ( pSaved )
    {
        int nCopy = nSaved < nCap ? nSaved : nCap;
        memcpy(pnNets,pSaved,(size_t)nCopy*sizeof(int));
        return nCopy;
    }
    while ( n >= 0 && k < nCap )
    {
        pnNets[k++] = n;
        if ( nGuard++ > p->pDes->vNets.nSize )
        {
            Msta_WarnOnce( "path backtrace hit its guard (loop?)" );
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
    return CornerOf(p,fMax)->pArr[nNet];
}
