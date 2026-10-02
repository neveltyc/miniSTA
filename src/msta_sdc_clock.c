/**CFile***************************************************************

  FileName    [msta_sdc_clock.c]

  Synopsis    [SDC 时钟类命令：create_clock、生成时钟、不确定度、延迟、摆率、极性与时钟组。]

  每条命令一张选项表加一个处理函数；位置参数的形状写在分发表 s_vSdcCommands
  （msta_sdc.c）里。处理函数被调用时语法已经由 msta_sdc_parse.c 的通用解析器
  检查过，只需检查取值、查对象、写模型。

***********************************************************************/

#include <math.h>
#include "msta_sdc_int.h"

/* ---------------- 时钟 ---------------- */

/* 分配并初始化一条时钟记录。 */
static MstaClock *Msta_SdcNewClock( MstaSdc *p, const char *pName )
{
    MstaClock *pClock = MstaClockArrayAppend( &p->vClocks );
    pClock->Name = Msta_NameId( pName );
    pClock->Period = 0.0;
    pClock->UncertaintySetup = 0.0;
    pClock->UncertaintyHold  = 0.0;
    pClock->RiseEdge = 0.0;
    pClock->FallEdge = 0.0;
    pClock->fPropagated = 1; /* 默认沿 Liberty 时序弧传播时钟。 */
    pClock->MasterClock = MSTA_NO_ID;
    pClock->SourceNet  = MSTA_NO_ID;
    pClock->SourceText = MSTA_NO_ID;
    pClock->Slew[0][0] = pClock->Slew[0][1] = MSTA_UNSET;
    pClock->Slew[1][0] = pClock->Slew[1][1] = MSTA_UNSET;
    Msta_IntMapSet( &p->clockMap, pClock->Name, p->vClocks.nSize - 1 );
    return pClock;
}

/* 网络 nNet 上是否已经有时钟。 */
static int Msta_SdcNetHasClock( MstaSdc *p, int nNet )
{
    int i;
    for ( i = 0; i < p->vClocks.nSize; i++ )
        if ( Msta_SdcClockByIndex(p,i)->SourceNet == nNet )
            return 1;
    return 0;
}

/* create_clock -period 周期 [-name 名字] [-waveform {上升 下降}] [-add] [源端口]
   -period 的 "多周期波形"（{4 8}）写法不支持；只认标量。不写 -name 时，
   源端口名就是时钟名；同一个源上的第二个时钟要写 -add。 */
const MstaSdcOpt Msta_SdcCreateClockOpts[] = {
    { "-name",     MSTA_SDC_VALUE, 0 },
    { "-period",   MSTA_SDC_VALUE, 0 },
    { "-waveform", MSTA_SDC_VALUE, 0 },
    { "-add",      MSTA_SDC_FLAG,  0 },
    { NULL,        MSTA_SDC_FLAG,  0 } };

void Msta_SdcCreateClock( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    const char *pName = Msta_SdcOptValue( pCmd, "-name" );
    const char *pWave = Msta_SdcOptValue( pCmd, "-waveform" );
    double Period, Rise = 0.0, Fall;
    int nSource = MSTA_NO_ID, *pNets;
    MstaClock *pClock;

    if ( !Msta_SdcRequire( p, pCmd, "-period" ) ||
         !Msta_SdcGetNumber( p, pCmd, "-period", Msta_SdcOptValue( pCmd, "-period" ),
                             MSTA_SDC_POSITIVE, &Period ) )
        return;
    Fall = 0.5 * Period;
    if ( pWave )
    {
        char Extra;
        if ( sscanf( pWave, "%lf %lf %c", &Rise, &Fall, &Extra ) != 2 )
        { Msta_SdcReject( p, pCmd, "-waveform 必须正好给两个边沿时间" ); return; }
        if ( Rise < 0.0 || Fall <= Rise || Fall >= Period )
        { Msta_SdcReject( p, pCmd, "-waveform 必须满足 0 <= 上升沿 < 下降沿 < 周期" ); return; }
    }
    if ( pCmd->nObjs > 1 )
    { Msta_SdcReject( p, pCmd, "多个源对象未建模" ); return; }
    if ( pName == NULL && pCmd->nObjs == 0 )
    { Msta_SdcReject( p, pCmd, "需要 -name 或一个源对象" ); return; }
    if ( pName == NULL )
        pName = pCmd->ppObjs[0];
    if ( Msta_SdcFindClock( p, pName ) )
    { Msta_SdcReject( p, pCmd, "时钟 \"%s\" 已经存在", pName ); return; }
    if ( pCmd->nObjs == 1 )
    {
        if ( Msta_SdcResolveNets( pDes, pCmd->ppObjs[0], &pNets ) != 1 )
        { Msta_SdcReject( p, pCmd, "源对象必须对应唯一一个网络" ); return; }
        nSource = pNets[0];
        /* 不写 -add 是"换掉这个源上的时钟"，msta 不做替换。 */
        if ( !Msta_SdcHasFlag( pCmd, "-add" ) && Msta_SdcNetHasClock( p, nSource ) )
        { Msta_SdcReject( p, pCmd, "源对象上已经有时钟（要再加一个请用 -add）" ); return; }
    }
    pClock = Msta_SdcNewClock( p, pName );
    pClock->Period   = Period * p->TimeScalePs;
    pClock->RiseEdge = Rise * p->TimeScalePs;
    pClock->FallEdge = Fall * p->TimeScalePs;
    if ( pCmd->nObjs == 1 )
    {
        pClock->SourceNet  = nSource;
        pClock->SourceText = Msta_NameId( pCmd->ppObjs[0] );
    }
}

/* 解析 SDC 的列表写法：-edges {1 3 5} 传到 C 侧就是一段 "1 3 5" 文本。
   花括号、逗号、空格都当分隔符；碰到不是数字的地方停下。返回取到的个数。 */
static int Msta_SdcParseNumberList( const char *pText, double *pOut, int nCap )
{
    const char *p = pText;
    int n = 0;
    while ( p != NULL && *p != 0 && n < nCap )
    {
        char *pEnd;
        double Value;
        while ( *p == ' ' || *p == '\t' || *p == '{' || *p == '}' || *p == ',' ) p++;
        if ( *p == 0 ) break;
        Value = strtod( p, &pEnd );
        if ( pEnd == p ) break;
        pOut[n++] = Value;
        p = pEnd;
    }
    return n;
}

/* 时钟的第 n 个边沿（n 从 1 开始数）：1 = 首个上升沿，2 = 首个下降沿，
   3 = 第二个上升沿…… 返回值是相对时钟源的时间，ps。 */
static double Msta_SdcClockEdgeTime( MstaClock *pClock, int nEdge )
{
    int n = nEdge - 1;
    double Phase = ( n % 2 == 0 ) ? pClock->RiseEdge : pClock->FallEdge;
    return Phase + (double)( n / 2 ) * pClock->Period;
}

/* 分频/倍频系数：必须是 1..1000000 的整数。 */
static int Msta_SdcGetRatio( MstaSdc *p, const MstaSdcCmd *pCmd, const char *pName, double *pRatio )
{
    const char *pText = Msta_SdcOptValue( pCmd, pName );
    *pRatio = 1.0;
    if ( pText == NULL )
        return 1;
    if ( !Msta_SdcGetNumber( p, pCmd, pName, pText, MSTA_SDC_POSITIVE, pRatio ) )
        return 0;
    if ( *pRatio > 1000000.0 || *pRatio != (double)(int)*pRatio )
    { Msta_SdcReject( p, pCmd, "%s 必须是正整数（给的是 %s）", pName, pText ); return 0; }
    return 1;
}

/* create_generated_clock -source 主时钟源 [-name 名字] [-master_clock 主时钟]
       [-divide_by n | -multiply_by n] [-duty_cycle d] [-invert]
       [-edges {e1 e2 e3} [-edge_shift {s1 s2 s3}]] 目标引脚
   创建支持分频、倍频和反相的生成时钟。-add / -combinational 不建模。 */
const MstaSdcOpt Msta_SdcGeneratedClockOpts[] = {
    { "-name",          MSTA_SDC_VALUE, 0               },
    { "-source",        MSTA_SDC_VALUE, 0               },
    { "-master_clock",  MSTA_SDC_VALUE, 0               },
    { "-divide_by",     MSTA_SDC_VALUE, 0               },
    { "-multiply_by",   MSTA_SDC_VALUE, 0               },
    { "-duty_cycle",    MSTA_SDC_VALUE, 0               },
    { "-edges",         MSTA_SDC_VALUE, 0               },
    { "-edge_shift",    MSTA_SDC_VALUE, 0               },
    { "-invert",        MSTA_SDC_FLAG,  0               },
    { "-add",           MSTA_SDC_FLAG,  MSTA_SDC_REJECT },
    { "-combinational", MSTA_SDC_FLAG,  MSTA_SDC_REJECT },
    { NULL,             MSTA_SDC_FLAG,  0               } };

void Msta_SdcCreateGeneratedClock( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    const char *pName       = Msta_SdcOptValue( pCmd, "-name" );
    const char *pSource     = Msta_SdcOptValue( pCmd, "-source" );
    const char *pMasterName = Msta_SdcOptValue( pCmd, "-master_clock" );
    const char *pDuty       = Msta_SdcOptValue( pCmd, "-duty_cycle" );
    const char *pEdges      = Msta_SdcOptValue( pCmd, "-edges" );
    const char *pShifts     = Msta_SdcOptValue( pCmd, "-edge_shift" );
    const char *pTarget     = pCmd->ppObjs[0];
    MstaClock *pMaster = NULL, *pClock;
    int *pNets, i;
    double Div, Mult, Duty = 50.0;
    double vEdges[3], vShifts[3] = { 0.0, 0.0, 0.0 };

    if ( !Msta_SdcRequire( p, pCmd, "-source" ) )
        return;
    if ( pEdges != NULL && ( Msta_SdcHasFlag( pCmd, "-divide_by" ) ||
                             Msta_SdcHasFlag( pCmd, "-multiply_by" ) || pDuty != NULL ) )
    { Msta_SdcReject( p, pCmd, "-edges 不能与 -divide_by/-multiply_by/-duty_cycle 同时使用" ); return; }
    if ( Msta_SdcHasFlag( pCmd, "-divide_by" ) && Msta_SdcHasFlag( pCmd, "-multiply_by" ) )
    { Msta_SdcReject( p, pCmd, "-divide_by 与 -multiply_by 不能同时使用" ); return; }
    if ( !Msta_SdcGetRatio( p, pCmd, "-divide_by", &Div ) ||
         !Msta_SdcGetRatio( p, pCmd, "-multiply_by", &Mult ) )
        return;
    if ( pDuty != NULL && !Msta_SdcGetNumber( p, pCmd, "-duty_cycle", pDuty, MSTA_SDC_ANY, &Duty ) )
        return;
    if ( Duty <= 0.0 || Duty >= 100.0 )
    { Msta_SdcReject( p, pCmd, "-duty_cycle 必须在 0 到 100 之间（给的是 %s）", pDuty ); return; }
    if ( pShifts != NULL && pEdges == NULL )
    { Msta_SdcReject( p, pCmd, "-edge_shift 需要和 -edges 一起使用" ); return; }
    /* -edges {e1 e2 e3}：用主时钟的第 e1/e2/e3 个边沿定义新时钟的
       上升沿、下降沿和下一个上升沿。边号从 1 开始数：1 = 首个上升沿，
       2 = 首个下降沿，3 = 第二个上升沿……。 */
    if ( pEdges != NULL )
    {
        if ( Msta_SdcParseNumberList( pEdges, vEdges, 3 ) != 3 )
        { Msta_SdcReject( p, pCmd, "-edges 必须正好给三个边沿序号" ); return; }
        if ( pShifts != NULL && Msta_SdcParseNumberList( pShifts, vShifts, 3 ) != 3 )
        { Msta_SdcReject( p, pCmd, "-edge_shift 的值个数必须与 -edges 相同" ); return; }
        for ( i = 0; i < 3; i++ )
        {
            if ( vEdges[i] < 1.0 || vEdges[i] != (double)(int)vEdges[i] )
            { Msta_SdcReject( p, pCmd, "-edges 的值必须是正整数" ); return; }
            if ( i > 0 && vEdges[i] <= vEdges[i-1] )
            { Msta_SdcReject( p, pCmd, "-edges 的值必须递增" ); return; }
        }
    }
    /* 主时钟：-master_clock 点名，或者 -source 所在网络上的那个时钟。 */
    if ( pMasterName != NULL )
    {
        pMaster = Msta_SdcFindClock( p, pMasterName );
        if ( pMaster == NULL )
        { Msta_SdcReject( p, pCmd, "找不到主时钟 \"%s\"", pMasterName ); return; }
    }
    else if ( Msta_SdcResolveNets( pDes, pSource, &pNets ) > 0 )
        for ( i = 0; i < p->vClocks.nSize && pMaster == NULL; i++ )
            if ( Msta_SdcClockByIndex(p,i)->SourceNet == pNets[0] )
                pMaster = Msta_SdcClockByIndex(p,i);
    if ( pMaster == NULL )
    { Msta_SdcReject( p, pCmd, "\"%s\" 上找不到主时钟", pSource ); return; }
    if ( Msta_SdcResolveNets( pDes, pTarget, &pNets ) != 1 )
    { Msta_SdcReject( p, pCmd, "目标必须对应唯一一个网络" ); return; }
    if ( Msta_SdcNetHasClock( p, pNets[0] ) )
    { Msta_SdcReject( p, pCmd, "目标上已经有时钟" ); return; }
    if ( pName == NULL )
        pName = pTarget;
    if ( Msta_SdcFindClock( p, pName ) )
    { Msta_SdcReject( p, pCmd, "时钟 \"%s\" 已经存在", pName ); return; }
    /* Append 可能重新分配 vClocks，先把主时钟的各项值取出来。 */
    {
        double Period = pMaster->Period * Div / Mult;
        double Rise = pMaster->RiseEdge;
        double Fall = Rise + Period * Duty / 100.0;
        MstaId MasterName = pMaster->Name;
        if ( pEdges != NULL )
        {
            Rise = Msta_SdcClockEdgeTime( pMaster, (int)vEdges[0] ) + vShifts[0] * p->TimeScalePs;
            Fall = Msta_SdcClockEdgeTime( pMaster, (int)vEdges[1] ) + vShifts[1] * p->TimeScalePs;
            Period = Msta_SdcClockEdgeTime( pMaster, (int)vEdges[2] ) + vShifts[2] * p->TimeScalePs - Rise;
            if ( Period <= 0.0 || Fall <= Rise )
            { Msta_SdcReject( p, pCmd, "-edges 算出的周期不大于 0" ); return; }
        }
        pClock = Msta_SdcNewClock( p, pName );
        pClock->MasterClock = MasterName;
        pClock->Period   = Period;
        pClock->RiseEdge = Msta_SdcHasFlag( pCmd, "-invert" ) ? Fall : Rise;
        pClock->FallEdge = Msta_SdcHasFlag( pCmd, "-invert" ) ? Rise : Fall;
        pClock->SourceNet  = pNets[0];
        pClock->SourceText = Msta_NameId( pTarget );
    }
}

/* set_clock_uncertainty [-setup] [-hold] [-rise|-fall] 值 时钟列表
   set_clock_uncertainty [-setup] [-hold] -from|-rise_from|-fall_from 时钟列表
                         -to|-rise_to|-fall_to 时钟列表 值
   给时钟、或者两组时钟之间设置 setup/hold 不确定度。-setup/-hold 都没写时两者
   都设；裸 -rise/-fall 限定的是 -to 一侧（捕获沿）的边沿。 */
const MstaSdcOpt Msta_SdcClockUncertaintyOpts[] = {
    { "-setup", MSTA_SDC_FLAG, 0           },
    { "-hold",  MSTA_SDC_FLAG, 0           },
    { "-rise",  MSTA_SDC_FLAG, 0           },
    { "-fall",  MSTA_SDC_FLAG, 0           },
    { "-from",  MSTA_SDC_LIST, MSTA_SDC_RF },
    { "-to",    MSTA_SDC_LIST, MSTA_SDC_RF },
    { NULL,     MSTA_SDC_FLAG, 0           } };

void Msta_SdcSetClockUncertainty( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    int fSetup = Msta_SdcHasFlag( pCmd, "-setup" ) || !Msta_SdcHasFlag( pCmd, "-hold" );
    int fHold  = Msta_SdcHasFlag( pCmd, "-hold" ) || !Msta_SdcHasFlag( pCmd, "-setup" );
    double Value = Msta_SdcToPs( p, pCmd->pValue );
    char **ppFrom, **ppTo, ToRF = Msta_SdcOptEdge( pCmd, "-to" );
    int nFrom = Msta_SdcOptList( pCmd, "-from", &ppFrom );
    int nTo   = Msta_SdcOptList( pCmd, "-to", &ppTo );
    int *pFrom = NULL, *pTo = NULL, *pClocks, nClocks, i, j, c;

    if ( ToRF == 0 )
        ToRF = Msta_SdcCmdEdge( pCmd );
    if ( nFrom == 0 && nTo == 0 )
    {
        if ( pCmd->nObjs == 0 )
        { Msta_SdcReject( p, pCmd, "需要时钟列表或 -from/-to" ); return; }
        nClocks = Msta_SdcObjectClocks( p, pCmd, pCmd->ppObjs, pCmd->nObjs, &pClocks );
        for ( i = 0; i < nClocks; i++ )
        {
            MstaClock *pClock = Msta_SdcClockByIndex( p, pClocks[i] );
            if ( fSetup ) pClock->UncertaintySetup = Value;
            if ( fHold )  pClock->UncertaintyHold  = Value;
        }
        return;
    }
    /* -from/-to 形式：两组时钟两两之间各记一条；没写的一侧表示任意时钟。 */
    if ( pCmd->nObjs > 0 )
    { Msta_SdcReject( p, pCmd, "时钟列表不能与 -from/-to 同时使用" ); return; }
    if ( nFrom > 0 && ( nFrom = Msta_SdcObjectClocks( p, pCmd, ppFrom, nFrom, &pFrom ) ) < 0 )
        return;
    if ( nTo > 0 && ( nTo = Msta_SdcObjectClocks( p, pCmd, ppTo, nTo, &pTo ) ) < 0 )
        return;
    for ( i = 0; i < ( nFrom > 0 ? nFrom : 1 ); i++ )
        for ( j = 0; j < ( nTo > 0 ? nTo : 1 ); j++ )
            for ( c = 0; c < 2; c++ )
            {
                MstaInterClockUnc *pUnc;
                if ( c == 0 ? !fSetup : !fHold )
                    continue;
                pUnc = MstaInterClockUncArrayAppend( &p->vInterClockUnc );
                pUnc->FromClock = nFrom > 0 ? Msta_SdcClockByIndex( p, pFrom[i] )->Name : MSTA_NO_ID;
                pUnc->ToClock   = nTo   > 0 ? Msta_SdcClockByIndex( p, pTo[j] )->Name   : MSTA_NO_ID;
                pUnc->FromRF = Msta_SdcOptEdge( pCmd, "-from" );
                pUnc->ToRF   = ToRF;
                pUnc->fSetup = c == 0;
                pUnc->fHold  = c == 1;
                pUnc->Value  = Value;
            }
}

/* set_clock_latency [-source] [-min|-max|-early|-late] [-rise|-fall] [-clock 时钟列表] 延迟 [时钟列表]
   设置理想时钟分析的源延迟 / 网络延迟。-early/-late 与 -min/-max 同义；
   要设的时钟可以写在 -clock 后面，也可以写成对象列表。 */
const MstaSdcOpt Msta_SdcClockLatencyOpts[] = {
    { "-clock",  MSTA_SDC_OBJECTS, 0 },
    { "-source", MSTA_SDC_FLAG,    0 },
    { "-min",    MSTA_SDC_FLAG, 0 },
    { "-max",    MSTA_SDC_FLAG, 0 },
    { "-early",  MSTA_SDC_FLAG, 0 },
    { "-late",   MSTA_SDC_FLAG, 0 },
    { "-rise",   MSTA_SDC_FLAG, 0 },
    { "-fall",   MSTA_SDC_FLAG, 0 },
    { NULL,      MSTA_SDC_FLAG, 0 } };

void Msta_SdcSetClockLatency( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    int fMax = Msta_SdcHasFlag( pCmd, "-max" ) || Msta_SdcHasFlag( pCmd, "-late" );
    int fMin = Msta_SdcHasFlag( pCmd, "-min" ) || Msta_SdcHasFlag( pCmd, "-early" );
    double Delay = Msta_SdcToPs( p, pCmd->pValue );
    char **ppOpt, **ppNames;
    int nOpt = Msta_SdcOptList( pCmd, "-clock", &ppOpt );
    int Sel[2][2], *pClocks, nClocks, i;

    /* 要设的时钟：-clock 的列表加上对象列表。 */
    ppNames = (char **)Msta_SdcArenaZeros( nOpt + pCmd->nObjs, sizeof(char *) );
    for ( i = 0; i < nOpt; i++ )
        ppNames[i] = ppOpt[i];
    for ( i = 0; i < pCmd->nObjs; i++ )
        ppNames[nOpt + i] = pCmd->ppObjs[i];
    if ( nOpt + pCmd->nObjs == 0 )
    { Msta_SdcReject( p, pCmd, "需要时钟列表" ); return; }
    nClocks = Msta_SdcObjectClocks( p, pCmd, ppNames, nOpt + pCmd->nObjs, &pClocks );
    Msta_SdcMinMaxRiseFall( fMin, fMax, Msta_SdcHasFlag( pCmd, "-rise" ),
                            Msta_SdcHasFlag( pCmd, "-fall" ), Sel );
    for ( i = 0; i < nClocks; i++ )
    {
        MstaClock *pClock = Msta_SdcClockByIndex( p, pClocks[i] );
        if ( Msta_SdcHasFlag( pCmd, "-source" ) )
        {
            Msta_SdcStoreMinMaxRiseFall( pClock->SourceLatency, Sel, Delay );
            pClock->SourceLatencyMax = fmax(pClock->SourceLatency[1][0],pClock->SourceLatency[1][1]);
            pClock->SourceLatencyMin = fmin(pClock->SourceLatency[0][0],pClock->SourceLatency[0][1]);
        }
        else
        {
            Msta_SdcStoreMinMaxRiseFall( pClock->NetworkLatency, Sel, Delay );
            pClock->NetworkLatencyMax = fmax(pClock->NetworkLatency[1][0],pClock->NetworkLatency[1][1]);
            pClock->NetworkLatencyMin = fmin(pClock->NetworkLatency[0][0],pClock->NetworkLatency[0][1]);
            /* 网络延迟是对时钟树延迟的估计，只对理想时钟有意义：给了它就是要用
               估计值代替沿时钟树算出的真实延迟，所以这个时钟按理想时钟处理。
               它也撤销此前的 set_propagated_clock（SDC 里后写的为准）。 */
            pClock->fPropagated = pClock->fPropagatedSet = 0;
        }
    }
}

/* set_propagated_clock 时钟列表
   把选中的时钟改成按时钟树传播。 */
void Msta_SdcSetPropagatedClock( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    int *pClocks, nClocks, i;
    nClocks = Msta_SdcObjectClocks( p, pCmd, pCmd->ppObjs, pCmd->nObjs, &pClocks );
    for ( i = 0; i < nClocks; i++ )
    {
        MstaClock *pClock = Msta_SdcClockByIndex( p, pClocks[i] );
        pClock->fPropagated = pClock->fPropagatedSet = 1;
    }
}

/* set_clock_transition [-rise|-fall] [-min|-max] 摆率 时钟列表
   给时钟源指定摆率；不写这里就用时钟网络上的输入摆率或默认值。
   虚拟时钟没有源，跳过。 */
void Msta_SdcSetClockTransition( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    int Sel[2][2], *pClocks, nClocks, i;
    double Slew;
    if ( !Msta_SdcGetNumber( p, pCmd, "值", pCmd->pValue, MSTA_SDC_NONNEG, &Slew ) )
        return;
    Msta_SdcCmdMinMaxRiseFall( pCmd, Sel );
    nClocks = Msta_SdcObjectClocks( p, pCmd, pCmd->ppObjs, pCmd->nObjs, &pClocks );
    for ( i = 0; i < nClocks; i++ )
    {
        MstaClock *pClock = Msta_SdcClockByIndex( p, pClocks[i] );
        if ( pClock->SourceNet < 0 )
            Msta_SdcNote( pCmd, "虚拟时钟 \"%s\" 没有源，已忽略", Msta_NameStr(pClock->Name) );
        else
            Msta_SdcStoreMinMaxRiseFall( pClock->Slew, Sel, Slew * p->TimeScalePs );
    }
}

/* set_clock_sense [-positive|-negative] [-stop_propagation] [-clock 时钟列表] 引脚列表
   记下某个脚/网络上的时钟极性（-negative）和"这个时钟到这里不再往下传"。
   不写 -clock 时对所有时钟生效。-pulse 不建模。 */
const MstaSdcOpt Msta_SdcClockSenseOpts[] = {
    { "-positive",         MSTA_SDC_FLAG,  0               },
    { "-negative",         MSTA_SDC_FLAG,  0               },
    { "-stop_propagation", MSTA_SDC_FLAG,  0               },
    { "-clock",            MSTA_SDC_OBJECTS, 0             },
    { "-pulse",            MSTA_SDC_VALUE, MSTA_SDC_REJECT },
    { NULL,                MSTA_SDC_FLAG,  0               } };

void Msta_SdcSetClockSense( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    int fPos  = Msta_SdcHasFlag( pCmd, "-positive" );
    int fNeg  = Msta_SdcHasFlag( pCmd, "-negative" );
    int fStop = Msta_SdcHasFlag( pCmd, "-stop_propagation" );
    char **ppClocks;
    int nClockNames = Msta_SdcOptList( pCmd, "-clock", &ppClocks );
    int *pClocks = NULL, nClocks = 0, *pNets, nNets, i, j;

    if ( fPos && fNeg )
    { Msta_SdcReject( p, pCmd, "-positive 与 -negative 不能同时使用" ); return; }
    if ( !fPos && !fNeg && !fStop )
    { Msta_SdcReject( p, pCmd, "需要 -positive、-negative 或 -stop_propagation" ); return; }
    for ( i = 0; i < pCmd->nObjs; i++ )
        if ( Msta_SdcKindOf( pCmd->ppObjs[i] ) == 'C' )
        { Msta_SdcReject( p, pCmd, "时钟对象未建模（请用 -clock）" ); return; }
    if ( nClockNames > 0 &&
         ( nClocks = Msta_SdcObjectClocks( p, pCmd, ppClocks, nClockNames, &pClocks ) ) < 0 )
        return;
    if ( ( nNets = Msta_SdcObjectNets( p, pDes, pCmd, pCmd->ppObjs, pCmd->nObjs, &pNets ) ) < 0 )
        return;
    for ( i = 0; i < nNets; i++ )
        for ( j = 0; j < ( nClocks > 0 ? nClocks : 1 ); j++ )
        {
            MstaClockSense *pSense = MstaClockSenseArrayAppend( &p->vClockSense );
            pSense->Net = pNets[i];
            pSense->Clock = nClocks > 0 ? Msta_SdcClockByIndex( p, pClocks[j] )->Name : MSTA_NO_ID;
            pSense->Polarity = fNeg ? -1 : ( fPos ? 1 : 0 );
            pSense->fStop = fStop;
        }
}

/* set_clock_groups -asynchronous|-logically_exclusive|-physically_exclusive
                    [-name 名字] -group 时钟列表 -group 时钟列表 ...
   不同时钟组之间没有 setup/hold 关系，两两展开成例外。-allow_paths 不建模。 */
const MstaSdcOpt Msta_SdcClockGroupsOpts[] = {
    { "-asynchronous",         MSTA_SDC_FLAG,  0               },
    { "-logically_exclusive",  MSTA_SDC_FLAG,  0               },
    { "-physically_exclusive", MSTA_SDC_FLAG,  0               },
    { "-name",                 MSTA_SDC_VALUE, 0               },
    { "-allow_paths",          MSTA_SDC_FLAG,  MSTA_SDC_REJECT },
    { "-group",                MSTA_SDC_LIST,  MSTA_SDC_REPEAT },
    { NULL,                    MSTA_SDC_FLAG,  0               } };

void Msta_SdcSetClockGroups( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    int *pGroups = (int *)Msta_SdcArenaZeros( p->vClocks.nSize, sizeof(int) );
    int *pClocks, nClocks, nGroups = 0, l, i, j;

    if ( !Msta_SdcHasFlag( pCmd, "-asynchronous" ) && !Msta_SdcHasFlag( pCmd, "-logically_exclusive" ) &&
         !Msta_SdcHasFlag( pCmd, "-physically_exclusive" ) )
    { Msta_SdcReject( p, pCmd, "需要 -asynchronous、-logically_exclusive 或 -physically_exclusive" ); return; }
    /* 每个 -group 是一组；pGroups[时钟] 记它属于第几组（从 1 数）。 */
    for ( l = 0; l < pCmd->nLists; l++ )
    {
        nClocks = Msta_SdcObjectClocks( p, pCmd, pCmd->pppListWords[l], pCmd->pListCount[l], &pClocks );
        if ( nClocks < 0 )
            return;
        nGroups++;
        for ( i = 0; i < nClocks; i++ )
            pGroups[pClocks[i]] = nGroups;
    }
    if ( nGroups < 2 )
    { Msta_SdcReject( p, pCmd, "至少需要两个 -group 列表" ); return; }
    for ( i = 0; i < p->vClocks.nSize; i++ )
        for ( j = 0; j < p->vClocks.nSize; j++ )
        {
            MstaException *pEx;
            if ( pGroups[i] == 0 || pGroups[j] == 0 || pGroups[i] == pGroups[j] )
                continue;
            pEx = MstaExceptionArrayAppend(&p->vExceptions);
            pEx->FromText = Msta_SdcClockByIndex(p,i)->Name;
            pEx->ToText = Msta_SdcClockByIndex(p,j)->Name;
            pEx->FromKind = pEx->ToKind = 'C';
            pEx->nSetupCycles = 1;
            pEx->fFalseSetup = pEx->fFalseHold = 1;
        }
}
