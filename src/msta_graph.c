/**CFile***************************************************************

  FileName    [msta_graph.c]

  Synopsis    [时序图构建：工艺角单元映射、网络负载、when 条件、理想网络、拓扑排序与逻辑级数。]

***********************************************************************/

#include "msta_timing_int.h"

/* 网络的驱动脚（没有驱动返回 NULL）。 */
MstaPin *Msta_TiNetDriverPin( MstaTiming *p, MstaNet *pNet, int fMax )
{
    MstaInst *pInst;
    if ( pNet->Driver.InstId == MSTA_NO_ID )
        return NULL;
    pInst = Msta_TiInstAt( p, pNet->Driver.InstId );
    return Msta_LibPinForCorner( p->pLib, pInst->pCell, pNet->Driver.PinId,
                                 Msta_SdcOperatingLibrary(p->pSdc, fMax) );
}

/* 取当前 max/min corner 的 cell definition；没有 alternate library cell 时回退默认值。 */
MstaCell *Msta_TiTimingCellCorner( MstaTiming *p, MstaCell *pCell, int fMax )
{
    MstaId Library = Msta_SdcOperatingLibrary( p->pSdc, fMax );
    MstaCell *pCornerCell = Msta_LibCellForCorner( p->pLib, pCell, Library );
    if ( pCell != NULL && Library != MSTA_NO_ID && pCornerCell == pCell &&
         pCell->LibName != Library )
        Msta_WarnOnce( "工艺角库 \"%s\" 缺少设计用到的部分单元；这些单元改用默认库里的定义",
                       Msta_NameStr(Library) );
    return pCornerCell;
}

/* 取当前 corner 的等价 arc。ArcId 来自默认 cell 的结构索引。 */
MstaArc *Msta_TiTimingArcById( MstaTiming *p, MstaCell *pCell, MstaId ArcId, int fMax )
{
    MstaArc *pArc = Msta_LibArcForCorner( p->pLib, pCell, ArcId,
                                          Msta_SdcOperatingLibrary(p->pSdc, fMax) );
    if ( pArc == NULL && pCell != NULL && ArcId >= 0 && ArcId < pCell->vArcs.nSize )
    {
        Msta_WarnOnce( "所选工艺角库 \"%s\" 缺少部分时序弧；缺的弧改用默认单元里的定义",
                       Msta_NameStr( Msta_SdcOperatingLibrary(p->pSdc, fMax) ) );
        pArc = Msta_CellArcById( pCell, ArcId );
    }
    return pArc;
}

/* 取当前 corner 中含指定组合弧的单元变体（缺弧时告警一次并退回默认单元）。 */
MstaCell *Msta_TiTimingCombCell( MstaTiming *p, MstaCell *pCell,
                                 MstaId InPin, MstaId OutPin, int fMax )
{
    MstaCell *pCornerCell = Msta_TiTimingCellCorner( p, pCell, fMax );
    if ( pCornerCell != pCell && Msta_CellCombArc( pCornerCell, InPin, OutPin ) == NULL )
    {
        Msta_WarnOnce( "所选工艺角库 \"%s\" 缺少部分组合弧",
                       Msta_NameStr( Msta_SdcOperatingLibrary(p->pSdc, fMax) ) );
        return pCell;
    }
    return pCornerCell;
}

/* 按当前 corner 在对应 cell variant 上找组合弧。 */
MstaArc *Msta_TiTimingCombArc( MstaTiming *p, MstaCell *pCell,
                               MstaId InPin, MstaId OutPin, int fMax )
{
    return Msta_CellCombArc( Msta_TiTimingCombCell(p, pCell, InPin, OutPin, fMax), InPin, OutPin );
}

/* 将默认库中的弧指针映射到当前 min/max corner 库上的对应弧（未找到则保留原指针）。 */
const MstaArc *Msta_TiTimingArcForCheckCorner( MstaTiming *p, const MstaArc *pArc,
                                               int nInst, int fMax )
{
    if ( pArc != NULL && nInst >= 0 && nInst < p->pDes->vInsts.nSize )
    {
        MstaCell *pCell = Msta_TiInstAt( p, nInst )->pCell;
        int i;
        for ( i = 0; i < pCell->vArcs.nSize; i++ )
            if ( pArc == MstaArcArrayAt( &pCell->vArcs, i ) )
                return Msta_TiTimingArcById( p, pCell, (MstaId)i, fMax );
    }
    return pArc;
}

/* 在网络的 vLoads 列表里查找具体接收引脚 (nInst, nPin) 的下标；未命中返回 -1。 */
int Msta_TiFindNetLoadIdx( MstaTiming *p, int nNet, int nInst, int nPin )
{
    MstaNet *pNet;
    int i;
    if ( nNet < 0 || nNet >= p->pDes->vNets.nSize || nInst < 0 || nPin < 0 )
        return -1;
    pNet = MstaNetArrayAt( &p->pDes->vNets, nNet );
    for ( i = 0; i < pNet->vLoads.nSize; i++ )
    {
        MstaPinRef *pRef = MstaPinRefArrayAt( &pNet->vLoads, i );
        if ( pRef->InstId == nInst && pRef->PinId == nPin )
            return i;
    }
    return -1;
}

/* 一根网络的有效负载（fF）：委托当前线网插件统计引脚电容、线电容和 set_load。
   fFallback=1 时，没有负载的网络用一个默认值顶上（查表用）。 */
double Msta_TiNetLoadRaw( MstaTiming *p, int nNet, int fMax, int fFallback )
{
    return Msta_TiEffCapRaw( p, nNet, fMax, p->pfClockNet ? (int)p->pfClockNet[nNet] : 0, fFallback );
}

double Msta_TiNetLoad( MstaTiming *p, int nNet, int fMax )
{
    return Msta_TiNetLoadRaw( p, nNet, fMax, 1 );
}

void Msta_TimingPinWireDelay( MstaTiming *p, int nNet, int nInst, int nPin,
                              int fMax, int fRise, int fClockPath,
                              double DrvSlew, double *pWireDelay, double *pSinkSlew )
{
    double WireD = 0.0, SinkSlew = DrvSlew;
    if ( p != NULL && nNet >= 0 && nNet < p->pDes->vNets.nSize )
        Msta_TiWireToPin( p, nNet, nInst, nPin, fMax, fRise, fClockPath,
                          DrvSlew, &WireD, &SinkSlew );
    if ( pWireDelay ) *pWireDelay = WireD;
    if ( pSinkSlew )  *pSinkSlew = SinkSlew;
}

/* 找一个输出脚的 clk2q 弧：检查项里记下的 Q 脚优先；同一个寄存器还有另一个
   输出脚（比如网表用的是 QN）时，按"时钟脚 -> 这个输出脚"的边沿弧现找一条。 */
MstaArc *Msta_TiRegClkToQArc( MstaTiming *p, MstaCell *pCell,
                              MstaRegCheck *pReg, MstaId OutPin,
                              int *pfClkRises, int fMax, int nInst )
{
    MstaCell *pCornerCell;
    int i;
    if ( Msta_SdcTimingDisabled( p->pSdc, nInst, pReg->ClkPin, OutPin ) )
        return NULL;
    pCornerCell = Msta_TiTimingCellCorner( p, pCell, fMax );
    *pfClkRises = pReg->fClkRises;
    if ( pReg->QPin == OutPin && pReg->ClkToQArc != MSTA_NO_ID )
        return Msta_TiTimingArcById( p, pCell, pReg->ClkToQArc, fMax );
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

/* 填出实例引脚的 case/常量值（-1=未知，0/1=固定电平），返回是否存在已知脚。 */
int Msta_TiFillInstCaseValues( MstaTiming *p, int nInst, MstaInst *pInst,
                               MstaCell *pEvalCell, signed char *pCases )
{
    int i, fKnown = 0;
    for ( i = 0; i < pEvalCell->vPins.nSize; i++ )
    {
        int n = ( pEvalCell == pInst->pCell )
              ? ( i < pInst->nPins ? pInst->pNets[i] : -1 )
              : Msta_TiInstPinNet( p, nInst, pEvalCell->vPins.pData[i].Name );
        MstaNet *pNet = n >= 0 ? MstaNetArrayAt( &p->pDes->vNets, n ) : NULL;
        int Value = pNet ? ( pNet->fCaseValue ? pNet->fCaseValue : pNet->fConst ) : 0;
        pCases[i] = ( Value == 1 || Value == 2 ) ? (signed char)( Value - 1 ) : -1;
        if ( pCases[i] >= 0 )
            fKnown = 1;
    }
    return fKnown;
}

/* 按侧输入 case/常量值检查数据弧的 when 条件是否仍可能激活。 */
int Msta_TiDataArcWhenActive( MstaTiming *p, int nInst, MstaInst *pInst,
                              MstaCell *pEvalCell, const MstaArc *pArc )
{
    signed char aCases[64];
    signed char *pCases = aCases;
    int fKnown, fActive;
    if ( pArc == NULL || pArc->pWhen == NULL )
        return 1;
    if ( pEvalCell->vPins.nSize > (int)sizeof(aCases) )
    {
        pCases = (signed char *)malloc( (size_t)pEvalCell->vPins.nSize );
        assert( pCases );
    }
    fKnown = Msta_TiFillInstCaseValues( p, nInst, pInst, pEvalCell, pCases );
    fActive = fKnown ? Msta_LibArcWhenActive( pEvalCell, pArc, pCases ) : 1;
    if ( pCases != aCases )
        free( pCases );
    return fActive;
}

/* 正向标记组合扇出网络（供时钟标记与理想网络扩散共用）。 */
void Msta_TiMarkCombFanout( MstaTiming *p, char *pfMark, int *pQueue, int nQueue,
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
            MstaInst *pLoad = Msta_TiInstAt( p, pRef->InstId );
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
                if ( Msta_SdcTimingDisabled( p->pSdc, pRef->InstId, pInPin->Name, pOutPin->Name ) )
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

/* =====================================================================
   3. 理想网络（set_ideal_network）
   ===================================================================== */

void Msta_TiMarkIdealNets( MstaTiming *p )
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

    Msta_TiMarkCombFanout( p, p->pfIdealNet, pQueue, nQueue, 1 );
    free( pQueue );
}

/* =====================================================================
   4. 拓扑序与逻辑级数
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
    pCell = Msta_TiInstAt( p, pNet->Driver.InstId )->pCell;
    if ( pCell->fSequential )
        return 0;
    {
        MstaInst *pDriver = Msta_TiInstAt( p, pNet->Driver.InstId );
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
    MstaInst *pDriver = Msta_TiInstAt( p, pNet->Driver.InstId );
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
void Msta_TiBuildTopoOrder( MstaTiming *p )
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
                    Msta_WarnOnce( "在网络 \"%s\" 处发现组合环路（分析时在这里断开）",
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

/* 按拓扑序计算每根网络的组合逻辑级数（PI/常量/时钟/寄存器输出记为 0 级）。 */
void Msta_TiComputeLogicLevels( MstaTiming *p )
{
    int q;
    for ( q = 0; q < p->nTopoOrder; q++ )
    {
        int nNet = p->pTopoOrder[q];
        MstaNet *pNet = MstaNetArrayAt( &p->pDes->vNets, nNet );
        MstaInst *pDriver;
        MstaCell *pCell;
        MstaPin *pOutPin;
        int k, MaxLvl = 0;

        if ( pNet->Driver.InstId == MSTA_NO_ID || pNet->fConst || pNet->fCaseValue || p->pfClockNet[nNet] )
        {
            p->pLevel[nNet] = 0;
            continue;
        }
        pDriver = Msta_TiInstAt( p, pNet->Driver.InstId );
        pCell = pDriver->pCell;
        if ( pCell->fSequential )
        {
            p->pLevel[nNet] = 0;
            continue;
        }
        pOutPin = MstaPinArrayAt( &pCell->vPins, pNet->Driver.PinId );
        for ( k = 0; k < pDriver->nPins && k < pCell->vPins.nSize; k++ )
        {
            MstaPin *pIn = MstaPinArrayAt( &pCell->vPins, k );
            int nInNet = pDriver->pNets[k], a, m, fHasArc = 0, fAnyComb = 0;
            if ( pIn->Dir == MSTA_DIR_OUTPUT || nInNet < 0 || p->pfClockNet[nInNet] )
                continue;
            if ( Msta_SdcTimingDisabled( p->pSdc, pNet->Driver.InstId, pIn->Name, pOutPin->Name ) )
                continue;
            for ( m = 0; m < 2; m++ )
            {
                MstaCell *pCombCell = Msta_TiTimingCombCell( p, pCell, pIn->Name, pOutPin->Name, m );
                for ( a = 0; a < pCombCell->vArcs.nSize; a++ )
                {
                    MstaArc *pArc = MstaArcArrayAt( &pCombCell->vArcs, a );
                    if ( pArc->InPin != pIn->Name || pArc->OutPin != pOutPin->Name ||
                         !Msta_ArcIsComb( pCombCell, pArc ) )
                        continue;
                    fAnyComb = 1;
                    if ( Msta_TiDataArcWhenActive( p, pNet->Driver.InstId, pDriver, pCombCell, pArc ) )
                        fHasArc = 1;
                }
            }
            if ( !fAnyComb && !Msta_CellHasAsyncArc( pCell, pIn->Name, pOutPin->Name ) )
                fHasArc = 1;
            if ( fHasArc && p->pLevel[nInNet] + 1 > MaxLvl )
                MaxLvl = p->pLevel[nInNet] + 1;
        }
        p->pLevel[nNet] = MaxLvl;
    }
}

double Msta_TimingNetLoad( MstaTiming *p, int nNet, int fMax )
{
    if ( p == NULL || nNet < 0 || nNet >= p->pDes->vNets.nSize )
        return 0.0;
    return Msta_TiNetLoadRaw( p, nNet, fMax, 0 );
}

int Msta_TimingDataArcActive( MstaTiming *p, int nInst, const MstaCell *pCombCell, const MstaArc *pArc )
{
    MstaInst *pInst;
    if ( p == NULL || pCombCell == NULL || pArc == NULL || nInst < 0 || nInst >= p->pDes->vInsts.nSize )
        return 0;
    if ( Msta_SdcTimingDisabled( p->pSdc, nInst, pArc->InPin, pArc->OutPin ) )
        return 0;
    pInst = Msta_TiInstAt( p, nInst );
    return Msta_TiDataArcWhenActive( p, nInst, pInst, (MstaCell *)pCombCell, pArc );
}
