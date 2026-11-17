/**CFile***************************************************************

  FileName    [msta_drc.c]

  Synopsis    [设计规则检查（DRC）：max_transition / max_capacitance / min_capacitance / max_fanout。]

***********************************************************************/

#include "msta_timing_int.h"

/* 设计规则检查：只有 SDC 或 Liberty 设过限制才统计；时钟网络用时钟传播得到的摆率。 */
void Msta_TiCheckDesignRules( MstaTiming *p )
{
    MstaSdc *pSdc = p->pSdc;
    int n;
    p->nDrcTransitionViol = p->nDrcCapacitanceViol = p->nDrcFanoutViol = 0;
    p->nDrcMinCapacitanceViol = 0;
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
            MstaPin *pPin = Msta_TiNetDriverPin( p, pNet, 1 );
            if ( pPin && pPin->MaxSlew >= 0.0 && Msta_IsSet(pPin->MaxSlew) &&
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
                double ClkSlew = Msta_TiNetHasClock(p, c, n)
                               ? Msta_TiClockArrAt(p, c, n)->MaxSlew : 0.0;
                if ( ClkSlew > Slew )
                    Slew = ClkSlew;
            }
        }
        if ( Msta_IsSet(Limit) && Slew > Limit )
        {
            Margin = Slew - Limit;
            p->nDrcTransitionViol++;
            if ( Margin > p->WorstDrcTransition )
                p->WorstDrcTransition = Margin;
        }
        Limit = ( pCons && Msta_IsSet(pCons->DrcMaxCapacitance) )
              ? pCons->DrcMaxCapacitance : pSdc->MaxCapacitance;
        if ( pNet->Driver.InstId != MSTA_NO_ID )
        {
            MstaPin *pPin = Msta_TiNetDriverPin( p, pNet, 1 );
            if ( pPin && pPin->MaxCap >= 0.0 && Msta_IsSet(pPin->MaxCap) &&
                 ( !Msta_IsSet(Limit) || pPin->MaxCap < Limit ) )
                Limit = pPin->MaxCap;
        }
        Cap = Msta_TiNetLoadRaw( p, n, 1, 0 );
        if ( Msta_IsSet(Limit) && Cap > Limit )
        {
            Margin = Cap - Limit;
            p->nDrcCapacitanceViol++;
            if ( Margin > p->WorstDrcCapacitance )
                p->WorstDrcCapacitance = Margin;
        }
        /* 最小电容：负载低于限制就是违例。限制同上，"更紧"在这里是更大。 */
        Limit = ( pCons && Msta_IsSet(pCons->DrcMinCapacitance) )
              ? pCons->DrcMinCapacitance : pSdc->MinCapacitance;
        if ( pNet->Driver.InstId != MSTA_NO_ID )
        {
            MstaPin *pPin = Msta_TiNetDriverPin( p, pNet, 0 );
            if ( pPin && pPin->MinCap >= 0.0 && Msta_IsSet(pPin->MinCap) &&
                 ( !Msta_IsSet(Limit) || pPin->MinCap > Limit ) )
                Limit = pPin->MinCap;
        }
        if ( Msta_IsSet(Limit) && Cap < Limit )
        {
            Margin = Limit - Cap;
            p->nDrcMinCapacitanceViol++;
            if ( Margin > p->WorstDrcMinCapacitance )
                p->WorstDrcMinCapacitance = Margin;
        }
        Limit = ( pCons && Msta_IsSet(pCons->DrcMaxFanout) )
              ? pCons->DrcMaxFanout : pSdc->MaxFanout;
        Fanout = pNet->vLoads.nSize;
        if ( Msta_IsSet(Limit) && (double)Fanout > Limit )
        {
            p->nDrcFanoutViol++;
            if ( Fanout > p->WorstDrcFanout )
                p->WorstDrcFanout = Fanout;
        }
    }
}
