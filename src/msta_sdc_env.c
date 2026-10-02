/**CFile***************************************************************

  FileName    [msta_sdc_env.c]

  Synopsis    [SDC 设计规则、derate、工作条件与单位类命令。]

  每条命令一张选项表加一个处理函数；位置参数的形状写在分发表 s_vSdcCommands
  （msta_sdc.c）里。处理函数被调用时语法已经由 msta_sdc_parse.c 的通用解析器
  检查过，只需检查取值、查对象、写模型。
  Msta_SdcDefaultOpCond 也给 msta_sdc_query.c 报告工作条件用。

***********************************************************************/

#include <math.h>
#include <strings.h>
#include "msta_sdc_int.h"

/* ---------------- 设计规则、derate、工作条件与单位 ---------------- */

/* set_max_transition / set_max_fanout / set_max_capacitance / set_min_capacitance
       [-clock_path] [-data_path] [-rise] [-fall] 限制值 对象列表
   [current_design] 是全局限制；给端口/网络/引脚/单元时记在对应的网络上
   （单元记在它驱动的网络上），检查在时序分析之后统一做（按对象优先，没写对象
   的用全局值）。时钟对象（时钟域限制）不建模。-clock_path/-data_path/-rise/-fall
   不建模，msta 一律按最坏角检查。 */
const MstaSdcOpt Msta_SdcDrcLimitOpts[] = {
    { "-clock_path", MSTA_SDC_FLAG, MSTA_SDC_IGNORE },
    { "-data_path",  MSTA_SDC_FLAG, MSTA_SDC_IGNORE },
    { "-rise",       MSTA_SDC_FLAG, MSTA_SDC_IGNORE },
    { "-fall",       MSTA_SDC_FLAG, MSTA_SDC_IGNORE },
    { NULL,          MSTA_SDC_FLAG, 0               } };

/* nWhich：0 = max_transition，1 = max_fanout，2 = max_capacitance，3 = min_capacitance。
   pTarget 是 MstaSdc 的全局字段或 MstaNetCons 的分对象字段（四个按 nWhich 排）。 */
static void Msta_SdcStoreDrcLimit( double *pMaxTran, double *pMaxFanout, double *pMaxCap,
                                   double *pMinCap, int nWhich, double Value )
{
    if      ( nWhich == 0 ) *pMaxTran   = Value;
    else if ( nWhich == 1 ) *pMaxFanout = Value;
    else if ( nWhich == 2 ) *pMaxCap    = Value;
    else                    *pMinCap    = Value;
}

static void Msta_SdcSetDrcLimit( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib,
                                 int nWhich, MstaSdcCmd *pCmd )
{
    MstaSdcIntArray vNets;
    double Value;
    int i, j, fDesign = 0;

    if ( !Msta_SdcGetNumber( p, pCmd, "限值", pCmd->pValue, MSTA_SDC_POSITIVE, &Value ) )
        return;
    if ( nWhich == 0 )
        Value *= p->TimeScalePs;
    else if ( nWhich >= 2 )
        Value *= p->CapScaleFf > 0.0 ? p->CapScaleFf : pLib->CapScale;
    for ( i = 0; i < pCmd->nObjs; i++ )
        if ( Msta_SdcKindOf( pCmd->ppObjs[i] ) == 'C' )
        { Msta_SdcReject( p, pCmd, "时钟对象（按时钟域设限值）未建模" ); return; }
    /* 先把所有对象都解析成网络，一个都没有时整条作废。 */
    MstaSdcIntArrayInit( &vNets );
    for ( i = 0; i < pCmd->nObjs; i++ )
    {
        const char *pObj = pCmd->ppObjs[i];
        char Kind = Msta_SdcKindOf( pObj );
        if ( Kind == 'D' )
            fDesign = 1;
        else if ( Kind == 'I' )
        {
            int nInst = Msta_DesignFindInstByName( pDes, pObj );
            MstaInst *pInst;
            if ( nInst < 0 )
            { Msta_SdcNote( pCmd, "找不到实例 \"%s\"", pObj ); continue; }
            pInst = MstaInstArrayAt( &pDes->vInsts, nInst );
            for ( j = 0; j < pInst->nPins && j < pInst->pCell->vPins.nSize; j++ )
                if ( pInst->pNets[j] >= 0 && pInst->pCell->vPins.pData[j].Dir == MSTA_DIR_OUTPUT )
                    *MstaSdcIntArrayAppend( &vNets ) = pInst->pNets[j];
        }
        else
            Msta_SdcResolveNetsInto( pDes, pObj, &vNets );
    }
    Msta_SdcArenaKeep( vNets.pData );
    if ( !fDesign && vNets.nSize == 0 )
    { Msta_SdcReject( p, pCmd, "一个对象都没有找到" ); return; }
    if ( fDesign )
        Msta_SdcStoreDrcLimit( &p->MaxTransition, &p->MaxFanout, &p->MaxCapacitance,
                               &p->MinCapacitance, nWhich, Value );
    for ( i = 0; i < vNets.nSize; i++ )
    {
        MstaNetCons *pCons = Msta_SdcNetConsOrCreate( p, vNets.pData[i] );
        Msta_SdcStoreDrcLimit( &pCons->DrcMaxTransition, &pCons->DrcMaxFanout,
                               &pCons->DrcMaxCapacitance, &pCons->DrcMinCapacitance, nWhich, Value );
    }
}

void Msta_SdcSetMaxTransition( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    Msta_SdcSetDrcLimit( p, pDes, pLib, 0, pCmd );
}

void Msta_SdcSetMaxFanout( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    Msta_SdcSetDrcLimit( p, pDes, pLib, 1, pCmd );
}

void Msta_SdcSetMaxCapacitance( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    Msta_SdcSetDrcLimit( p, pDes, pLib, 2, pCmd );
}

void Msta_SdcSetMinCapacitance( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    Msta_SdcSetDrcLimit( p, pDes, pLib, 3, pCmd );
}

/* set_max_area 面积
   整个设计的面积目标，单位跟随库。 */
void Msta_SdcSetMaxArea( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    double Area;
    if ( Msta_SdcGetNumber( p, pCmd, "面积", pCmd->pValue, MSTA_SDC_NONNEG, &Area ) )
        p->MaxArea = Area;
}

/* set_timing_derate [-early|-late] [-cell_delay|-net_delay|-cell_check]
                     [-clock|-data] [-rise|-fall] 系数 [实例或时钟列表]
   不带对象时是全局系数；带对象时只对那个实例/时钟生效（分对象的值覆盖全局值，
   不是相乘）。-clock 管时钟树上的弧，-data 管数据路径上的单元延迟
   （含 FF 的 clk-to-Q），不写这两个开关时两者都算。msta 没有线延迟，
   只写 -net_delay 的约束作废。 */
const MstaSdcOpt Msta_SdcTimingDerateOpts[] = {
    { "-early",      MSTA_SDC_FLAG, 0 },
    { "-late",       MSTA_SDC_FLAG, 0 },
    { "-cell_delay", MSTA_SDC_FLAG, 0 },
    { "-cell_check", MSTA_SDC_FLAG, 0 },
    { "-net_delay",  MSTA_SDC_FLAG, 0 },
    { "-clock",      MSTA_SDC_FLAG, 0 },
    { "-data",       MSTA_SDC_FLAG, 0 },
    { "-rise",       MSTA_SDC_FLAG, 0 },
    { "-fall",       MSTA_SDC_FLAG, 0 },
    { NULL,          MSTA_SDC_FLAG, 0 } };

void Msta_SdcSetTimingDerate( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    int fEarly     = Msta_SdcHasFlag( pCmd, "-early" ) || !Msta_SdcHasFlag( pCmd, "-late" );
    int fLate      = Msta_SdcHasFlag( pCmd, "-late" ) || !Msta_SdcHasFlag( pCmd, "-early" );
    int fCellCheck = Msta_SdcHasFlag( pCmd, "-cell_check" );
    int fCellDelay = Msta_SdcHasFlag( pCmd, "-cell_delay" ) ||
                     ( !fCellCheck && !Msta_SdcHasFlag( pCmd, "-net_delay" ) );
    int fClock     = Msta_SdcHasFlag( pCmd, "-clock" ) || !Msta_SdcHasFlag( pCmd, "-data" );
    int fData      = Msta_SdcHasFlag( pCmd, "-data" ) || !Msta_SdcHasFlag( pCmd, "-clock" );
    char Edge      = Msta_SdcCmdEdge( pCmd );
    double Factor;
    int i, nApplied = 0;

    if ( !fCellDelay && !fCellCheck )
    { Msta_SdcReject( p, pCmd, "-net_delay 未建模（msta 不计算线网延迟）" ); return; }
    if ( !Msta_SdcGetNumber( p, pCmd, "系数", pCmd->pValue, MSTA_SDC_POSITIVE, &Factor ) )
        return;
    /* 分对象只支持实例（get_cells）和时钟（get_clocks）；其他种类的对象在 msta
       的模型里没有对应量，整条作废。 */
    for ( i = 0; i < pCmd->nObjs; i++ )
    {
        char Kind = Msta_SdcKindOf( pCmd->ppObjs[i] );
        if ( Kind != 0 && Kind != 'C' && Kind != 'I' )
        {
            Msta_SdcReject( p, pCmd, "对象 \"%s\" 未建模（只支持实例和时钟）",
                            pCmd->ppObjs[i] );
            return;
        }
    }
    if ( pCmd->nObjs == 0 )
    {
        p->fDerateClock = fCellDelay && fClock;
        p->fDerateData  = fCellDelay && fData;
        if ( fEarly ) p->DerateEarly = Factor;
        if ( fLate )  p->DerateLate  = Factor;
        if ( fCellCheck && fEarly ) p->DerateCheckEarly = Factor;
        if ( fCellCheck && fLate )  p->DerateCheckLate  = Factor;
        return;
    }
    for ( i = 0; i < pCmd->nObjs; i++ )
    {
        const char *pObj = pCmd->ppObjs[i];
        MstaClock *pClock = Msta_SdcKindOf( pObj ) == 'C' ? Msta_SdcFindClock( p, pObj ) : NULL;
        int nInst = pClock ? -1 : Msta_DesignFindInstByName( pDes, pObj );
        MstaObjDerate *pRec;
        if ( pClock == NULL && nInst < 0 )
        {
            Msta_SdcNote( pCmd, Msta_SdcKindOf( pObj ) == 'C' ? "找不到时钟 \"%s\"" : "找不到实例 \"%s\"", pObj );
            continue;
        }
        if ( pClock && Edge != 0 )
            Msta_SdcNote( pCmd, "时钟对象上的 -rise/-fall 未建模；"
                          "时钟树不区分上升和下降" );
        pRec = MstaObjDerateArrayAppend( &p->vObjDerate );
        pRec->Kind  = pClock ? 'C' : 'I';
        pRec->Inst  = nInst;
        pRec->Name  = pClock ? pClock->Name : MSTA_NO_ID;
        pRec->fRise = Edge == 'r';
        pRec->fFall = Edge == 'f';
        pRec->fCellCheck = fCellCheck;
        pRec->Early = fEarly ? Factor : 1.0;
        pRec->Late  = fLate  ? Factor : 1.0;
        nApplied++;
    }
    if ( nApplied == 0 )
        Msta_SdcReject( p, pCmd, "一个对象都没有找到" );
}

/* 没点名工作条件时用的角：库里 default_operating_conditions 指的那个，没有就取
   第一个。nLibrary 不是 MSTA_NO_ID 时只看这个库。返回角所在的库，角写进 *ppCond。 */
MstaLibInfo *Msta_SdcDefaultOpCond( MstaLib *pLib, MstaId nLibrary, MstaOpCond **ppCond )
{
    int i, j;
    for ( i = 0; i < pLib->vLibs.nSize; i++ )
    {
        MstaLibInfo *pOne = MstaLibInfoArrayAt( &pLib->vLibs, i );
        if ( nLibrary != MSTA_NO_ID && pOne->Name != nLibrary )
            continue;
        for ( j = 0; j < pOne->vOpConds.nSize; j++ )
            if ( MstaOpCondArrayAt(&pOne->vOpConds,j)->Name == pOne->OpCondName )
            {
                *ppCond = MstaOpCondArrayAt( &pOne->vOpConds, j );
                return pOne;
            }
        if ( pOne->vOpConds.nSize > 0 )
        {
            *ppCond = MstaOpCondArrayAt( &pOne->vOpConds, 0 );
            return pOne;
        }
    }
    return NULL;
}

/* 按库里的 K 因子算延迟缩放系数：
     derate = 1 + k_volt*(V - Vnom) + k_temp*(T - Tnom)   （process 项不建模）
   库没声明 K 因子时系数保持 1.0，电压/温度只记录、报告。pCmd 是触发它的命令
   （告警里用）；fWarnNoK=0 时"库里没有 K 因子"由调用方自己告警。
   返回 1 表示至少一个角用上了库里的 K 因子。 */
static int Msta_SdcUpdateKFactor( MstaSdc *p, MstaLib *pLib, const MstaSdcCmd *pCmd, int fWarnNoK )
{
    int c, fHasK = 0;
    if ( pLib == NULL )
        return 0;
    for ( c = 0; c < 2; c++ )
    {
        int fMax = ( c == 0 );
        double v = fMax ? p->VoltageMax : p->VoltageMin;
        double t = fMax ? p->TempMax : p->TempMin;
        MstaId nName = fMax ? p->OpCondMax : p->OpCondMin;
        MstaId nLibrary = fMax ? p->OpCondLibraryMax : p->OpCondLibraryMin;
        MstaOpCond *pCond = NULL;
        MstaLibInfo *pInfo = ( nName != MSTA_NO_ID )
                           ? Msta_LibFindOpCond(pLib,Msta_NameStr(nName),nLibrary,&pCond)
                           : NULL;
        double Derate = 1.0, dV, dT;
        /* 没点名就用库里默认的角（K 因子/标称值从它身上取）。 */
        if ( pInfo == NULL )
            pInfo = Msta_SdcDefaultOpCond( pLib, nLibrary, &pCond );
        if ( pInfo == NULL || pCond == NULL )
            continue;
        if ( !Msta_IsSet(v) ) v = pCond->Voltage;
        if ( !Msta_IsSet(t) ) t = pCond->Temperature;
        if ( pCond->KVolt < 0.0 && pCond->KTemp < 0.0 && pCond->KProcess < 0.0 )
        {
            /* 库里没有 K 因子：表值按读进来的原样用，只把请求的电压/温度报出来。 */
            if ( fWarnNoK &&
                 ( ( Msta_IsSet(v) && pInfo->NomVoltage >= 0.0 && v != pInfo->NomVoltage ) ||
                   ( Msta_IsSet(t) && pInfo->NomTemperature >= 0.0 && t != pInfo->NomTemperature ) ) )
                Msta_SdcNote( pCmd, "库里没有 k_volt/k_temp 系数；"
                              "延迟表按原值使用（指定的电压/温度只记录并在报告里显示）" );
            continue;
        }
        fHasK = 1;
        dV = ( Msta_IsSet(v) && pInfo->NomVoltage >= 0.0 ) ? v - pInfo->NomVoltage : 0.0;
        dT = ( Msta_IsSet(t) && pInfo->NomTemperature >= 0.0 ) ? t - pInfo->NomTemperature : 0.0;
        if ( pCond->KVolt >= 0.0 )    Derate += pCond->KVolt * dV;
        if ( pCond->KTemp >= 0.0 )    Derate += pCond->KTemp * dT;
        if ( Derate <= 0.0 ) Derate = 1.0;
        if ( fMax ) p->KFactorDerateLate = Derate;
        else        p->KFactorDerateEarly = Derate;
        if ( Derate != 1.0 )
            Msta_SdcNote( pCmd, "已按 K 系数把延迟乘以 %.4f（%.3f V / %.1f C）；"
                          "K 系数只是对特征化表格的线性近似",
                          Derate, Msta_IsSet(v) ? v : 0.0, Msta_IsSet(t) ? t : 0.0 );
    }
    return fHasK;
}

/* 统计声明某个 operating condition 的库，发现同名角的歧义。 */
static int Msta_SdcOpCondLibraryCount( MstaLib *pLib, const char *pName )
{
    int i, n = 0;
    for ( i = 0; i < pLib->vLibs.nSize; i++ )
        if ( Msta_LibFindOpCond(pLib,pName,MstaLibInfoArrayAt(&pLib->vLibs,i)->Name,NULL) )
            n++;
    return n;
}

/* 库名是否是已读入的某个库。 */
static int Msta_SdcLibraryExists( MstaLib *pLib, MstaId nLibrary )
{
    int j;
    for ( j = 0; j < pLib->vLibs.nSize; j++ )
        if ( MstaLibInfoArrayAt(&pLib->vLibs,j)->Name == nLibrary )
            return 1;
    return 0;
}

/* set_operating_conditions [条件名] [-max 条件名] [-min 条件名] [-library 库]
       [-max_library 库] [-min_library 库] [-analysis_type single|bc_wc]
       [-voltage 电压] [-temperature 温度]
   选择 max/min 两个角的工作条件与对应的 Liberty 库。不写 -max/-min 时，
   位置参数里的条件名两个角共用。-object_list 不建模；
   -analysis_type on_chip_variation 不建模。 */
const MstaSdcOpt Msta_SdcOperatingConditionsOpts[] = {
    { "-library",       MSTA_SDC_VALUE, 0               },
    { "-max_library",   MSTA_SDC_VALUE, 0               },
    { "-min_library",   MSTA_SDC_VALUE, 0               },
    { "-max",           MSTA_SDC_VALUE, 0               },
    { "-min",           MSTA_SDC_VALUE, 0               },
    { "-analysis_type", MSTA_SDC_VALUE, 0               },
    { "-voltage",       MSTA_SDC_VALUE, 0               },
    { "-temperature",   MSTA_SDC_VALUE, 0               },
    { "-object_list",   MSTA_SDC_OBJECTS, MSTA_SDC_IGNORE },
    { NULL,             MSTA_SDC_FLAG,  0               } };

void Msta_SdcSetOperatingConditions( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    const char *pLibName    = Msta_SdcOptValue( pCmd, "-library" );
    const char *pMaxLibName = Msta_SdcOptValue( pCmd, "-max_library" );
    const char *pMinLibName = Msta_SdcOptValue( pCmd, "-min_library" );
    const char *pMax        = Msta_SdcOptValue( pCmd, "-max" );
    const char *pMin        = Msta_SdcOptValue( pCmd, "-min" );
    const char *pAnalysis   = Msta_SdcOptValue( pCmd, "-analysis_type" );
    const char *pVolt       = Msta_SdcOptValue( pCmd, "-voltage" );
    const char *pTemp       = Msta_SdcOptValue( pCmd, "-temperature" );
    int fMaxLibrary = pLibName != NULL || pMaxLibName != NULL;
    int fMinLibrary = pLibName != NULL || pMinLibName != NULL;
    MstaId nLibraryMax = pMaxLibName ? Msta_NameId(pMaxLibName) : pLibName ? Msta_NameId(pLibName) : MSTA_NO_ID;
    MstaId nLibraryMin = pMinLibName ? Msta_NameId(pMinLibName) : pLibName ? Msta_NameId(pLibName) : MSTA_NO_ID;
    MstaLibInfo *pMaxInfo = NULL, *pMinInfo = NULL;
    double Volt = 0.0, Temp = 0.0;

    if ( pAnalysis && strcasecmp(pAnalysis,"on_chip_variation") == 0 )
    { Msta_SdcReject( p, pCmd, "-analysis_type on_chip_variation 未建模" ); return; }
    if ( pAnalysis && strcasecmp(pAnalysis,"single") != 0 && strcasecmp(pAnalysis,"bc_wc") != 0 )
    { Msta_SdcReject( p, pCmd, "未知的 -analysis_type \"%s\"", pAnalysis ); return; }
    if ( pAnalysis && strcasecmp(pAnalysis,"single") == 0 &&
         ( pMax != NULL || pMin != NULL || pMaxLibName != NULL || pMinLibName != NULL ) )
    { Msta_SdcReject( p, pCmd, "-analysis_type single 不能与 -max/-min 或 -max_library/-min_library 同时使用" ); return; }
    if ( ( pVolt && !Msta_SdcGetNumber( p, pCmd, "-voltage", pVolt, MSTA_SDC_POSITIVE, &Volt ) ) ||
         ( pTemp && !Msta_SdcGetNumber( p, pCmd, "-temperature", pTemp, MSTA_SDC_ANY, &Temp ) ) )
        return;
    if ( fMaxLibrary && !Msta_SdcLibraryExists( pLib, nLibraryMax ) )
    { Msta_SdcReject( p, pCmd, "找不到库 \"%s\"", Msta_NameStr(nLibraryMax) ); return; }
    if ( fMinLibrary && !Msta_SdcLibraryExists( pLib, nLibraryMin ) )
    { Msta_SdcReject( p, pCmd, "找不到库 \"%s\"", Msta_NameStr(nLibraryMin) ); return; }
    if ( pMax == NULL && pMin == NULL && pCmd->nObjs == 1 )
        pMax = pMin = pCmd->ppObjs[0];
    else if ( pCmd->nObjs == 1 )
    { Msta_SdcReject( p, pCmd, "工作条件名不能与 -max/-min 同时使用" ); return; }
    if ( pMax == NULL && pMin == NULL && pVolt == NULL && pTemp == NULL && !fMaxLibrary && !fMinLibrary )
    { Msta_SdcReject( p, pCmd, "需要工作条件名、库或 -voltage/-temperature" ); return; }
    if ( pMax != NULL && ( pMaxInfo = Msta_LibFindOpCond( pLib, pMax, nLibraryMax, NULL ) ) == NULL )
    { Msta_SdcReject( p, pCmd, "没有哪个库声明了工作条件 \"%s\"", pMax ); return; }
    if ( pMin != NULL && ( pMinInfo = Msta_LibFindOpCond( pLib, pMin, nLibraryMin, NULL ) ) == NULL )
    { Msta_SdcReject( p, pCmd, "没有哪个库声明了工作条件 \"%s\"", pMin ); return; }

    /* 给出的名字都有效，库与工艺角的改动一起生效。 */
    if ( fMaxLibrary ) p->OpCondLibraryMax = nLibraryMax;
    if ( fMinLibrary ) p->OpCondLibraryMin = nLibraryMin;
    if ( pMaxInfo != NULL )
    {
        p->OpCondMax = Msta_NameId(pMax);
        if ( !fMaxLibrary )
        {
            if ( Msta_SdcOpCondLibraryCount(pLib,pMax) > 1 )
                Msta_SdcNote( pCmd, "max 工作条件 \"%s\" 出现在多个库里；请用 -max_library 指定", pMax );
            p->OpCondLibraryMax = pMaxInfo->Name;
        }
    }
    else if ( fMaxLibrary && p->OpCondMax != MSTA_NO_ID &&
              Msta_LibFindOpCond(pLib,Msta_NameStr(p->OpCondMax),nLibraryMax,NULL) == NULL )
        p->OpCondMax = MSTA_NO_ID;
    if ( pMinInfo != NULL )
    {
        p->OpCondMin = Msta_NameId(pMin);
        if ( !fMinLibrary )
        {
            if ( Msta_SdcOpCondLibraryCount(pLib,pMin) > 1 )
                Msta_SdcNote( pCmd, "min 工作条件 \"%s\" 出现在多个库里；请用 -min_library 指定", pMin );
            p->OpCondLibraryMin = pMinInfo->Name;
        }
    }
    else if ( fMinLibrary && p->OpCondMin != MSTA_NO_ID &&
              Msta_LibFindOpCond(pLib,Msta_NameStr(p->OpCondMin),nLibraryMin,NULL) == NULL )
        p->OpCondMin = MSTA_NO_ID;

    /* 只给电压/温度的写法，对带 K 因子的库同样有效。 */
    if ( pVolt != NULL ) p->VoltageMax = p->VoltageMin = Volt;
    if ( pTemp != NULL ) p->TempMax = p->TempMin = Temp;
    Msta_SdcUpdateKFactor( p, pLib, pCmd, 1 );
}

/* set_voltage 最高电压 [-min 最低电压] [-object_list 电源网络]
   记录设计的工作电压。分对象的电压要电源域模型，不建模。 */
const MstaSdcOpt Msta_SdcVoltageOpts[] = {
    { "-min",         MSTA_SDC_VALUE, 0               },
    { "-object_list", MSTA_SDC_OBJECTS, MSTA_SDC_IGNORE },
    { NULL,           MSTA_SDC_FLAG,  0               } };

void Msta_SdcSetVoltage( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    const char *pMin = Msta_SdcOptValue( pCmd, "-min" );
    double Max, Min;
    if ( pCmd->nObjs > 0 )
    { Msta_SdcReject( p, pCmd, "按对象设置电压未建模" ); return; }
    if ( !Msta_SdcGetNumber( p, pCmd, "电压", pCmd->pValue, MSTA_SDC_POSITIVE, &Max ) ||
         ( pMin && !Msta_SdcGetNumber( p, pCmd, "-min", pMin, MSTA_SDC_POSITIVE, &Min ) ) )
        return;
    p->VoltageMax = Max;
    p->VoltageMin = pMin ? Min : Max;
    /* 延迟表只能靠库里的 k 因子随电压缩放；没有 k 因子时 set_voltage 只被记录、
       不改变延迟，电压偏离标称值时要告警，免得用户以为结果已按新电压算过。
       有 k 因子时缩放系数由 Msta_SdcUpdateKFactor 报出。 */
    if ( !Msta_SdcUpdateKFactor( p, pLib, pCmd, 0 ) && pLib != NULL && pLib->vLibs.nSize > 0 )
    {
        double Nominal = MstaLibInfoArrayAt(&pLib->vLibs,0)->NomVoltage;
        if ( Nominal >= 0.0 && fabs(p->VoltageMax - Nominal) > 1e-6 )
            Msta_SdcNote( pCmd, "已记录 %.3f V，但库里没有 k_volt 系数；"
                          "延迟表按原值使用（标称 %.3f V）", p->VoltageMax, Nominal );
    }
}

/* 把时间/电容单位文本换算成内部比例（ps / fF）；不认识时返回 0。 */
static double Msta_SdcUnitScale( const char *pText, int fTime )
{
    char *pEnd;
    double Value = strtod(pText,&pEnd);
    if ( pEnd == pText ) { Value = 1.0; pEnd = (char *)pText; }
    if ( Value <= 0.0 ) return 0.0;
    if ( fTime )
    {
        if ( !strcasecmp(pEnd,"fs") ) return Value * 0.001;
        if ( !strcasecmp(pEnd,"ps") ) return Value;
        if ( !strcasecmp(pEnd,"ns") ) return Value * 1000.0;
        if ( !strcasecmp(pEnd,"us") ) return Value * 1000000.0;
    }
    else
    {
        if ( !strcasecmp(pEnd,"af") ) return Value * 0.001;
        if ( !strcasecmp(pEnd,"ff") ) return Value;
        if ( !strcasecmp(pEnd,"pf") ) return Value * 1000.0;
        if ( !strcasecmp(pEnd,"nf") ) return Value * 1000000.0;
    }
    return 0.0;
}

/* set_units [-time 单位] [-capacitance 单位]
   把单位用到后面命令的数值上。其他单位（-resistance 等）不建模。 */
const MstaSdcOpt Msta_SdcUnitsOpts[] = {
    { "-time",        MSTA_SDC_VALUE, 0               },
    { "-capacitance", MSTA_SDC_VALUE, 0               },
    { "-resistance",  MSTA_SDC_VALUE, MSTA_SDC_IGNORE },
    { "-voltage",     MSTA_SDC_VALUE, MSTA_SDC_IGNORE },
    { "-current",     MSTA_SDC_VALUE, MSTA_SDC_IGNORE },
    { "-power",       MSTA_SDC_VALUE, MSTA_SDC_IGNORE },
    { NULL,           MSTA_SDC_FLAG,  0               } };

void Msta_SdcSetUnits( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    const char *pTime = Msta_SdcOptValue( pCmd, "-time" );
    const char *pCap  = Msta_SdcOptValue( pCmd, "-capacitance" );
    double TimeScale = pTime ? Msta_SdcUnitScale( pTime, 1 ) : 0.0;
    double CapScale  = pCap  ? Msta_SdcUnitScale( pCap, 0 )  : 0.0;
    if ( pTime && TimeScale <= 0.0 )
    { Msta_SdcReject( p, pCmd, "不支持的时间单位 \"%s\"", pTime ); return; }
    if ( pCap && CapScale <= 0.0 )
    { Msta_SdcReject( p, pCmd, "不支持的电容单位 \"%s\"", pCap ); return; }
    if ( pTime ) p->TimeScalePs = TimeScale;
    if ( pCap )  p->CapScaleFf  = CapScale;
}
