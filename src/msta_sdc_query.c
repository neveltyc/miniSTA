/**CFile***************************************************************

  FileName    [msta_sdc_query.c]

  Synopsis    [SDC 查询：给时序引擎与报告用的公开接口（msta_sdc.h），含例外表索引。]

  约束由 msta_sdc.c 读入、各命令文件写进 MstaSdc，这里只读不改（例外表索引是
  查询时按需建的缓存）。例外索引用到的临时数组是本文件的 static 状态，
  由 Msta_SdcExIndexRelease 释放（msta_sdc.c 的 Msta_SdcFree 调用）。

***********************************************************************/

#include "msta_sdc_int.h"

MstaClock *Msta_SdcFindClock( MstaSdc *p, const char *pName )
{
    int i = Msta_IntMapGet( &p->clockMap, Msta_NameId(pName), -1 );
    return i < 0 ? NULL : MstaClockArrayAt( &p->vClocks, i );
}

MstaClock *Msta_SdcClockByIndex( MstaSdc *p, int i )
{
    return ( i >= 0 && i < p->vClocks.nSize ) ? MstaClockArrayAt( &p->vClocks, i ) : NULL;
}

int Msta_SdcClockCount( MstaSdc *p )
{
    return p->vClocks.nSize;
}

int Msta_SdcClockIndexOf( MstaSdc *p, MstaId NameId )
{
    if ( NameId == MSTA_NO_ID )
        return -1;
    return Msta_IntMapGet( &p->clockMap, NameId, -1 );
}

MstaNetCons *Msta_SdcNetCons( MstaSdc *p, int nNet )
{
    int i = Msta_IntMapGet( &p->netConsMap, nNet, -1 );
    return i < 0 ? NULL : MstaNetConsArrayAt( &p->vNets, i );
}

MstaNetCons *Msta_SdcNetConsOrCreate( MstaSdc *p, int nNet )
{
    MstaNetCons *pCons;
    int i = Msta_IntMapGet( &p->netConsMap, nNet, -1 );
    if ( i >= 0 )
        return MstaNetConsArrayAt( &p->vNets, i );
    pCons = MstaNetConsArrayAppend( &p->vNets );
    pCons->Net     = nNet;
    pCons->LoadMax = pCons->LoadMin = MSTA_UNSET;
    pCons->InputSlewMaxRise = pCons->InputSlewMaxFall = MSTA_UNSET;
    pCons->InputSlewMinRise = pCons->InputSlewMinFall = MSTA_UNSET;
    pCons->DrivingCell = pCons->DrivingPin = pCons->DrivingFromPin = MSTA_NO_ID;
    pCons->DriveInSlewRise = pCons->DriveInSlewFall = MSTA_UNSET;
    pCons->DrcMaxTransition = pCons->DrcMaxCapacitance = pCons->DrcMaxFanout = MSTA_UNSET;
    pCons->DrcMinCapacitance = MSTA_UNSET;
    pCons->IdealLatencyMaxRise = pCons->IdealLatencyMaxFall = MSTA_UNSET;
    pCons->IdealLatencyMinRise = pCons->IdealLatencyMinFall = MSTA_UNSET;
    pCons->IdealTranMaxRise = pCons->IdealTranMaxFall = MSTA_UNSET;
    pCons->IdealTranMinRise = pCons->IdealTranMinFall = MSTA_UNSET;
    pCons->DriveMultiply = 1.0;
    Msta_IntMapSet( &p->netConsMap, nNet, p->vNets.nSize - 1 );
    return pCons;
}

/* set_ideal_network 标过的网络。 */
int Msta_SdcIsIdealNet( MstaSdc *p, int nNet )
{
    MstaNetCons *pCons = Msta_SdcNetCons( p, nNet );
    return pCons != NULL && pCons->fIdeal;
}

/* 该网络的理想属性不往下游传（set_ideal_network -no_propagate）。 */
int Msta_SdcIdealNoPropagate( MstaSdc *p, int nNet )
{
    MstaNetCons *pCons = Msta_SdcNetCons( p, nNet );
    return pCons != NULL && pCons->fIdealNoPropagate;
}

/* 一个理想量的 rise/fall 与 min/max 四个格子：fRise=1/0 取单个边沿，
   fRise=-1 取该分析角最保守的那个（max 角取大、min 角取小）。 */
static double Msta_SdcPickIdeal( double MaxRise, double MaxFall,
                                 double MinRise, double MinFall, int fMax, int fRise )
{
    double Rise = fMax ? MaxRise : MinRise;
    double Fall = fMax ? MaxFall : MinFall;
    if ( fRise == 1 ) return Rise;
    if ( fRise == 0 ) return Fall;
    if ( !Msta_IsSet(Rise) ) return Fall;
    if ( !Msta_IsSet(Fall) ) return Rise;
    return fMax ? ( Rise > Fall ? Rise : Fall ) : ( Rise < Fall ? Rise : Fall );
}

double Msta_SdcIdealLatency( MstaSdc *p, int nNet, int fMax, int fRise )
{
    MstaNetCons *pCons = Msta_SdcNetCons( p, nNet );
    if ( pCons == NULL ) return MSTA_UNSET;
    return Msta_SdcPickIdeal( pCons->IdealLatencyMaxRise, pCons->IdealLatencyMaxFall,
                              pCons->IdealLatencyMinRise, pCons->IdealLatencyMinFall,
                              fMax, fRise );
}

double Msta_SdcIdealTransition( MstaSdc *p, int nNet, int fMax, int fRise )
{
    MstaNetCons *pCons = Msta_SdcNetCons( p, nNet );
    if ( pCons == NULL ) return MSTA_UNSET;
    return Msta_SdcPickIdeal( pCons->IdealTranMaxRise, pCons->IdealTranMaxFall,
                              pCons->IdealTranMinRise, pCons->IdealTranMinFall,
                              fMax, fRise );
}

/* 这根网络上的时钟极性：专门的 (网络, 时钟) 条目优先，其次是对所有时钟生效的条目。 */
int Msta_SdcClockSense( MstaSdc *p, int nNet, int nClock, int *pfStop )
{
    MstaClock *pClock = Msta_SdcClockByIndex( p, nClock );
    MstaClockSense *pAny = NULL;
    int i;
    if ( pfStop ) *pfStop = 0;
    for ( i = 0; i < p->vClockSense.nSize; i++ )
    {
        MstaClockSense *pSense = MstaClockSenseArrayAt( &p->vClockSense, i );
        if ( pSense->Net != nNet )
            continue;
        if ( pSense->Clock == MSTA_NO_ID )
        {
            if ( pAny == NULL ) pAny = pSense;
            continue;
        }
        if ( pClock != NULL && pSense->Clock == pClock->Name )
        {
            if ( pfStop ) *pfStop = pSense->fStop;
            return pSense->Polarity;
        }
    }
    if ( pAny != NULL )
    {
        if ( pfStop ) *pfStop = pAny->fStop;
        return pAny->Polarity;
    }
    return 0;
}

/* SDC 是否点过工作条件（set_operating_conditions 的角名/库，或 set_voltage）。 */
int Msta_SdcOpCondSelected( MstaSdc *p )
{
    return p->OpCondMax != MSTA_NO_ID || p->OpCondMin != MSTA_NO_ID ||
           p->OpCondLibraryMax != MSTA_NO_ID || p->OpCondLibraryMin != MSTA_NO_ID ||
           Msta_IsSet(p->VoltageMax) || Msta_IsSet(p->VoltageMin);
}

MstaId Msta_SdcOperatingLibrary( MstaSdc *p, int fMax )
{
    return fMax ? p->OpCondLibraryMax : p->OpCondLibraryMin;
}

/* 这个角上生效的工艺角：命令选中的名字，否则库里默认的；再取它的电压/温度。 */
void Msta_SdcOpCondInfo( MstaSdc *p, MstaLib *pLib, int fMax,
                         const char **ppName, double *pVoltage, double *pTemp )
{
    MstaId nName = fMax ? p->OpCondMax : p->OpCondMin;
    MstaId nLibrary = fMax ? p->OpCondLibraryMax : p->OpCondLibraryMin;
    MstaOpCond *pCond = NULL;
    MstaLibInfo *pInfo;
    if ( ppName ) *ppName = NULL;
    if ( pVoltage ) *pVoltage = fMax ? p->VoltageMax : p->VoltageMin;
    if ( pTemp ) *pTemp = fMax ? p->TempMax : p->TempMin;
    if ( pLib == NULL )
        return;
    if ( nName != MSTA_NO_ID )
        pInfo = Msta_LibFindOpCond( pLib, Msta_NameStr(nName), nLibrary, &pCond );
    else
        pInfo = Msta_SdcDefaultOpCond( pLib, nLibrary, &pCond );     /* 没选就报库里默认的那个角。 */
    if ( pInfo == NULL )
        return;
    if ( ppName ) *ppName = Msta_NameStr( pCond ? pCond->Name : pInfo->OpCondName );
    if ( pCond != NULL )
    {
        if ( pVoltage && !Msta_IsSet(*pVoltage) && pCond->Voltage >= 0.0 )
            *pVoltage = pCond->Voltage;
        if ( pTemp && !Msta_IsSet(*pTemp) ) *pTemp = pCond->Temperature;
    }
}

int Msta_SdcDataCheckCount( MstaSdc *p )
{
    return p->vDataChecks.nSize;
}

MstaDataCheck *Msta_SdcDataCheckByIndex( MstaSdc *p, int i )
{
    return ( i < 0 || i >= p->vDataChecks.nSize ) ? NULL
                                                  : MstaDataCheckArrayAt(&p->vDataChecks,i);
}

/* 是否设过任何 DRC 限制（全局或分网络）。 */
int Msta_SdcHasDrcLimits( MstaSdc *p )
{
    int i;
    if ( Msta_IsSet(p->MaxTransition) || Msta_IsSet(p->MaxCapacitance) ||
         Msta_IsSet(p->MaxFanout) || Msta_IsSet(p->MinCapacitance) )
        return 1;
    for ( i = 0; i < p->vNets.nSize; i++ )
    {
        MstaNetCons *pCons = MstaNetConsArrayAt( &p->vNets, i );
        if ( Msta_IsSet(pCons->DrcMaxTransition) || Msta_IsSet(pCons->DrcMaxCapacitance) ||
             Msta_IsSet(pCons->DrcMaxFanout) || Msta_IsSet(pCons->DrcMinCapacitance) )
            return 1;
    }
    return 0;
}

/* 一条检查的不确定度：-from/-to 专用条目优先，其余用捕获时钟的标量值。 */
double Msta_SdcClockUncertainty( MstaSdc *p, int nLaunchClock, int nCaptureClock,
                                 int fSetup, int fLaunchRises, int fCaptureRises )
{
    MstaClock *pCapture = Msta_SdcClockByIndex(p,nCaptureClock);
    int i;
    for ( i = 0; i < p->vInterClockUnc.nSize; i++ )
    {
        MstaInterClockUnc *pUnc = MstaInterClockUncArrayAt(&p->vInterClockUnc,i);
        MstaClock *pLaunch = Msta_SdcClockByIndex( p, nLaunchClock );
        MstaId From = pLaunch ? pLaunch->Name : MSTA_NO_ID;
        if ( fSetup ? !pUnc->fSetup : !pUnc->fHold )
            continue;
        if ( pUnc->FromClock != MSTA_NO_ID && pUnc->FromClock != From )
            continue;
        if ( pUnc->ToClock != MSTA_NO_ID && pCapture &&
             pUnc->ToClock != pCapture->Name )
            continue;
        /* 边沿限定只在边沿已知时才生效，未知就退回标量值。 */
        if ( pUnc->FromRF != 0 &&
             ( fLaunchRises < 0 || pUnc->FromRF != (fLaunchRises ? 'r' : 'f') ) )
            continue;
        if ( pUnc->ToRF != 0 &&
             ( fCaptureRises < 0 || pUnc->ToRF != (fCaptureRises ? 'r' : 'f') ) )
            continue;
        return pUnc->Value;
    }
    if ( pCapture == NULL )
        return 0.0;
    return fSetup ? pCapture->UncertaintySetup : pCapture->UncertaintyHold;
}

/* 全局 derate：fMax=1 用 late 系数，fMax=0 用 early 系数。 */
double Msta_SdcDataDerate( MstaSdc *p, int fMax )
{
    if ( !p->fDerateData )
        return fMax ? p->KFactorDerateLate : p->KFactorDerateEarly;
    return ( fMax ? p->DerateLate : p->DerateEarly ) *
           ( fMax ? p->KFactorDerateLate : p->KFactorDerateEarly );
}

double Msta_SdcClockDerate( MstaSdc *p, int fMax )
{
    if ( !p->fDerateClock )
        return fMax ? p->KFactorDerateLate : p->KFactorDerateEarly;
    return ( fMax ? p->DerateLate : p->DerateEarly ) *
           ( fMax ? p->KFactorDerateLate : p->KFactorDerateEarly );
}

double Msta_SdcCheckDerate( MstaSdc *p, int fMax )
{
    return ( fMax ? p->DerateCheckLate : p->DerateCheckEarly ) *
           ( fMax ? p->KFactorDerateLate : p->KFactorDerateEarly );
}

/* 分对象 derate 查表：实例上的按实例号比，时钟上的按时钟名比；
   同一条记录里只写了一个边沿时，另一个边沿不生效。多条形如
   "同一对象写了两条" 时取最后一个（与后写覆盖先写一致）。 */
static int Msta_SdcObjDerateValue( MstaSdc *p, char Kind, int nInst, MstaId Name,
                                   int fCellCheck, int fMax, int fRise, double *pValue )
{
    int i, fFound = 0;
    for ( i = 0; i < p->vObjDerate.nSize; i++ )
    {
        MstaObjDerate *pRec = MstaObjDerateArrayAt( &p->vObjDerate, i );
        if ( pRec->Kind != Kind || pRec->fCellCheck != fCellCheck )
            continue;
        if ( Kind == 'I' ? ( pRec->Inst != nInst ) : ( pRec->Name != Name ) )
            continue;
        if ( fRise >= 0 && ( ( fRise && pRec->fFall ) || ( !fRise && pRec->fRise ) ) )
            continue;                       /* 这条记录只管另一个边沿 */
        *pValue = fMax ? pRec->Late : pRec->Early;
        fFound = 1;
    }
    return fFound;
}

double Msta_SdcDataDerateInst( MstaSdc *p, int nInst, int fMax, int fRise )
{
    double Value;
    if ( Msta_SdcObjDerateValue( p, 'I', nInst, MSTA_NO_ID, 0, fMax, fRise, &Value ) )
        return Value * ( fMax ? p->KFactorDerateLate : p->KFactorDerateEarly );
    return Msta_SdcDataDerate( p, fMax );
}

double Msta_SdcCheckDerateInst( MstaSdc *p, int nInst, int fMax )
{
    double Value;
    if ( Msta_SdcObjDerateValue( p, 'I', nInst, MSTA_NO_ID, 1, fMax, -1, &Value ) )
        return Value * ( fMax ? p->KFactorDerateLate : p->KFactorDerateEarly );
    return Msta_SdcCheckDerate( p, fMax );
}

double Msta_SdcClockDerateClock( MstaSdc *p, int nClock, int fMax )
{
    MstaClock *pClock = Msta_SdcClockByIndex( p, nClock );
    double Value;
    if ( pClock != NULL &&
         Msta_SdcObjDerateValue( p, 'C', -1, pClock->Name, 0, fMax, -1, &Value ) )
        return Value * ( fMax ? p->KFactorDerateLate : p->KFactorDerateEarly );
    return Msta_SdcClockDerate( p, fMax );
}

int Msta_SdcTimingDisabled( MstaSdc *p, int nInst, MstaId FromPin, MstaId ToPin )
{
    int i;
    for ( i = 0; i < p->vDisabledArcs.nSize; i++ )
    {
        MstaDisabledArc *pArc = MstaDisabledArcArrayAt(&p->vDisabledArcs,i);
        if ( pArc->Inst != nInst )
            continue;
        /* MSTA_NO_ID = 这条约束没写那一端，按通配匹配。 */
        if ( pArc->FromPin != MSTA_NO_ID && pArc->FromPin != FromPin )
            continue;
        if ( pArc->ToPin != MSTA_NO_ID && pArc->ToPin != ToPin )
            continue;
        if ( pArc->FromPin != MSTA_NO_ID || pArc->ToPin != MSTA_NO_ID )
            return 1;
    }
    return 0;
}

/* 合并 I/O rise/fall 约束：max 取较大延迟，min 取较小延迟，避免结果依赖命令顺序。 */
static MstaIoDelay *Msta_SdcFindPortDelay( MstaSdc *p, int nNet, int nClock,
                                          int fOutput, int fMax )
{
    MstaIoDelayArray *pArr = fOutput ? &p->vOutputDelays : &p->vInputDelays;
    MstaClock *pClock = Msta_SdcClockByIndex(p,nClock);
    MstaIoDelay *pBest = NULL;
    double Best = MSTA_UNSET;
    int i;
    if ( pClock == NULL ) return NULL;
    for ( i = 0; i < pArr->nSize; i++ )
    {
        MstaIoDelay *pEntry = MstaIoDelayArrayAt(pArr,i);
        if ( pEntry->Net != nNet ) continue;
        if ( pEntry->Clock != pClock->Name &&
             !(pEntry->Clock == MSTA_NO_ID &&
               (p->vClocks.nSize == 1 ||
                (fMax ? pEntry->RefNetMax : pEntry->RefNetMin) >= 0)) )
            continue;
        {
            double Value = fMax ? pEntry->Max : pEntry->Min;
            if ( !Msta_IsSet(Value) ) continue;
            if ( pBest == NULL || (fMax ? Value > Best : Value < Best) )
            { pBest = pEntry; Best = Value; }
        }
    }
    return pBest;
}

double Msta_SdcPortDelay( MstaSdc *p, int nNet, int nClock,
                          int fOutput, int fMax )
{
    MstaIoDelay *pEntry = Msta_SdcFindPortDelay(p,nNet,nClock,fOutput,fMax);
    return pEntry ? (fMax ? pEntry->Max : pEntry->Min) : MSTA_UNSET;
}

int Msta_SdcPortReferenceNet( MstaSdc *p, int nNet, int nClock,
                              int fOutput, int fMax )
{
    MstaIoDelay *pEntry = Msta_SdcFindPortDelay(p,nNet,nClock,fOutput,fMax);
    return pEntry ? (fMax ? pEntry->RefNetMax : pEntry->RefNetMin) : -1;
}

int Msta_SdcPortSourceLatencyIncluded( MstaSdc *p, int nNet, int nClock,
                                       int fOutput, int fMax )
{
    MstaIoDelay *pEntry = Msta_SdcFindPortDelay(p,nNet,nClock,fOutput,fMax);
    return pEntry && (fMax ? pEntry->SourceLatencyIncludedMax
                           : pEntry->SourceLatencyIncludedMin);
}

int Msta_SdcPortNetworkLatencyIncluded( MstaSdc *p, int nNet, int nClock,
                                        int fOutput, int fMax )
{
    MstaIoDelay *pEntry = Msta_SdcFindPortDelay(p,nNet,nClock,fOutput,fMax);
    return pEntry && (fMax ? pEntry->NetworkLatencyIncludedMax
                           : pEntry->NetworkLatencyIncludedMin);
}

int Msta_SdcPortClockFall( MstaSdc *p, int nNet, int nClock, int fOutput, int fMax )
{
    MstaIoDelay *pEntry = Msta_SdcFindPortDelay(p,nNet,nClock,fOutput,fMax);
    return pEntry ? pEntry->ClockFall : 0;
}

/* 名字匹配规则：
     完全相等              -> 命中
     对象名以 "模式/" 开头 -> 命中（可以只写层次路径的前缀）
     * 和 ? 按 glob 展开。 */
static int Msta_SdcNameMatch( const char *pPattern, const char *pText )
{
    size_t n;
    if ( pText == NULL )
        return 0;
    if ( !strcmp( pPattern, pText ) || Msta_SdcGlobMatch(pPattern,pText) )
        return 1;
    n = strlen( pPattern );
    return ( strncmp( pPattern, pText, n ) == 0 && pText[n] == '/' );
}

/* 拼出引脚对象的 "实例/引脚" 全名：放得下就写进 pBuf，否则 malloc 一块新的。 */
static char *Msta_SdcObjectPinPath( MstaDesign *pDes, const MstaSdcObject *pObj,
                                    char *pBuf, size_t nBuf )
{
    MstaInst *pInst = MstaInstArrayAt(&pDes->vInsts,pObj->nInst);
    MstaPin  *pPin  = MstaPinArrayAt(&pInst->pCell->vPins,pObj->nPin);
    const char *pInstName = Msta_InstName(pDes,pObj->nInst);
    const char *pPinName = Msta_NameStr(pPin->Name);
    size_t nInst = strlen( pInstName ), nPin = strlen( pPinName );
    char *pPath = pBuf;
    if ( nInst + nPin + 2 > nBuf )
    {
        pPath = (char *)malloc( nInst + nPin + 2 );
        assert( pPath );
    }
    memcpy( pPath, pInstName, nInst );
    pPath[nInst] = '/';
    memcpy( pPath + nInst + 1, pPinName, nPin + 1 );
    return pPath;
}

/* 把模式匹配到一个端点对象上：时钟只比时钟名，其余比对象名和 "实例/引脚"。
   没写模式（MSTA_NO_ID）= 通配。 */
static int Msta_SdcMatchObject( MstaDesign *pDes, MstaId PatternId, char Kind,
                                const MstaSdcObject *pObj, const char *pClock )
{
    const char *pPattern;
    if ( PatternId == MSTA_NO_ID )
        return 1;
    pPattern = Msta_NameStr( PatternId );
    if ( Kind == 'C' )
        return Msta_SdcNameMatch( pPattern, pClock );
    if ( pObj == NULL )
        return 0;
    if ( Msta_SdcNameMatch( pPattern, pObj->pText ) )
        return 1;
    if ( pObj->nInst >= 0 && pObj->nPin >= 0 )
    {
        char sPath[512];
        char *pPath = Msta_SdcObjectPinPath( pDes, pObj, sPath, sizeof(sPath) );
        int fMatch = Msta_SdcNameMatch( pPattern, pPath );
        if ( pPath != sPath )
            free( pPath );
        if ( fMatch )
            return 1;
    }
    return 0;
}

/* -rise_from/-fall_from/-rise_to/-fall_to：边沿未知时按"不命中"处理并告警，
   宁可保留这条检查，也不要悄悄多切掉一条路径。 */
static int Msta_SdcEdgeMatches( char RF, int fRises )
{
    if ( RF == 0 )
        return 1;
    if ( fRises < 0 )
    {
        Msta_WarnOnce("edge-qualified path exception is not modeled where the edge is unknown; ignored there");
        return 0;
    }
    return RF == ( fRises ? 'r' : 'f' );
}

/* 时钟集合限定参考时钟边沿；引脚/端口集合限定该对象的数据边沿。 */
static int Msta_SdcEndpointEdge( const MstaSdcEndpoint *pEnd, char Kind )
{
    if ( pEnd == NULL ) return -1;
    return Kind == 'C' || pEnd->pObj == NULL ? pEnd->fRises : pEnd->pObj->fRises;
}

/* -through 匹配：每个分组按顺序在路径上找第一个命中的对象，组内取"或"。
   -rise_through/-fall_through 还要求那个点上的信号边沿对上。
   pnHits / pRF 记下每个分组命中的路径对象下标和它的边沿要求（找次优路径时用），
   都可以传 NULL。路径例外与 group_path 共用这一份匹配。 */
static int Msta_SdcMatchThroughList( MstaDesign *pDes, const MstaThruObject *pThru,
                                     int nThru, const MstaSdcObject *pObjects, int nObjects,
                                     int *pnHits, char *pRF, int *pnHitCount )
{
    int i = 0, j = 0;
    int nGroup = 0;
    while ( i < nThru )
    {
        int fMatched = 0;
        int nHit = -1;
        char RF = 0;
        if ( pThru[i].Kind != MSTA_SDC_THRU_SEP )
            return 0;                           /* 表结构坏了，按不匹配处理 */
        i++;                                    /* 跳过分组分隔符 */
        while ( j < nObjects && !fMatched )
        {
            int k;
            for ( k = i; k < nThru && pThru[k].Kind != MSTA_SDC_THRU_SEP; k++ )
                if ( Msta_SdcMatchObject(pDes,pThru[k].Text,pThru[k].Kind,
                                         &pObjects[j], NULL) &&
                     Msta_SdcEdgeMatches(pThru[k].RF, pObjects[j].fRises) )
                { fMatched = 1; nHit = j; RF = pThru[k].RF; break; }
            j++;
        }
        if ( !fMatched )
            return 0;
        if ( pnHits != NULL && nGroup < MSTA_SDC_MAX_THRU )
            pnHits[nGroup] = nHit;
        if ( pRF != NULL && nGroup < MSTA_SDC_MAX_THRU )
            pRF[nGroup] = RF;
        nGroup++;
        while ( i < nThru && pThru[i].Kind != MSTA_SDC_THRU_SEP ) i++;
    }
    if ( pnHitCount != NULL ) *pnHitCount = nGroup;
    return 1;
}

/* 例外表的 -through 匹配，转发给上面那份共用实现。 */
static int Msta_SdcMatchThrough( MstaDesign *pDes, MstaException *pEx,
                                 const MstaSdcObject *pObjects, int nObjects,
                                 int *pnHits, char *pRF, int *pnHitCount )
{
    return Msta_SdcMatchThroughList( pDes, pEx->Thru, pEx->nThru, pObjects, nObjects,
                                     pnHits, pRF, pnHitCount );
}

/* 一条例外是否命中这条路径：端点、边沿、-through 都要对上。 */
static int Msta_SdcExceptionHits( MstaSdc *p, MstaDesign *pDes, MstaException *pEx,
                                  const MstaSdcEndpoint *pFrom, const MstaSdcEndpoint *pTo,
                                  const MstaSdcObject *pObjects, int nObjects )
{
    (void)p;
    if ( !Msta_SdcMatchObject(pDes,pEx->FromText,pEx->FromKind,
                              pFrom ? pFrom->pObj : NULL, pFrom ? pFrom->pClock : NULL) )
        return 0;
    if ( !Msta_SdcMatchObject(pDes,pEx->ToText,pEx->ToKind,
                              pTo ? pTo->pObj : NULL, pTo ? pTo->pClock : NULL) )
        return 0;
    if ( !Msta_SdcEdgeMatches(pEx->FromRF, Msta_SdcEndpointEdge(pFrom,pEx->FromKind)) ||
         !Msta_SdcEdgeMatches(pEx->ToRF, Msta_SdcEndpointEdge(pTo,pEx->ToKind)) )
        return 0;
    return Msta_SdcMatchThrough( pDes, pEx, pObjects, nObjects, NULL, NULL, NULL );
}

static int Msta_SdcCompareInt( int a, int b )
{
    return ( a > b ) - ( a < b );
}

static int Msta_SdcCompareDouble( double a, double b )
{
    return ( a > b ) - ( a < b );
}

/* 例外表索引：每条路径都逐条试全部例外太慢，所以按 -from/-to 的名字文本各建一张哈希，
   只取可能命中的候选（真正是否命中仍由 Msta_SdcExceptionHits 判定）。没写这一侧、通配名、
   时钟例外无法按文本查，放进 always 表，每次都算候选。查名字时在每个 '/' 处的前缀也查一次，
   支持按层次前缀匹配。从两侧候选少的一侧出发，只留另一侧也命中的例外；候选按例外号升序，
   保证后写的例外覆盖先写的。s_pExMark 按例外号记 to 侧(1)/from 侧(2)已命中，用完即清。 */
static MstaSdcIntArray s_vExCandidates, s_vExHitsTo, s_vExHitsFrom, s_vExMerge;
static char *s_pExMark;
static int   s_nExMarkCap;

static unsigned Msta_SdcStrHash( const char *pText, size_t nLen )
{
    unsigned h = 2166136261u;
    size_t i;
    for ( i = 0; i < nLen; i++ )
    {
        h ^= (unsigned char)pText[i];
        h *= 16777619u;
    }
    return h;
}

static void Msta_SdcExIndexFree( MstaSdcExIndex *pIdx )
{
    free( pIdx->pHead );
    free( pIdx->pNext );
    free( pIdx->pAlways );
    free( pIdx->pfAlways );
    memset( pIdx, 0, sizeof(MstaSdcExIndex) );
}

static MstaId Msta_SdcExIndexKey( const MstaException *pEx, int fTo )
{
    MstaId Text = fTo ? pEx->ToText : pEx->FromText;
    char Kind = fTo ? pEx->ToKind : pEx->FromKind;
    if ( Text == MSTA_NO_ID || Kind == 'C' || strpbrk( Msta_NameStr(Text), "*?" ) != NULL )
        return MSTA_NO_ID;
    return Text;
}

static void Msta_SdcExIndexBuild( MstaSdc *p, MstaSdcExIndex *pIdx, int fTo )
{
    int i, nEx = p->vExceptions.nSize;
    Msta_SdcExIndexFree( pIdx );
    pIdx->nHeadCap = 16;
    while ( pIdx->nHeadCap < 2 * nEx )
        pIdx->nHeadCap *= 2;
    pIdx->pHead = (int *)malloc( (size_t)pIdx->nHeadCap * sizeof(int) );
    pIdx->pNext = (int *)malloc( (size_t)(nEx + 1) * sizeof(int) );
    pIdx->pAlways = (int *)malloc( (size_t)(nEx + 1) * sizeof(int) );
    pIdx->pfAlways = (char *)calloc( (size_t)(nEx + 1), 1 );
    assert( pIdx->pHead && pIdx->pNext && pIdx->pAlways && pIdx->pfAlways );
    for ( i = 0; i < pIdx->nHeadCap; i++ )
        pIdx->pHead[i] = -1;
    for ( i = 0; i < nEx; i++ )
    {
        MstaId Key = Msta_SdcExIndexKey( MstaExceptionArrayAt(&p->vExceptions,i), fTo );
        const char *pKey;
        unsigned h;
        if ( Key == MSTA_NO_ID )
        {
            pIdx->pAlways[pIdx->nAlways++] = i;
            pIdx->pfAlways[i] = 1;
            continue;
        }
        pKey = Msta_NameStr( Key );
        h = Msta_SdcStrHash( pKey, strlen(pKey) ) & (unsigned)( pIdx->nHeadCap - 1 );
        pIdx->pNext[i] = pIdx->pHead[h];
        pIdx->pHead[h] = i;
    }
}

void Msta_SdcExIndexRelease( MstaSdc *p )
{
    Msta_SdcExIndexFree( &p->ExIndexTo );
    Msta_SdcExIndexFree( &p->ExIndexFrom );
    p->nExIndexed = 0;
    MstaSdcIntArrayFree( &s_vExCandidates );
    MstaSdcIntArrayFree( &s_vExHitsTo );
    MstaSdcIntArrayFree( &s_vExHitsFrom );
    MstaSdcIntArrayFree( &s_vExMerge );
    free( s_pExMark );
    s_pExMark = NULL;
    s_nExMarkCap = 0;
}

static void Msta_SdcExIndexUpdate( MstaSdc *p )
{
    int nEx = p->vExceptions.nSize;
    if ( p->nExIndexed == nEx && p->ExIndexTo.pHead != NULL )
        return;
    Msta_SdcExIndexBuild( p, &p->ExIndexTo, 1 );
    Msta_SdcExIndexBuild( p, &p->ExIndexFrom, 0 );
    if ( s_nExMarkCap < nEx + 1 )
    {
        s_nExMarkCap = nEx + 1;
        free( s_pExMark );
        s_pExMark = (char *)calloc( (size_t)s_nExMarkCap, 1 );
        assert( s_pExMark );
    }
    p->nExIndexed = nEx;
}

static void Msta_SdcExIndexLookup( MstaSdc *p, MstaSdcExIndex *pIdx, int fTo,
                                   const char *pText, size_t nLen, MstaSdcIntArray *vHits )
{
    char Mark = fTo ? 1 : 2;
    unsigned h = Msta_SdcStrHash( pText, nLen ) & (unsigned)( pIdx->nHeadCap - 1 );
    int i;
    for ( i = pIdx->pHead[h]; i >= 0; i = pIdx->pNext[i] )
    {
        MstaException *pEx = MstaExceptionArrayAt( &p->vExceptions, i );
        const char *pKey = Msta_NameStr( fTo ? pEx->ToText : pEx->FromText );
        if ( ( s_pExMark[i] & Mark ) || strncmp( pKey, pText, nLen ) != 0 || pKey[nLen] != 0 )
            continue;
        s_pExMark[i] |= Mark;
        *MstaSdcIntArrayAppend( vHits ) = i;
    }
}

static void Msta_SdcExIndexLookupText( MstaSdc *p, MstaSdcExIndex *pIdx, int fTo,
                                       const char *pText, MstaSdcIntArray *vHits )
{
    size_t i;
    if ( pText == NULL )
        return;
    for ( i = 0; pText[i]; i++ )
        if ( pText[i] == '/' )
            Msta_SdcExIndexLookup( p, pIdx, fTo, pText, i, vHits );
    Msta_SdcExIndexLookup( p, pIdx, fTo, pText, i, vHits );
}

static void Msta_SdcExIndexHits( MstaSdc *p, MstaDesign *pDes, int fTo,
                                 const MstaSdcObject *pObj, MstaSdcIntArray *vHits )
{
    MstaSdcExIndex *pIdx = fTo ? &p->ExIndexTo : &p->ExIndexFrom;
    vHits->nSize = 0;
    if ( pObj == NULL )
        return;
    Msta_SdcExIndexLookupText( p, pIdx, fTo, pObj->pText, vHits );
    if ( pObj->nInst >= 0 && pObj->nPin >= 0 )
    {
        char sPath[512];
        char *pPath = Msta_SdcObjectPinPath( pDes, pObj, sPath, sizeof(sPath) );
        Msta_SdcExIndexLookupText( p, pIdx, fTo, pPath, vHits );
        if ( pPath != sPath )
            free( pPath );
    }
}

static int Msta_SdcCompareIntAsc( const void *pA, const void *pB )
{
    return Msta_SdcCompareInt( *(const int *)pA, *(const int *)pB );
}

static void Msta_SdcMergeSortedTail( MstaSdcIntArray *v, int nHead )
{
    int i = 0, j = nHead, n = 0;
    if ( v->nSize - nHead <= 0 )
        return;
    qsort( v->pData + nHead, (size_t)(v->nSize - nHead), sizeof(int), Msta_SdcCompareIntAsc );
    if ( nHead == 0 )
        return;
    s_vExMerge.nSize = 0;
    while ( i < nHead || j < v->nSize )
    {
        if ( j >= v->nSize || ( i < nHead && v->pData[i] < v->pData[j] ) )
            *MstaSdcIntArrayAppend( &s_vExMerge ) = v->pData[i++];
        else
            *MstaSdcIntArrayAppend( &s_vExMerge ) = v->pData[j++];
        n++;
    }
    memcpy( v->pData, s_vExMerge.pData, (size_t)n * sizeof(int) );
}

static void Msta_SdcExIndexCollect( MstaSdcExIndex *pIdx, MstaSdcIntArray *vHits,
                                    MstaSdcExIndex *pOther, char OtherMark,
                                    MstaSdcIntArray *vOut )
{
    int i, nHead;
    for ( i = 0; i < pIdx->nAlways; i++ )
    {
        int e = pIdx->pAlways[i];
        if ( pOther->pfAlways[e] || ( s_pExMark[e] & OtherMark ) )
            *MstaSdcIntArrayAppend( vOut ) = e;
    }
    nHead = vOut->nSize;
    for ( i = 0; i < vHits->nSize; i++ )
    {
        int e = vHits->pData[i];
        if ( pOther->pfAlways[e] || ( s_pExMark[e] & OtherMark ) )
            *MstaSdcIntArrayAppend( vOut ) = e;
    }
    Msta_SdcMergeSortedTail( vOut, nHead );
}

static void Msta_SdcExIndexClearMarks( MstaSdcIntArray *vHits )
{
    int i;
    for ( i = 0; i < vHits->nSize; i++ )
        s_pExMark[vHits->pData[i]] = 0;
}

static int Msta_SdcExceptionCandidates( MstaSdc *p, MstaDesign *pDes,
                                        const MstaSdcEndpoint *pFrom,
                                        const MstaSdcEndpoint *pTo )
{
    int nTo, nFrom;
    Msta_SdcExIndexUpdate( p );
    s_vExCandidates.nSize = 0;
    Msta_SdcExIndexHits( p, pDes, 1, pTo ? pTo->pObj : NULL, &s_vExHitsTo );
    Msta_SdcExIndexHits( p, pDes, 0, pFrom ? pFrom->pObj : NULL, &s_vExHitsFrom );
    nTo = p->ExIndexTo.nAlways + s_vExHitsTo.nSize;
    nFrom = p->ExIndexFrom.nAlways + s_vExHitsFrom.nSize;
    if ( nTo <= nFrom )
        Msta_SdcExIndexCollect( &p->ExIndexTo, &s_vExHitsTo, &p->ExIndexFrom, 2,
                                &s_vExCandidates );
    else
        Msta_SdcExIndexCollect( &p->ExIndexFrom, &s_vExHitsFrom, &p->ExIndexTo, 1,
                                &s_vExCandidates );
    Msta_SdcExIndexClearMarks( &s_vExHitsTo );
    Msta_SdcExIndexClearMarks( &s_vExHitsFrom );
    return s_vExCandidates.nSize;
}

void Msta_SdcFindExceptionPath( MstaSdc *p, MstaDesign *pDes,
                                const MstaSdcEndpoint *pFrom, const MstaSdcEndpoint *pTo,
                                int fSetup, const MstaSdcObject *pObjects, int nObjects,
                                int *pnCycles, int *pfFalse )
{
    int i, nSetupCycles = 1, nHoldShift = 0;
    int nCandidates = Msta_SdcExceptionCandidates( p, pDes, pFrom, pTo );
    *pfFalse = 0;
    for ( i = 0; i < nCandidates; i++ )
    {
        MstaException *pEx = MstaExceptionArrayAt(&p->vExceptions,s_vExCandidates.pData[i]);
        if ( !Msta_SdcExceptionHits(p,pDes,pEx,pFrom,pTo,pObjects,nObjects) )
            continue;
        if ( pEx->fApplySetup ) nSetupCycles = pEx->nSetupCycles;
        if ( pEx->fApplyHold )  nHoldShift  = pEx->nHoldShift;
        if ( fSetup ? pEx->fFalseSetup : pEx->fFalseHold )
            *pfFalse = 1;
    }
    /* SDC 规则：-setup N 把 setup 捕捉沿推后 N-1 拍，hold 沿默认比 setup 沿早一拍，
       也就跟着推后；-hold M 再把 hold 沿往回拉 M 拍。所以 hold 的拍数是 N - M（1 = 默认位置）。 */
    *pnCycles = fSetup ? nSetupCycles : nSetupCycles - nHoldShift;
}

double Msta_SdcFindPathDelay( MstaSdc *p, MstaDesign *pDes,
                              const MstaSdcEndpoint *pFrom, const MstaSdcEndpoint *pTo,
                              const MstaSdcObject *pObjects, int nObjects, int fMax )
{
    int i, nCandidates = Msta_SdcExceptionCandidates( p, pDes, pFrom, pTo );
    double Best = MSTA_UNSET;
    for ( i = 0; i < nCandidates; i++ )
    {
        MstaException *pEx = MstaExceptionArrayAt(&p->vExceptions,s_vExCandidates.pData[i]);
        if ( !pEx->fMaxDelay && !pEx->fMinDelay )
            continue;
        if ( !Msta_SdcExceptionHits(p,pDes,pEx,pFrom,pTo,pObjects,nObjects) )
            continue;
        if ( fMax && pEx->fMaxDelay && (!Msta_IsSet(Best) || pEx->MaxDelay < Best) )
            Best = pEx->MaxDelay;
        if ( !fMax && pEx->fMinDelay && (!Msta_IsSet(Best) || pEx->MinDelay > Best) )
            Best = pEx->MinDelay;
    }
    return Best;
}

/* 路径被 false path 命中后，为了找次优路径：把命中的那条例外的 -through 匹配到的
   (网络, 边沿) 收集起来，调用方排除掉这些到达时间再重算。
   -through 不带边沿时两个边沿都排除（那条例外对这根网络上的任何边沿都成立）；
   只有 -from/-to 的例外没法细分，返回 0 个。 */
int Msta_SdcPathExclusions( MstaSdc *p, MstaDesign *pDes,
                            const MstaSdcEndpoint *pFrom, const MstaSdcEndpoint *pTo,
                            int fSetup, const MstaSdcObject *pObjects, int nObjects,
                            MstaPathExclude *pOut, int nCap )
{
    int i, nOut = 0, nCandidates = Msta_SdcExceptionCandidates( p, pDes, pFrom, pTo );
    for ( i = 0; i < nCandidates && nOut < nCap; i++ )
    {
        MstaException *pEx = MstaExceptionArrayAt(&p->vExceptions,s_vExCandidates.pData[i]);
        int nHits[MSTA_SDC_MAX_THRU], nHitCount = 0, g;
        char vRF[MSTA_SDC_MAX_THRU];
        if ( pEx->nThru == 0 )
            continue;
        if ( !( fSetup ? pEx->fFalseSetup : pEx->fFalseHold ) )
            continue;
        if ( !Msta_SdcExceptionHits(p,pDes,pEx,pFrom,pTo,pObjects,nObjects) )
            continue;
        if ( !Msta_SdcMatchThrough(pDes,pEx,pObjects,nObjects,nHits,vRF,&nHitCount) )
            continue;
        for ( g = 0; g < nHitCount && nOut < nCap; g++ )
        {
            const MstaSdcObject *pObj = &pObjects[nHits[g]];
            int j, fSeen = 0;
            int EdgeMask = ( vRF[g] == 'r' ) ? 1 : ( vRF[g] == 'f' ) ? 2 : 3;
            if ( pObj->nNet < 0 )
                continue;
            for ( j = 0; j < nOut; j++ )
                if ( pOut[j].Net == pObj->nNet )
                { pOut[j].EdgeMask |= EdgeMask; fSeen = 1; break; }
            if ( !fSeen )
            {
                pOut[nOut].Net = pObj->nNet;
                pOut[nOut].EdgeMask = EdgeMask;
                nOut++;
            }
        }
    }
    return nOut;
}

static const MstaException *s_pSortExceptions;

/* 比较两条例外除 -from 名字（FromText/FromKind）以外的全部字段；返回 0 表示两者只差 -from。 */
static int Msta_SdcCompareExceptionRest( const MstaException *pA, const MstaException *pB )
{
    int k, r;
    if ( (r = Msta_SdcCompareInt(pA->ToText, pB->ToText)) != 0 ) return r;
    if ( (r = Msta_SdcCompareInt(pA->ToKind, pB->ToKind)) != 0 ) return r;
    if ( (r = Msta_SdcCompareInt(pA->FromRF, pB->FromRF)) != 0 ) return r;
    if ( (r = Msta_SdcCompareInt(pA->ToRF, pB->ToRF)) != 0 ) return r;
    if ( (r = Msta_SdcCompareInt(pA->nThru, pB->nThru)) != 0 ) return r;
    for ( k = 0; k < pA->nThru; k++ )
    {
        if ( (r = Msta_SdcCompareInt(pA->Thru[k].Text, pB->Thru[k].Text)) != 0 ) return r;
        if ( (r = Msta_SdcCompareInt(pA->Thru[k].Kind, pB->Thru[k].Kind)) != 0 ) return r;
        if ( (r = Msta_SdcCompareInt(pA->Thru[k].RF, pB->Thru[k].RF)) != 0 ) return r;
    }
    if ( (r = Msta_SdcCompareInt(pA->nSetupCycles, pB->nSetupCycles)) != 0 ) return r;
    if ( (r = Msta_SdcCompareInt(pA->nHoldShift, pB->nHoldShift)) != 0 ) return r;
    if ( (r = Msta_SdcCompareInt(pA->fApplySetup, pB->fApplySetup)) != 0 ) return r;
    if ( (r = Msta_SdcCompareInt(pA->fApplyHold, pB->fApplyHold)) != 0 ) return r;
    if ( (r = Msta_SdcCompareInt(pA->fFalseSetup, pB->fFalseSetup)) != 0 ) return r;
    if ( (r = Msta_SdcCompareInt(pA->fFalseHold, pB->fFalseHold)) != 0 ) return r;
    if ( (r = Msta_SdcCompareInt(pA->fMaxDelay, pB->fMaxDelay)) != 0 ) return r;
    if ( (r = Msta_SdcCompareInt(pA->fMinDelay, pB->fMinDelay)) != 0 ) return r;
    if ( pA->fMaxDelay && (r = Msta_SdcCompareDouble(pA->MaxDelay, pB->MaxDelay)) != 0 ) return r;
    if ( pA->fMinDelay && (r = Msta_SdcCompareDouble(pA->MinDelay, pB->MinDelay)) != 0 ) return r;
    return 0;
}

static int Msta_SdcCompareExceptionIndex( const void *pA, const void *pB )
{
    int a = *(const int *)pA, b = *(const int *)pB;
    int r = Msta_SdcCompareExceptionRest( &s_pSortExceptions[a], &s_pSortExceptions[b] );
    return r ? r : Msta_SdcCompareInt( a, b );
}

int Msta_SdcExceptionCount( MstaSdc *p )
{
    return p->vExceptions.nSize;
}

/* 给 -from 是非时钟对象的例外编组号（其余记 -1），只差 -from 名字的例外同组。
   -from 例外只对部分起点生效，命中例外不同的起点不能合在一起传播；引擎按起点命中的组号
   给起点分类（msta_timing.c 的 BuildStartClasses），同组例外对起点效果相同，合组可减少分类数。 */
int Msta_SdcFromExceptionGroups( MstaSdc *p, int *pGroups )
{
    int i, n = 0, nGroups = 0;
    int *pOrder = (int *)malloc( (size_t)(p->vExceptions.nSize + 1) * sizeof(int) );
    assert( pOrder );
    for ( i = 0; i < p->vExceptions.nSize; i++ )
    {
        MstaException *pEx = MstaExceptionArrayAt(&p->vExceptions,i);
        pGroups[i] = -1;
        if ( pEx->FromText != MSTA_NO_ID && pEx->FromKind != 'C' )
            pOrder[n++] = i;
    }
    s_pSortExceptions = p->vExceptions.pData;
    qsort( pOrder, (size_t)n, sizeof(int), Msta_SdcCompareExceptionIndex );
    for ( i = 0; i < n; i++ )
    {
        if ( i == 0 || Msta_SdcCompareExceptionRest( &s_pSortExceptions[pOrder[i-1]],
                                                     &s_pSortExceptions[pOrder[i]] ) != 0 )
            nGroups++;
        pGroups[pOrder[i]] = nGroups - 1;
    }
    free( pOrder );
    return nGroups;
}

int Msta_SdcFromExceptionMatches( MstaSdc *p, MstaDesign *pDes, const MstaSdcObject *pObj,
                                  const int **ppExceptions )
{
    int i, n = 0, nHead;
    Msta_SdcExIndexUpdate( p );
    Msta_SdcExIndexHits( p, pDes, 0, pObj, &s_vExHitsFrom );
    Msta_SdcExIndexClearMarks( &s_vExHitsFrom );
    s_vExCandidates.nSize = 0;
    for ( i = 0; i < p->ExIndexFrom.nAlways; i++ )
    {
        MstaException *pEx = MstaExceptionArrayAt( &p->vExceptions, p->ExIndexFrom.pAlways[i] );
        if ( pEx->FromText != MSTA_NO_ID && pEx->FromKind != 'C' )
            *MstaSdcIntArrayAppend( &s_vExCandidates ) = p->ExIndexFrom.pAlways[i];
    }
    nHead = s_vExCandidates.nSize;
    for ( i = 0; i < s_vExHitsFrom.nSize; i++ )
        *MstaSdcIntArrayAppend( &s_vExCandidates ) = s_vExHitsFrom.pData[i];
    Msta_SdcMergeSortedTail( &s_vExCandidates, nHead );
    for ( i = 0; i < s_vExCandidates.nSize; i++ )
    {
        MstaException *pEx = MstaExceptionArrayAt( &p->vExceptions, s_vExCandidates.pData[i] );
        if ( pEx->FromText == MSTA_NO_ID || pEx->FromKind == 'C' ||
             !Msta_SdcMatchObject( pDes, pEx->FromText, pEx->FromKind, pObj, NULL ) )
            continue;
        s_vExCandidates.pData[n++] = s_vExCandidates.pData[i];
    }
    s_vExCandidates.nSize = n;
    *ppExceptions = s_vExCandidates.pData;
    return n;
}

int Msta_SdcPathGroupCount( MstaSdc *p )
{
    return p->vPathGroups.nSize;
}

MstaPathGroup *Msta_SdcPathGroupByIndex( MstaSdc *p, int i )
{
    return MstaPathGroupArrayAt( &p->vPathGroups, i );
}

int Msta_SdcPathGroupsUsed( MstaSdc *p )
{
    return p->vPathGroups.nSize > 0;
}

/* 分组记录的端点、边沿、-through 是否命中一条路径（与例外用同一套匹配）。 */
static int Msta_SdcPathGroupHits( MstaSdc *p, MstaDesign *pDes, MstaPathGroup *pGroup,
                                  const MstaSdcEndpoint *pFrom, const MstaSdcEndpoint *pTo,
                                  const MstaSdcObject *pObjects, int nObjects )
{
    (void)p;
    if ( !Msta_SdcMatchObject(pDes,pGroup->FromText,pGroup->FromKind,
                              pFrom ? pFrom->pObj : NULL, pFrom ? pFrom->pClock : NULL) )
        return 0;
    if ( !Msta_SdcMatchObject(pDes,pGroup->ToText,pGroup->ToKind,
                              pTo ? pTo->pObj : NULL, pTo ? pTo->pClock : NULL) )
        return 0;
    if ( !Msta_SdcEdgeMatches(pGroup->FromRF, Msta_SdcEndpointEdge(pFrom,pGroup->FromKind)) ||
         !Msta_SdcEdgeMatches(pGroup->ToRF, Msta_SdcEndpointEdge(pTo,pGroup->ToKind)) )
        return 0;
    return Msta_SdcMatchThroughList( pDes, pGroup->Thru, pGroup->nThru,
                                     pObjects, nObjects, NULL, NULL, NULL );
}

MstaId Msta_SdcFindPathGroup( MstaSdc *p, MstaDesign *pDes,
                              const MstaSdcEndpoint *pFrom, const MstaSdcEndpoint *pTo,
                              const MstaSdcObject *pObjects, int nObjects )
{
    int i, fDefault = 0;
    for ( i = 0; i < p->vPathGroups.nSize; i++ )
    {
        MstaPathGroup *pGroup = MstaPathGroupArrayAt( &p->vPathGroups, i );
        if ( pGroup->fDefault )
            { fDefault = 1; continue; }        /* 兜底组留到最后再看 */
        if ( Msta_SdcPathGroupHits(p,pDes,pGroup,pFrom,pTo,pObjects,nObjects) )
            return pGroup->Name;
    }
    if ( !fDefault )
        return MSTA_NO_ID;
    for ( i = 0; i < p->vPathGroups.nSize; i++ )
    {
        MstaPathGroup *pGroup = MstaPathGroupArrayAt( &p->vPathGroups, i );
        if ( pGroup->fDefault &&
             Msta_SdcPathGroupHits(p,pDes,pGroup,pFrom,pTo,pObjects,nObjects) )
            return Msta_NameId( MSTA_SDC_GROUP_DEFAULT );
    }
    return MSTA_NO_ID;
}

double Msta_SdcPathGroupWeight( MstaSdc *p, MstaId NameId )
{
    int i;
    for ( i = 0; i < p->vPathGroups.nSize; i++ )
    {
        MstaPathGroup *pGroup = MstaPathGroupArrayAt( &p->vPathGroups, i );
        if ( pGroup->fDefault ) continue;
        if ( pGroup->Name == NameId ) return pGroup->Weight;
    }
    return 1.0;
}

double Msta_SdcMaxTimeBorrow( MstaSdc *p, int nInst )
{
    int i;
    double Value = MSTA_UNSET;
    /* 先看这个实例的专用值，再看全局默认（后写的覆盖先写的）。 */
    for ( i = 0; i < p->vBorrow.nSize; i++ )
    {
        MstaBorrowSdc *pRec = MstaBorrowSdcArrayAt( &p->vBorrow, i );
        if ( pRec->Inst == nInst ) Value = pRec->Value;
    }
    if ( Msta_IsSet(Value) )
        return Value;
    for ( i = 0; i < p->vBorrow.nSize; i++ )
    {
        MstaBorrowSdc *pRec = MstaBorrowSdcArrayAt( &p->vBorrow, i );
        if ( pRec->Inst == -1 ) Value = pRec->Value;
    }
    return Value;
}

void Msta_SdcClockGatingValue( MstaSdc *p, int nInst, int fMax,
                               double *pValue, int *pfSet )
{
    int i, fDefault = 0;
    *pValue = MSTA_UNSET;
    *pfSet = 0;
    /* 先看这个实例的专用条目，再看全局默认（后写的覆盖先写的）。 */
    for ( i = 0; i < p->vClockGating.nSize; i++ )
    {
        MstaClockGatingSdc *pRec = MstaClockGatingSdcArrayAt( &p->vClockGating, i );
        double v;
        if ( pRec->Inst == -1 ) { fDefault = 1; continue; }
        if ( pRec->Inst != nInst ) continue;
        v = fMax ? pRec->Setup : pRec->Hold;
        if ( Msta_IsSet(v) ) { *pValue = v; *pfSet = 1; }
    }
    if ( !*pfSet && fDefault )
        for ( i = 0; i < p->vClockGating.nSize; i++ )
        {
            MstaClockGatingSdc *pRec = MstaClockGatingSdcArrayAt( &p->vClockGating, i );
            double v;
            if ( pRec->Inst != -1 ) continue;
            v = fMax ? pRec->Setup : pRec->Hold;
            if ( Msta_IsSet(v) ) { *pValue = v; *pfSet = 1; }
        }
}

void Msta_SdcPrintClocks( MstaSdc *p, MstaDesign *pDes, FILE *pFile )
{
    int i;
    fprintf( pFile, "%-12s %11s %10s %10s  %s\n", "clock", "period(ns)", "setup_unc",
             "hold_unc", "source" );
    for ( i = 0; i < p->vClocks.nSize; i++ )
    {
        MstaClock *pClock = MstaClockArrayAt( &p->vClocks, i );
        fprintf( pFile, "%-12s %11.3f %10.3f %10.3f  %s\n",
                 Msta_NameStr( pClock->Name ), pClock->Period / 1000.0,
                 pClock->UncertaintySetup / 1000.0, pClock->UncertaintyHold / 1000.0,
                 pClock->SourceNet >= 0 ? Msta_NetName( pDes, pClock->SourceNet ) : "(virtual)" );
        fprintf( pFile, "  edges %.3f/%.3f ns  source latency max/min %.3f/%.3f ns  network latency max/min %.3f/%.3f ns  %s\n",
                 pClock->RiseEdge / 1000.0, pClock->FallEdge / 1000.0,
                 pClock->SourceLatencyMax / 1000.0, pClock->SourceLatencyMin / 1000.0,
                 pClock->NetworkLatencyMax / 1000.0, pClock->NetworkLatencyMin / 1000.0,
                 pClock->fPropagated ? "propagated" : "ideal network" );
    }
    if ( p->vClocks.nSize == 0 )
        fprintf( pFile, "(no clock -- say create_clock)\n" );
}
