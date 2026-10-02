/**CFile***************************************************************

  FileName    [msta_sdc_io.c]

  Synopsis    [SDC I/O 与网络属性类命令：I/O 延迟、负载、输入摆率、驱动单元、理想网络、常量与屏蔽时序弧。]

  每条命令一张选项表加一个处理函数；位置参数的形状写在分发表 s_vSdcCommands
  （msta_sdc.c）里。处理函数被调用时语法已经由 msta_sdc_parse.c 的通用解析器
  检查过，只需检查取值、查对象、写模型。

***********************************************************************/

#include "msta_sdc_int.h"

/* ---------------- I/O 与网络属性 ---------------- */

/* 存一条 I/O 延迟（ps）；-add_delay 时保留同一网络上的其他时钟。
   fMax/fMin 表示这条命令约束哪个角。 */
static void Msta_SdcStorePortDelay( MstaSdc *p, int nNet, MstaId Clock, int fOutput,
                                    int fMax, int fMin, double Value, int fAdd,
                                    int fClockFall, int DataRise, int RefNet,
                                    int fSourceLatencyIncluded, int fNetworkLatencyIncluded )
{
    MstaIoDelayArray *pArr = fOutput ? &p->vOutputDelays : &p->vInputDelays;
    MstaIoDelay *pEntry = NULL;
    int i;
    for ( i = 0; i < pArr->nSize; i++ )
    {
        MstaIoDelay *pOld = MstaIoDelayArrayAt(pArr,i);
        if ( pOld->Net != nNet ) continue;
        if ( pOld->Clock == Clock && pOld->ClockFall == fClockFall && pOld->DataRise == DataRise ) pEntry = pOld;
        else if ( !fAdd )
        {
            if ( fMax ) pOld->Max = MSTA_UNSET;
            if ( fMin ) pOld->Min = MSTA_UNSET;
        }
    }
    if ( pEntry == NULL )
    {
        pEntry = MstaIoDelayArrayAppend(pArr);
        pEntry->Net = nNet;
        pEntry->Clock = Clock;
        pEntry->Max = pEntry->Min = MSTA_UNSET;
        pEntry->ClockFall = fClockFall;
        pEntry->DataRise = DataRise;
        pEntry->RefNetMax = pEntry->RefNetMin = -1;
    }
    if ( fMax )
    {
        if ( !fAdd || !Msta_IsSet(pEntry->Max) || Value > pEntry->Max )
            pEntry->Max = Value;
        pEntry->RefNetMax = RefNet;
        pEntry->SourceLatencyIncludedMax = fSourceLatencyIncluded;
        pEntry->NetworkLatencyIncludedMax = fNetworkLatencyIncluded;
    }
    if ( fMin )
    {
        if ( !fAdd || !Msta_IsSet(pEntry->Min) || Value < pEntry->Min )
            pEntry->Min = Value;
        pEntry->RefNetMin = RefNet;
        pEntry->SourceLatencyIncludedMin = fSourceLatencyIncluded;
        pEntry->NetworkLatencyIncludedMin = fNetworkLatencyIncluded;
    }
}

/* set_input_delay / set_output_delay [-clock 时钟] [-clock_fall] [-rise|-fall]
       [-max] [-min] [-add_delay] [-reference_pin 引脚]
       [-source_latency_included] [-network_latency_included] 延迟 端口列表
   不写 -max/-min 时一个值同时约束两个角；写了就只改对应角。
   -level_sensitive 不建模。 */
const MstaSdcOpt Msta_SdcPortDelayOpts[] = {
    { "-clock",                    MSTA_SDC_VALUE, 0               },
    { "-reference_pin",            MSTA_SDC_VALUE, 0               },
    { "-clock_fall",               MSTA_SDC_FLAG,  0               },
    { "-rise",                     MSTA_SDC_FLAG,  0               },
    { "-fall",                     MSTA_SDC_FLAG,  0               },
    { "-max",                      MSTA_SDC_FLAG,  0               },
    { "-min",                      MSTA_SDC_FLAG,  0               },
    { "-add_delay",                MSTA_SDC_FLAG,  0               },
    { "-source_latency_included",  MSTA_SDC_FLAG,  0               },
    { "-network_latency_included", MSTA_SDC_FLAG,  0               },
    { "-level_sensitive",          MSTA_SDC_FLAG,  MSTA_SDC_REJECT },
    { NULL,                        MSTA_SDC_FLAG,  0               } };

/* fOutput=0 是 set_input_delay，=1 是 set_output_delay。 */
static void Msta_SdcSetPortDelay( MstaSdc *p, MstaDesign *pDes, int fOutput, MstaSdcCmd *pCmd )
{
    const char *pClockText    = Msta_SdcOptValue( pCmd, "-clock" );
    const char *pReferencePin = Msta_SdcOptValue( pCmd, "-reference_pin" );
    int fSourceIncluded  = Msta_SdcHasFlag( pCmd, "-source_latency_included" );
    int fNetworkIncluded = Msta_SdcHasFlag( pCmd, "-network_latency_included" );
    int fMax = Msta_SdcHasFlag( pCmd, "-max" ) || !Msta_SdcHasFlag( pCmd, "-min" );
    int fMin = Msta_SdcHasFlag( pCmd, "-min" ) || !Msta_SdcHasFlag( pCmd, "-max" );
    char Edge = Msta_SdcCmdEdge( pCmd );
    int DataRise = Edge == 'r' ? 1 : Edge == 'f' ? 0 : -1;
    int RefNet = -1, *pNets, nNets, i;

    if ( pClockText && Msta_SdcFindClock( p, pClockText ) == NULL )
    { Msta_SdcReject( p, pCmd, "找不到时钟 \"%s\"", pClockText ); return; }
    if ( pReferencePin != NULL )
    {
        char RefKind = Msta_SdcKindOf( pReferencePin );
        if ( ( RefKind != 0 && RefKind != 'G' && RefKind != 'P' ) ||
             Msta_SdcResolveNets( pDes, pReferencePin, &pNets ) != 1 )
        { Msta_SdcReject( p, pCmd, "-reference_pin 必须对应唯一一个引脚或端口" ); return; }
        RefNet = pNets[0];
        if ( fSourceIncluded || fNetworkIncluded )
            Msta_SdcNote( pCmd, "用了 -reference_pin 时 -source_latency_included/-network_latency_included 不起作用，已忽略" );
        fSourceIncluded = fNetworkIncluded = 0;
    }
    /* 不写 -clock 时先记成"未指定"：SDC 允许 create_clock 写在 I/O 约束之后，
       所以真正用哪个时钟留到查询时再按当时只有一个时钟来判定。 */
    if ( pClockText == NULL && pReferencePin == NULL && Msta_SdcClockCount(p) > 1 )
        Msta_SdcNote( pCmd, "没写 -clock，但已有多个时钟；"
                      "这条延迟只在设计中只有一个时钟时生效" );
    if ( ( nNets = Msta_SdcObjectNets( p, pDes, pCmd, pCmd->ppObjs, pCmd->nObjs, &pNets ) ) < 0 )
        return;
    for ( i = 0; i < nNets; i++ )
    {
        Msta_SdcNetConsOrCreate( p, pNets[i] );
        Msta_SdcStorePortDelay( p, pNets[i], pClockText ? Msta_NameId(pClockText) : MSTA_NO_ID,
                                fOutput, fMax, fMin, Msta_SdcToPs( p, pCmd->pValue ),
                                Msta_SdcHasFlag( pCmd, "-add_delay" ),
                                Msta_SdcHasFlag( pCmd, "-clock_fall" ), DataRise, RefNet,
                                fSourceIncluded, fNetworkIncluded );
    }
}

void Msta_SdcSetInputDelay( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    Msta_SdcSetPortDelay( p, pDes, 0, pCmd );
}

void Msta_SdcSetOutputDelay( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    Msta_SdcSetPortDelay( p, pDes, 1, pCmd );
}

/* set_load [-min|-max] [-subtract_pin_load] [-pin_load|-wire_load] 值 对象列表
   值默认按库的电容单位，set_units 可覆盖。
   -pin_load / -wire_load 是"这到底是脚负载还是线负载"的标注；msta 把它们
   都当外加负载加在网络上。-subtract_pin_load 只对网络有意义（减的是网络上
   的脚电容），不能用在端口上。 */
const MstaSdcOpt Msta_SdcLoadOpts[] = {
    { "-min",               MSTA_SDC_FLAG, 0 },
    { "-max",               MSTA_SDC_FLAG, 0 },
    { "-subtract_pin_load", MSTA_SDC_FLAG, 0 },
    { "-pin_load",          MSTA_SDC_FLAG, 0 },
    { "-wire_load",         MSTA_SDC_FLAG, 0 },
    { NULL,                 MSTA_SDC_FLAG, 0 } };

void Msta_SdcSetLoad( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    int fSubtract = Msta_SdcHasFlag( pCmd, "-subtract_pin_load" );
    int fMax = Msta_SdcHasFlag( pCmd, "-max" ) || !Msta_SdcHasFlag( pCmd, "-min" );
    int fMin = Msta_SdcHasFlag( pCmd, "-min" ) || !Msta_SdcHasFlag( pCmd, "-max" );
    int *pNets, nNets, i;
    double Load;
    if ( Msta_SdcHasFlag( pCmd, "-pin_load" ) && Msta_SdcHasFlag( pCmd, "-wire_load" ) )
    { Msta_SdcReject( p, pCmd, "-pin_load 与 -wire_load 不能同时使用" ); return; }
    if ( !Msta_SdcGetNumber( p, pCmd, "值", pCmd->pValue, MSTA_SDC_NONNEG, &Load ) )
        return;
    for ( i = 0; fSubtract && i < pCmd->nObjs; i++ )
        if ( Msta_SdcKindOf( pCmd->ppObjs[i] ) == 'P' )
        { Msta_SdcReject( p, pCmd, "-subtract_pin_load 不能用在端口 \"%s\" 上", pCmd->ppObjs[i] ); return; }
    Load *= p->CapScaleFf > 0.0 ? p->CapScaleFf : pLib->CapScale;
    if ( ( nNets = Msta_SdcObjectNets( p, pDes, pCmd, pCmd->ppObjs, pCmd->nObjs, &pNets ) ) < 0 )
        return;
    for ( i = 0; i < nNets; i++ )
    {
        MstaNetCons *pCons = Msta_SdcNetConsOrCreate( p, pNets[i] );
        if ( fMax ) pCons->LoadMax = Load;
        if ( fMin ) pCons->LoadMin = Load;
        if ( fSubtract ) pCons->fSubtractPinLoad = 1;
    }
}

/* set_input_transition [-rise|-fall] [-min|-max] [-clock 时钟] [-clock_fall] 摆率 端口列表
   可以按 -rise/-fall、-max/-min 分别给值，时序引擎用逐边沿字段起步。
   -clock / -clock_fall 只是标注，认下来不起作用。 */
const MstaSdcOpt Msta_SdcInputTransitionOpts[] = {
    { "-min",        MSTA_SDC_FLAG,  0 },
    { "-max",        MSTA_SDC_FLAG,  0 },
    { "-rise",       MSTA_SDC_FLAG,  0 },
    { "-fall",       MSTA_SDC_FLAG,  0 },
    { "-clock",      MSTA_SDC_VALUE, 0 },
    { "-clock_fall", MSTA_SDC_FLAG,  0 },
    { NULL,          MSTA_SDC_FLAG,  0 } };

void Msta_SdcSetInputTransition( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    int Sel[2][2], *pNets, nNets, i;
    double Slew;
    if ( !Msta_SdcGetNumber( p, pCmd, "值", pCmd->pValue, MSTA_SDC_NONNEG, &Slew ) )
        return;
    if ( ( nNets = Msta_SdcObjectNets( p, pDes, pCmd, pCmd->ppObjs, pCmd->nObjs, &pNets ) ) < 0 )
        return;
    Msta_SdcCmdMinMaxRiseFall( pCmd, Sel );
    for ( i = 0; i < nNets; i++ )
    {
        MstaNetCons *pCons = Msta_SdcNetConsOrCreate( p, pNets[i] );
        Msta_SdcStoreMinMaxRiseFall4( Sel, Slew * p->TimeScalePs,
                                      &pCons->InputSlewMaxRise, &pCons->InputSlewMaxFall,
                                      &pCons->InputSlewMinRise, &pCons->InputSlewMinFall );
    }
}

/* set_driving_cell -lib_cell 单元 [-library 库] [-pin 输出脚] [-from_pin 输入脚]
       [-multiply_by 倍数] [-input_transition_rise 值] [-input_transition_fall 值]
       [-min|-max] 端口列表
   根据驱动单元的输出转换表设置输入端口摆率，并记下驱动单元供分析时算延迟。
   -min/-max 只影响哪一角的摆率，msta 两个角都记同一份驱动单元。 */
const MstaSdcOpt Msta_SdcDrivingCellOpts[] = {
    { "-lib_cell",              MSTA_SDC_VALUE, 0               },
    { "-library",               MSTA_SDC_VALUE, 0               },
    { "-pin",                   MSTA_SDC_VALUE, 0               },
    { "-from_pin",              MSTA_SDC_VALUE, 0               },
    { "-multiply_by",           MSTA_SDC_VALUE, 0               },
    { "-input_transition_rise", MSTA_SDC_VALUE, 0               },
    { "-input_transition_fall", MSTA_SDC_VALUE, 0               },
    { "-min",                   MSTA_SDC_FLAG,  0               },
    { "-max",                   MSTA_SDC_FLAG,  0               },
    { "-clock",                 MSTA_SDC_VALUE, MSTA_SDC_IGNORE },
    { "-rise",                  MSTA_SDC_FLAG,  MSTA_SDC_IGNORE },
    { "-fall",                  MSTA_SDC_FLAG,  MSTA_SDC_IGNORE },
    { "-dont_scale",            MSTA_SDC_FLAG,  MSTA_SDC_IGNORE },
    { "-no_design_rule",        MSTA_SDC_FLAG,  MSTA_SDC_IGNORE },
    { "-clock_fall",            MSTA_SDC_FLAG,  MSTA_SDC_IGNORE },
    { NULL,                     MSTA_SDC_FLAG,  0               } };

void Msta_SdcSetDrivingCell( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    const char *pCellName = Msta_SdcOptValue( pCmd, "-lib_cell" );
    const char *pLibName  = Msta_SdcOptValue( pCmd, "-library" );
    const char *pPinName  = Msta_SdcOptValue( pCmd, "-pin" );
    const char *pFromPin  = Msta_SdcOptValue( pCmd, "-from_pin" );
    const char *pMult     = Msta_SdcOptValue( pCmd, "-multiply_by" );
    const char *pInRise   = Msta_SdcOptValue( pCmd, "-input_transition_rise" );
    const char *pInFall   = Msta_SdcOptValue( pCmd, "-input_transition_fall" );
    double Mult = 1.0, InRise = 0.0, InFall = 0.0, Slew = 0.0;
    char sCellQual[512];
    MstaCell *pCell;
    int *pNets, nNets, i, k;

    if ( !Msta_SdcRequire( p, pCmd, "-lib_cell" ) )
        return;
    if ( ( pMult   && !Msta_SdcGetNumber( p, pCmd, "-multiply_by", pMult, MSTA_SDC_POSITIVE, &Mult ) ) ||
         ( pInRise && !Msta_SdcGetNumber( p, pCmd, "-input_transition_rise", pInRise, MSTA_SDC_NONNEG, &InRise ) ) ||
         ( pInFall && !Msta_SdcGetNumber( p, pCmd, "-input_transition_fall", pInFall, MSTA_SDC_NONNEG, &InFall ) ) )
        return;
    /* 多库时用 "库名/cell 名" 限定（SDC 手册的 -library + -lib_cell 组合）。 */
    if ( pLibName != NULL && strchr( pCellName, '/' ) == NULL )
    {
        snprintf( sCellQual, sizeof(sCellQual), "%s/%s", pLibName, pCellName );
        pCellName = sCellQual;
    }
    pCell = Msta_LibFindCell( pLib, pCellName );
    if ( pCell == NULL )
    { Msta_SdcReject( p, pCmd, "单元 \"%s\" 不在指定的库里", pCellName ); return; }
    for ( k = 0; k < pCell->vArcs.nSize && Slew <= 0.0; k++ )
    {
        MstaArc *pArc = MstaArcArrayAt( &pCell->vArcs, k );
        if ( pPinName && pArc->OutPin != Msta_NameId(pPinName) ) continue;
        if ( Msta_TableExists( &pArc->TransRise ) )
            Slew = pArc->TransRise.pValues[0] * pLib->TimeScale;
        else if ( Msta_TableExists( &pArc->TransFall ) )
            Slew = pArc->TransFall.pValues[0] * pLib->TimeScale;
    }
    if ( ( nNets = Msta_SdcObjectNets( p, pDes, pCmd, pCmd->ppObjs, pCmd->nObjs, &pNets ) ) < 0 )
        return;
    for ( i = 0; i < nNets; i++ )
    {
        MstaNetCons *pCons = Msta_SdcNetConsOrCreate( p, pNets[i] );
        pCons->DrivingCell    = Msta_NameId( pCellName );
        pCons->DrivingPin     = pPinName ? Msta_NameId( pPinName ) : MSTA_NO_ID;
        pCons->DrivingFromPin = pFromPin ? Msta_NameId( pFromPin ) : MSTA_NO_ID;
        pCons->DriveMultiply  = Mult;
        if ( pInRise ) pCons->DriveInSlewRise = InRise * p->TimeScalePs;
        if ( pInFall ) pCons->DriveInSlewFall = InFall * p->TimeScalePs;
        if ( !Msta_IsSet( pCons->InputSlewMaxRise ) ) pCons->InputSlewMaxRise = Slew;
        if ( !Msta_IsSet( pCons->InputSlewMaxFall ) ) pCons->InputSlewMaxFall = Slew;
        if ( !Msta_IsSet( pCons->InputSlewMinRise ) ) pCons->InputSlewMinRise = Slew;
        if ( !Msta_IsSet( pCons->InputSlewMinFall ) ) pCons->InputSlewMinFall = Slew;
    }
}

/* 理想网络类命令的对象都是网络；时钟对象没建模。 */
static int Msta_SdcIdealNets( MstaSdc *p, MstaDesign *pDes, const MstaSdcCmd *pCmd, int **ppNets )
{
    int i;
    for ( i = 0; i < pCmd->nObjs; i++ )
        if ( Msta_SdcKindOf( pCmd->ppObjs[i] ) == 'C' )
        { Msta_SdcReject( p, pCmd, "时钟对象未建模" ); return -1; }
    return Msta_SdcObjectNets( p, pDes, pCmd, pCmd->ppObjs, pCmd->nObjs, ppNets );
}

/* set_ideal_network [-no_propagate] 对象列表
   把对象标成理想网络；不带 -no_propagate 时理想属性沿组合扇出继续往下传。
   -no_propagation 是方言拼写，按 -no_propagate 处理；-force 不建模。 */
const MstaSdcOpt Msta_SdcIdealNetworkOpts[] = {
    { "-no_propagate",   MSTA_SDC_FLAG, 0               },
    { "-no_propagation", MSTA_SDC_FLAG, 0               },
    { "-force",          MSTA_SDC_FLAG, MSTA_SDC_IGNORE },
    { NULL,              MSTA_SDC_FLAG, 0               } };

void Msta_SdcSetIdealNetwork( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    int fNoProp = Msta_SdcHasFlag( pCmd, "-no_propagate" ) || Msta_SdcHasFlag( pCmd, "-no_propagation" );
    int *pNets, nNets, i;
    if ( Msta_SdcHasFlag( pCmd, "-no_propagation" ) )
        Msta_SdcNote( pCmd, "-no_propagation 不是 SDC 1.8 语法，按 -no_propagate 处理" );
    if ( ( nNets = Msta_SdcIdealNets( p, pDes, pCmd, &pNets ) ) < 0 )
        return;
    for ( i = 0; i < nNets; i++ )
    {
        MstaNetCons *pCons = Msta_SdcNetConsOrCreate( p, pNets[i] );
        pCons->fIdeal = 1;
        if ( fNoProp ) pCons->fIdealNoPropagate = 1;
    }
}

/* set_ideal_latency [-rise|-fall] [-min|-max] 延迟 对象列表 */
void Msta_SdcSetIdealLatency( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    int *pNets, nNets, i, Sel[2][2];
    if ( ( nNets = Msta_SdcIdealNets( p, pDes, pCmd, &pNets ) ) < 0 )
        return;
    Msta_SdcCmdMinMaxRiseFall( pCmd, Sel );
    for ( i = 0; i < nNets; i++ )
    {
        MstaNetCons *pCons = Msta_SdcNetConsOrCreate( p, pNets[i] );
        Msta_SdcStoreMinMaxRiseFall4( Sel, Msta_SdcToPs( p, pCmd->pValue ),
                                      &pCons->IdealLatencyMaxRise, &pCons->IdealLatencyMaxFall,
                                      &pCons->IdealLatencyMinRise, &pCons->IdealLatencyMinFall );
    }
}

/* set_ideal_transition [-rise|-fall] [-min|-max] 摆率 对象列表 */
void Msta_SdcSetIdealTransition( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    int *pNets, nNets, i, Sel[2][2];
    double Slew;
    if ( !Msta_SdcGetNumber( p, pCmd, "值", pCmd->pValue, MSTA_SDC_NONNEG, &Slew ) )
        return;
    if ( ( nNets = Msta_SdcIdealNets( p, pDes, pCmd, &pNets ) ) < 0 )
        return;
    Msta_SdcCmdMinMaxRiseFall( pCmd, Sel );
    for ( i = 0; i < nNets; i++ )
    {
        MstaNetCons *pCons = Msta_SdcNetConsOrCreate( p, pNets[i] );
        Msta_SdcStoreMinMaxRiseFall4( Sel, Slew * p->TimeScalePs,
                                      &pCons->IdealTranMaxRise, &pCons->IdealTranMaxFall,
                                      &pCons->IdealTranMinRise, &pCons->IdealTranMinFall );
    }
}

/* 把网络钉成常量。nCase 是 fCaseValue 编码：1 = 逻辑 0，2 = 逻辑 1，
   3 = dc（无关值，按"不传播"处理，路径到这里断开；见 msta_net.h）。 */
static void Msta_SdcSetConstNets( MstaSdc *p, MstaDesign *pDes, MstaSdcCmd *pCmd, int nCase )
{
    int *pNets, nNets, i;
    if ( ( nNets = Msta_SdcObjectNets( p, pDes, pCmd, pCmd->ppObjs, pCmd->nObjs, &pNets ) ) < 0 )
        return;
    for ( i = 0; i < nNets; i++ )
    {
        MstaNet *pNet = MstaNetArrayAt( &pDes->vNets, pNets[i] );
        pNet->fCaseValue = nCase;
        pNet->fConst = nCase == 3 ? 0 : nCase;
    }
}

/* set_case_analysis 0|1|zero|one 对象列表
   把选中的网络钉成常量。rise/fall 这类"只能沿某个边沿翻转"的值不建模。 */
void Msta_SdcSetCaseAnalysis( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    const char *pValue = pCmd->pValue;
    if ( !strcmp( pValue, "0" ) || !strcmp( pValue, "zero" ) )
        Msta_SdcSetConstNets( p, pDes, pCmd, 1 );
    else if ( !strcmp( pValue, "1" ) || !strcmp( pValue, "one" ) )
        Msta_SdcSetConstNets( p, pDes, pCmd, 2 );
    else if ( !strcmp( pValue, "rise" ) || !strcmp( pValue, "rising" ) ||
              !strcmp( pValue, "fall" ) || !strcmp( pValue, "falling" ) )
        Msta_SdcReject( p, pCmd, "值 \"%s\" 未建模（只支持 0、1、zero 和 one）", pValue );
    else
        Msta_SdcReject( p, pCmd, "值 \"%s\" 必须是 0、1、zero 或 one", pValue );
}

/* set_logic_zero / set_logic_one / set_logic_dc 端口列表 */
void Msta_SdcSetLogicZero( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    Msta_SdcSetConstNets( p, pDes, pCmd, 1 );
}

void Msta_SdcSetLogicOne( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    Msta_SdcSetConstNets( p, pDes, pCmd, 2 );
}

void Msta_SdcSetLogicDc( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    Msta_SdcSetConstNets( p, pDes, pCmd, 3 );
}

/* set_disable_timing [-from 引脚名] [-to 引脚名] 实例列表
   屏蔽指定实例上从 -from 到 -to 的时序弧。两个端点可以只写一个，
   缺的那个按通配处理（与 SDC 一致）。-from/-to 各是一个库引脚名。 */
const MstaSdcOpt Msta_SdcDisableTimingOpts[] = {
    { "-from", MSTA_SDC_VALUE, 0 },
    { "-to",   MSTA_SDC_VALUE, 0 },
    { NULL,    MSTA_SDC_FLAG,  0 } };

void Msta_SdcSetDisableTiming( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    const char *pFrom = Msta_SdcOptValue( pCmd, "-from" );
    const char *pTo   = Msta_SdcOptValue( pCmd, "-to" );
    int *pInsts, nInsts, i;
    if ( pFrom == NULL && pTo == NULL )
    { Msta_SdcReject( p, pCmd, "至少需要 -from 或 -to 之一" ); return; }
    if ( ( nInsts = Msta_SdcObjectInsts( p, pDes, pLib, pCmd, pCmd->ppObjs, pCmd->nObjs, 0, &pInsts ) ) < 0 )
        return;
    for ( i = 0; i < nInsts; i++ )
    {
        MstaInst *pInst = MstaInstArrayAt( &pDes->vInsts, pInsts[i] );
        MstaDisabledArc *pArc = MstaDisabledArcArrayAppend( &p->vDisabledArcs );
        pArc->Inst = pInsts[i];
        pArc->FromPin = pFrom ? Msta_NameId(pFrom) : MSTA_NO_ID;
        pArc->ToPin = pTo ? Msta_NameId(pTo) : MSTA_NO_ID;
        if ( ( pFrom && Msta_CellPinIndexOf(pInst->pCell,pArc->FromPin) < 0 ) ||
             ( pTo && Msta_CellPinIndexOf(pInst->pCell,pArc->ToPin) < 0 ) )
            Msta_SdcNote( pCmd, "实例 \"%s\" 没有匹配的引脚", Msta_InstName(pDes,pInsts[i]) );
    }
}
