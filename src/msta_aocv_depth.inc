/**CFile***************************************************************

  FileName    [msta_aocv_depth.inc]

  Synopsis    [AOCV 图深度：端点标记、最短前向级数与最短后向级数。]

  在 msta_timing.c 的拓扑排序之后包含，复用其静态弧/网络查询函数。
  F 是任意起点到当前网络的最短级数，B 是当前网络到任意端点的最短
  级数；一条弧的完整上下文是 F[In] + 1 + B[Out]，不是当前级的位置。

***********************************************************************/

/* 返回 -1=不可遍历，0=未建模的零延迟连接，1=普通单元弧。 */
static int AocvCombStep( MstaTiming *p, int nInst, int nInPin, int nOutPin, int fClock )
{
    MstaInst *pInst = InstAt( p, nInst );
    MstaPin *pIn = MstaPinArrayAt( &pInst->pCell->vPins, nInPin );
    MstaPin *pOut = MstaPinArrayAt( &pInst->pCell->vPins, nOutPin );
    MstaArc *pArc;

    if ( pIn->Dir == MSTA_DIR_OUTPUT || Msta_SdcTimingDisabled( p->pSdc, nInst, pIn->Name, pOut->Name ) )
        return -1;
    pArc = TimingCombArc( p, pInst->pCell, pIn->Name, pOut->Name, 1 );
    if ( !pArc )
        pArc = TimingCombArc( p, pInst->pCell, pIn->Name, pOut->Name, 0 );
    if ( !pArc && ( fClock || Msta_CellHasAsyncArc( pInst->pCell, pIn->Name, pOut->Name ) ) )
        return -1;
    return pArc ? 1 : 0;
}

/* 数据图排除时钟网络；时钟图只允许属于当前时钟的网络。 */
static int AocvNetMatchesGraph( MstaTiming *p, int nClock, int nNet )
{
    return nClock >= 0 ? NetHasClock( p, nClock, nNet ) : !p->pfClockNet[nNet];
}

/* 理想网络与理想时钟不累计弧级数，但禁用/不存在的时钟弧仍不可遍历。 */
static int AocvEffectiveStep( MstaTiming *p, MstaClock *pClock, int nNet, int nInst,
                              int nInPin, int nOutPin )
{
    int nStep = AocvCombStep( p, nInst, nInPin, nOutPin, pClock != NULL );
    if ( nStep >= 0 && ( p->pfIdealNet[nNet] || ( pClock && !pClock->fPropagated ) ) )
        return 0;
    return nStep;
}

/* 同一时钟下，有效 I/O delay 的参考引脚也是时钟深度端点。 */
static void MarkAocvIoEndpoints( MstaTiming *p, int nClock, MstaClock *pClock,
                                MstaIoDelayArray *pDelays, int *pBackward )
{
    int i;
    for ( i = 0; i < pDelays->nSize; i++ )
    {
        MstaIoDelay *pDelay = MstaIoDelayArrayAt( pDelays, i );
        if ( pDelay->Clock != MSTA_NO_ID && pDelay->Clock != pClock->Name )
            continue;
        if ( Msta_IsSet( pDelay->Max ) && NetHasClock( p, nClock, pDelay->RefNetMax ) )
            pBackward[pDelay->RefNetMax] = 0;
        if ( Msta_IsSet( pDelay->Min ) && NetHasClock( p, nClock, pDelay->RefNetMin ) )
            pBackward[pDelay->RefNetMin] = 0;
    }
}

/* 寄存器、异步检查、门控时钟脚和生成时钟源构成时钟图的边界。 */
static void MarkAocvClockEndpoints( MstaTiming *p, int nClock, MstaClock *pClock, int *pBackward )
{
    int i, k;
    for ( i = 0; i < p->pDes->vInsts.nSize; i++ )
    {
        MstaCell *pCell = InstAt( p, i )->pCell;
        for ( k = 0; k < pCell->vRegs.nSize; k++ )
        {
            int nNet = InstPinNet( p, i, MstaRegCheckArrayAt( &pCell->vRegs, k )->ClkPin );
            if ( NetHasClock( p, nClock, nNet ) )
                pBackward[nNet] = 0;
        }
        for ( k = 0; k < pCell->vAsync.nSize; k++ )
        {
            int nNet = InstPinNet( p, i, MstaAsyncCheckArrayAt( &pCell->vAsync, k )->ClkPin );
            if ( NetHasClock( p, nClock, nNet ) )
                pBackward[nNet] = 0;
        }
        for ( k = 0; k < pCell->vGates.nSize; k++ )
        {
            int nNet = InstPinNet( p, i, MstaGateCheckArrayAt( &pCell->vGates, k )->ClkPin );
            if ( NetHasClock( p, nClock, nNet ) )
                pBackward[nNet] = 0;
        }
    }
    MarkAocvIoEndpoints( p, nClock, pClock, &p->pSdc->vInputDelays, pBackward );
    MarkAocvIoEndpoints( p, nClock, pClock, &p->pSdc->vOutputDelays, pBackward );
    for ( i = 0; i < Msta_SdcClockCount( p->pSdc ); i++ )
    {
        MstaClock *pChild = Msta_SdcClockByIndex( p->pSdc, i );
        /* MasterClock 保存名字 ID，不能直接与时钟下标 nClock 比较。 */
        if ( pChild->MasterClock == pClock->Name && NetHasClock( p, nClock, pChild->SourceNet ) )
            pBackward[pChild->SourceNet] = 0;
    }
}

/* 数据端点包括普通检查、受支持的 setup data check 两侧和门控使能脚。 */
static void MarkAocvDataEndpoints( MstaTiming *p, int *pBackward )
{
    int i, k;
    for ( i = 0; i < p->vChecks.nSize; i++ )
        pBackward[MstaCheckArrayAt( &p->vChecks, i )->nEndNet] = 0;
    for ( i = 0; i < Msta_SdcDataCheckCount( p->pSdc ); i++ )
    {
        MstaDataCheck *pCheck = Msta_SdcDataCheckByIndex( p->pSdc, i );
        if ( !pCheck->fSetup )
            continue; /* hold 数据检查尚未建模 */
        if ( pCheck->FromNet >= 0 )
            pBackward[pCheck->FromNet] = 0;
        if ( pCheck->ToNet >= 0 )
            pBackward[pCheck->ToNet] = 0;
    }
    for ( i = 0; i < p->pDes->vInsts.nSize; i++ )
    {
        MstaCell *pCell = InstAt( p, i )->pCell;
        for ( k = 0; k < pCell->vGates.nSize; k++ )
        {
            int nNet = InstPinNet( p, i, MstaGateCheckArrayAt( &pCell->vGates, k )->EnablePin );
            if ( nNet >= 0 )
                pBackward[nNet] = 0;
        }
    }
}

/* 有 clk-to-Q 的时序输出是新的数据起点：普通输出计 1 级，理想输出计 0 级。 */
static int AocvSequentialStartDepth( MstaTiming *p, MstaInst *pInst, MstaNet *pNet, int nNet )
{
    MstaId OutPin = MstaPinArrayAt( &pInst->pCell->vPins, pNet->Driver.PinId )->Name;
    int k, fClockRise, nDepth = MSTA_AOCV_DEPTH_INF;
    for ( k = 0; k < pInst->pCell->vRegs.nSize; k++ )
    {
        MstaRegCheck *pReg = MstaRegCheckArrayAt( &pInst->pCell->vRegs, k );
        if ( RegClkToQArc( p, pInst->pCell, pReg, OutPin, &fClockRise, 1, pNet->Driver.InstId ) ||
             RegClkToQArc( p, pInst->pCell, pReg, OutPin, &fClockRise, 0, pNet->Driver.InstId ) )
            nDepth = p->pfIdealNet[nNet] ? 0 : 1;
    }
    return nDepth;
}

/* 按拓扑序累积从任意起点出发的最短级数。 */
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
        pInst = InstAt( p, pNet->Driver.InstId );
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

/* 逆拓扑序累积到任意端点的最短级数，遇到数据起点或时钟源停止。 */
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
        pInst = InstAt( p, pNet->Driver.InstId );
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

/* 初始化图边界，再分别传播最短前向与后向级数。nClock=-1 表示数据图。 */
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

/* 数据图一份、每个时钟各一份；未开启 AOCV 时不分配深度数组。 */
static void BuildAocvDepths( MstaTiming *p )
{
    int nNets = p->pDes->vNets.nSize;
    int c, nClocks = Msta_SdcClockCount( p->pSdc );
    if ( !p->pSdc->fAocv || !p->pSdc->vAocv.nSize )
        return;
    p->pAocvDataForward = malloc( (size_t)nNets * sizeof(int) );
    p->pAocvDataBackward = malloc( (size_t)nNets * sizeof(int) );
    p->pAocvClockForward = malloc( (size_t)nNets * nClocks * sizeof(int) );
    p->pAocvClockBackward = malloc( (size_t)nNets * nClocks * sizeof(int) );
    assert( p->pAocvDataForward && p->pAocvDataBackward && p->pAocvClockForward && p->pAocvClockBackward );
    BuildAocvGraphDepth( p, -1, p->pAocvDataForward, p->pAocvDataBackward );
    for ( c = 0; c < nClocks; c++ )
        BuildAocvGraphDepth( p, c, p->pAocvClockForward + (size_t)c * nNets,
                            p->pAocvClockBackward + (size_t)c * nNets );
}
