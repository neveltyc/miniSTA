/* Conservative depth-only GBA: min stages from any start + min stages to any end.
   Not the position of the current cell in a path. Shorter contexts widen monotone tables. */
#define MSTA_DEPTH_INF 1000000000

static int AocvCombStep( MstaTiming *p, int Inst, int InPin, int OutPin, int fClock )
{
    MstaInst *pI=InstAt(p,Inst);
    MstaPin *In=MstaPinArrayAt(&pI->pCell->vPins,InPin);
    MstaPin *Out=MstaPinArrayAt(&pI->pCell->vPins,OutPin);
    if ( In->Dir==MSTA_DIR_OUTPUT || Msta_SdcTimingDisabled(p->pSdc,Inst,In->Name,Out->Name) ) return -1;
    MstaArc *A=TimingCombArc(p,pI->pCell,In->Name,Out->Name,1);
    if ( !A ) A=TimingCombArc(p,pI->pCell,In->Name,Out->Name,0);
    if ( !A && (fClock || Msta_CellHasAsyncArc(pI->pCell,In->Name,Out->Name)) ) return -1;
    return A ? 1 : 0;
}

static void BuildAocvGraphDepth( MstaTiming *p, int Clock, int *F, int *B )
{
    int n=p->pDes->vNets.nSize, q,i,k;
    int fClock=Clock>=0;
    MstaClock *pClock=fClock ? Msta_SdcClockByIndex(p->pSdc,Clock) : NULL;
    for ( i=0; i<n; i++ ) F[i]=B[i]=MSTA_DEPTH_INF;
    if ( fClock )
    {
        if ( pClock->SourceNet>=0 ) F[pClock->SourceNet]=0;
        for ( i=0; i<p->pDes->vInsts.nSize; i++ )
        {
            MstaCell *C=InstAt(p,i)->pCell;
            for ( k=0; k<C->vRegs.nSize; k++ )
            {
                int Net=InstPinNet(p,i,MstaRegCheckArrayAt(&C->vRegs,k)->ClkPin);
                if ( NetHasClock(p,Clock,Net) ) B[Net]=0;
            }
            for ( k=0; k<C->vAsync.nSize; k++ )
            {
                int Net=InstPinNet(p,i,MstaAsyncCheckArrayAt(&C->vAsync,k)->ClkPin);
                if ( NetHasClock(p,Clock,Net) ) B[Net]=0;
            }
            for ( k=0; k<C->vGates.nSize; k++ )
            {
                int Net=InstPinNet(p,i,MstaGateCheckArrayAt(&C->vGates,k)->ClkPin);
                if ( NetHasClock(p,Clock,Net) ) B[Net]=0;
            }
        }
        /* Generated-clock source is a conservative boundary for the master graph. */
        for ( i=0; i<Msta_SdcClockCount(p->pSdc); i++ )
        {
            MstaClock *Child=Msta_SdcClockByIndex(p->pSdc,i);
            if ( Child->MasterClock==pClock->Name && NetHasClock(p,Clock,Child->SourceNet) ) B[Child->SourceNet]=0;
        }
    }
    else
    {
        for ( i=0; i<p->vChecks.nSize; i++ ) B[MstaCheckArrayAt(&p->vChecks,i)->nEndNet]=0;
        for ( i=0; i<p->pDes->vInsts.nSize; i++ )
        {
            MstaInst *I=InstAt(p,i);
            for ( k=0; k<I->pCell->vGates.nSize; k++ )
            {
                int Net=InstPinNet(p,i,MstaGateCheckArrayAt(&I->pCell->vGates,k)->EnablePin);
                if ( Net>=0 ) B[Net]=0;
            }
        }
    }
    for ( q=0; q<p->nTopoOrder; q++ )
    {
        int Net=p->pTopoOrder[q];
        MstaNet *N=MstaNetArrayAt(&p->pDes->vNets,Net);
        if ( N->fCaseValue || N->fConst || (fClock ? !NetHasClock(p,Clock,Net) : p->pfClockNet[Net]) ) continue;
        if ( !fClock && N->Driver.InstId<0 ) { F[Net]=0; continue; }
        if ( fClock && Net==pClock->SourceNet ) continue;
        if ( N->Driver.InstId<0 ) continue;
        MstaInst *I=InstAt(p,N->Driver.InstId);
        if ( I->pCell->fSequential )
        {
            if ( !fClock )
            {
                MstaId Out=MstaPinArrayAt(&I->pCell->vPins,N->Driver.PinId)->Name;
                for ( k=0; k<I->pCell->vRegs.nSize; k++ )
                {
                    int Rise;
                    MstaRegCheck *R=MstaRegCheckArrayAt(&I->pCell->vRegs,k);
                    if ( RegClkToQArc(p,I->pCell,R,Out,&Rise,1,N->Driver.InstId) ||
                         RegClkToQArc(p,I->pCell,R,Out,&Rise,0,N->Driver.InstId) )
                        F[Net]=p->pfIdealNet[Net] ? 0 : 1;
                }
            }
            continue;
        }
        for ( k=0; k<I->nPins && k<I->pCell->vPins.nSize; k++ )
        {
            int In=I->pNets[k], Step=AocvCombStep(p,N->Driver.InstId,k,N->Driver.PinId,fClock);
            if ( In<0 || Step<0 || (fClock ? !NetHasClock(p,Clock,In) : p->pfClockNet[In]) ) continue;
            if ( p->pfIdealNet[Net] || (fClock && !pClock->fPropagated) ) Step=0;
            if ( F[In]+Step<F[Net] ) F[Net]=F[In]+Step;
        }
    }
    for ( q=p->nTopoOrder-1; q>=0; q-- )
    {
        int Net=p->pTopoOrder[q];
        MstaNet *N=MstaNetArrayAt(&p->pDes->vNets,Net);
        if ( B[Net]==MSTA_DEPTH_INF || N->Driver.InstId<0 || N->fCaseValue || N->fConst ||
             (fClock ? !NetHasClock(p,Clock,Net) : p->pfClockNet[Net]) ) continue;
        MstaInst *I=InstAt(p,N->Driver.InstId);
        if ( I->pCell->fSequential || (fClock && Net==pClock->SourceNet) ) continue;
        for ( k=0; k<I->nPins && k<I->pCell->vPins.nSize; k++ )
        {
            int In=I->pNets[k], Step=AocvCombStep(p,N->Driver.InstId,k,N->Driver.PinId,fClock);
            if ( In<0 || Step<0 || (fClock ? !NetHasClock(p,Clock,In) : p->pfClockNet[In]) ) continue;
            if ( p->pfIdealNet[Net] || (fClock && !pClock->fPropagated) ) Step=0;
            if ( B[Net]+Step<B[In] ) B[In]=B[Net]+Step;
        }
    }
}

static void BuildAocvDepths( MstaTiming *p )
{
    int n=p->pDes->vNets.nSize, c, nc=Msta_SdcClockCount(p->pSdc);
    if ( !p->pSdc->fAocv || !p->pSdc->vAocv.nSize ) return;
    p->pAocvDataForward=malloc((size_t)n*sizeof(int));
    p->pAocvDataBackward=malloc((size_t)n*sizeof(int));
    p->pAocvClockForward=malloc((size_t)n*nc*sizeof(int));
    p->pAocvClockBackward=malloc((size_t)n*nc*sizeof(int));
    assert(p->pAocvDataForward && p->pAocvDataBackward && p->pAocvClockForward && p->pAocvClockBackward);
    BuildAocvGraphDepth(p,-1,p->pAocvDataForward,p->pAocvDataBackward);
    for ( c=0; c<nc; c++ )
        BuildAocvGraphDepth(p,c,p->pAocvClockForward+(size_t)c*n,p->pAocvClockBackward+(size_t)c*n);
}
