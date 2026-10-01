/**CFile***************************************************************

  FileName    [msta_lib.c]

  Synopsis    [Liberty 语义层：把语法树变成 STA 能用的 cell/pin/arc/表。]

  本文件分四段，按调用顺序读：
    A. 数值与表的解析（Liberty 文本 -> MstaTable）
    B. 一个 cell 的解析（pin / timing() / ff() / latch()）
    C. 寄存器检查项的归并（setup/hold/clk2q 三者挂到一起）
    D. 对外查询接口 + NLDM 双线性插值

  Liberty 的语法树解析器来自 ABC 的 sclLiberty（见 abc_scl_liberty_tree.inc
  的文件头与 THIRD_PARTY_NOTICES.md），本文件其余部分是 STA 语义层。

***********************************************************************/

#include <strings.h>
#include <ctype.h>
#include "msta_lib.h"
#include "msta_util.h"
#include "abc_scl_liberty_tree.inc"

/* =====================================================================
   A. 数值与表的解析
   ===================================================================== */

/* Liberty 里的数组长这样：
     index_1("0.01, 0.02, 0.03");
     values("-0.002, -0.003", \
             "0.001, 0.002");
   行与行之间用引号和反斜杠续行分隔，本质上就是 "一串被标点包围的浮点数"。
   所以这里不按逗号切，而是顺序扫描：凡是能strtod成功的就收，失败就跳一个字符。
   这样对引号/换行/续行反斜杠/多余空格全部免疫。 */
static int Msta_LibParseNumbers( const char *pText, ssize_t nLen, double **ppValues, int *pnValues )
{
    const char *p = pText, *pStop = pText + nLen;
    int nCap = 16, nSize = 0;
    double *pVals = (double *)malloc( (size_t)nCap * sizeof(double) );
    assert( pVals );
    while ( p < pStop )
    {
        char *pNext;
        double Value;
        if ( !isdigit((unsigned char)*p) && *p != '-' && *p != '+' && *p != '.' )
        {   p++;  continue;  }
        Value = strtod( p, &pNext );
        if ( pNext == p )
        {   p++;  continue;  }   /* 例如单独的 '.' 或 "-abc" */
        if ( nSize == nCap )
        {   nCap *= 2; pVals = (double *)realloc( pVals, (size_t)nCap*sizeof(double) ); assert(pVals); }
        pVals[nSize++] = Value;
        p = pNext;
    }
    *ppValues = pVals;
    *pnValues = nSize;
    return nSize;
}

/* 读一个 LIST 条目（index_1("...")）里的数字数组。 */
static double *Msta_LibItemNumbers( Scl_Tree_t *pTree, Scl_Item_t *pItem, int *pnValues )
{
    char *pText;
    ssize_t nLen;
    double *pVals = NULL;
    *pnValues = 0;
    if ( pItem == NULL )
        return NULL;
    Scl_LibertyItemRaw( pTree, pItem, &pText, &nLen );
    Msta_LibParseNumbers( pText, nLen, &pVals, pnValues );
    return pVals;
}

/* lu_table_template 是 "索引的别名"：
     lu_table_template("delay_template_7x7"){ variable_1:"input_net_transition";
                                               variable_2:"total_output_net_capacitance";
                                               index_1("..."); index_2("..."); }
   有些库把 index 只写在模板里，表体只留 values()，所以必须存下来备用。 */
typedef struct {
    MstaId Name;
    double *pRow;  int nRow;
    double *pCol;  int nCol;
    int fRowIsLoad;
    int fColIsSlew;
    int fRowUsesSecond;
    int fColUsesFirst;
} MstaTemplate;

/* 模板表只在本文件里用，只需要 init/at/append/free 四个动作，
   所以不套 MstaArrayDefine（宏还会生成这里用不到的按指针查找）。 */
typedef struct {
    MstaTemplate *pData;
    int           nSize;
    int           nCapacity;
} MstaTemplateArray;

static void MstaTemplateArrayInit( MstaTemplateArray *p )
{ p->pData = NULL; p->nSize = p->nCapacity = 0; }

static MstaTemplate *MstaTemplateArrayAt( MstaTemplateArray *p, int i )
{ assert( 0 <= i && i < p->nSize ); return &p->pData[i]; }

static MstaTemplate *MstaTemplateArrayAppend( MstaTemplateArray *p )
{
    if ( p->nSize == p->nCapacity )
    {
        int nNew = p->nCapacity ? p->nCapacity * 2 : 8;
        p->pData = (MstaTemplate *)realloc( p->pData, (size_t)nNew * sizeof(MstaTemplate) );
        assert( p->pData );
        p->nCapacity = nNew;
    }
    memset( &p->pData[p->nSize], 0, sizeof(MstaTemplate) );
    return &p->pData[p->nSize++];
}

static void MstaTemplateArrayFree( MstaTemplateArray *p )
{ free( p->pData ); p->pData = NULL; p->nSize = p->nCapacity = 0; }

static MstaTemplate *Msta_LibFindTemplate( MstaTemplateArray *pArr, const char *pName )
{
    MstaId Name = Msta_NameTableId( Msta_Names(), pName );
    int i;
    for ( i = 0; i < pArr->nSize; i++ )
        if ( MstaTemplateArrayAt(pArr,i)->Name == Name )
            return MstaTemplateArrayAt(pArr,i);
    return NULL;
}

/* 把一根索引轴整理成恰好 nWant 个数：不够的用 1,2,3... 顶上，多了截断。
   传进来的 pAxis 可以是 NULL。返回的数组归调用方（表）所有。 */
static double *Msta_LibFitAxis( double *pAxis, int nLen, int nWant )
{
    double *pNew = (double *)malloc( (size_t)(nWant > 0 ? nWant : 1) * sizeof(double) );
    int i;
    assert( pNew );
    for ( i = 0; i < nWant; i++ )
        pNew[i] = ( pAxis && i < nLen ) ? pAxis[i] : (double)(i + 1);
    free( pAxis );
    return pNew;
}

/* 读一张表：cell_rise("tpl"){ index_1(); index_2(); values(); }
   pIndexesFromTemplate 在表体自己没写 index 时兜底。
   时间轴乘 TimeScale 变成 ps，负载轴乘 CapScale 变成 fF。 */
static int Msta_LibReadTable( Scl_Tree_t *pTree, Scl_Item_t *pItem,
                              const char *pTagName, MstaLib *pLib,
                              MstaTemplateArray *pTemplates, MstaTable *pOut )
{
    Scl_Item_t *pIndex1, *pIndex2, *pValues;
    double *pRow = NULL, *pCol = NULL, *pVals = NULL;
    int nRow = 0, nCol = 0, nVals = 0, i;
    int nRowAxis, nColAxis;       /* 两根轴各自的原始长度 */

    memset( pOut, 0, sizeof(MstaTable) );
    if ( pItem == NULL )
        return 0;

    pIndex1 = Scl_LibertyFindChild( pTree, pItem, "index_1" );
    pIndex2 = Scl_LibertyFindChild( pTree, pItem, "index_2" );
    pValues = Scl_LibertyFindChild( pTree, pItem, "values" );
    if ( pValues == NULL )
        return 0;                                    /* 空表：这张弧不存在 */

    pRow = Msta_LibItemNumbers( pTree, pIndex1, &nRow );
    pCol = Msta_LibItemNumbers( pTree, pIndex2, &nCol );
    pVals = Msta_LibItemNumbers( pTree, pValues, &nVals );

    /* 表体没带索引时去模板里补。模板存的是原始单位，所以补进来后
       和自带索引的表走同一条换算路径，不会乘两次。 */
    if ( nRow == 0 || nCol == 0 )
    {
        MstaTemplate *pT = Msta_LibFindTemplate( pTemplates, Scl_LibertyItemName(pTree, pItem) );
        if ( pT )
        {
            if ( nRow == 0 && pT->nRow > 0 )
            {   nRow = pT->nRow;
                free( pRow );
                pRow = (double*)malloc( (size_t)nRow * sizeof(double) );
                memcpy( pRow, pT->pRow, (size_t)nRow * sizeof(double) );  }
            if ( nCol == 0 && pT->nCol > 0 )
            {   nCol = pT->nCol;
                free( pCol );
                pCol = (double*)malloc( (size_t)nCol * sizeof(double) );
                memcpy( pCol, pT->pCol, (size_t)nCol * sizeof(double) );  }
        }
    }

    if ( nVals == 0 )
    {   free(pRow); free(pCol); free(pVals); return 0;  }
    nRowAxis = nRow;
    nColAxis = nCol;

    /* 形状纠正，规则很简单（也讲得清）：
         列数取 index_2 的长度；values 能整除它 -> 就是 2-D 表；
         否则认为这是一维表（列数=1，行数=values 个数）。
       之后两根轴都被拉成和行列数等长，缺的补 1,2,3...，多的截断，
       这样后面的双线性插值永远不用判空。 */
    if ( nCol > 0 && nVals % nCol == 0 )
        nRow = nVals / nCol;
    else
    {   nCol = 1;  nRow = nVals;  }
    if ( nRow < 1 )  nRow = 1;
    pRow = Msta_LibFitAxis( pRow, nRowAxis, nRow );
    pCol = Msta_LibFitAxis( pCol, nColAxis, nCol );

    /* 单位换算：行轴是摆率(ps)，列轴是负载(fF)，表值要么是延迟要么是
       setup/hold 时间，都是时间量纲。 */
    snprintf( pOut->sTag, sizeof(pOut->sTag), "%s", pTagName );
    {
        MstaTemplate *pT = Msta_LibFindTemplate( pTemplates, Scl_LibertyItemName(pTree, pItem) );
        pOut->fRowIsLoad = pT ? pT->fRowIsLoad : 0;
        pOut->fColIsSlew = pT ? pT->fColIsSlew : 0;
        pOut->fRowUsesSecond = pT ? pT->fRowUsesSecond : 0;
        pOut->fColUsesFirst = pT ? pT->fColUsesFirst : 0;
    }
    pOut->nRows    = nRow;
    pOut->nCols    = nCol;
    pOut->pValues  = pVals;
    pOut->pRowIndex = pRow;
    pOut->pColIndex = pCol;
    for ( i = 0; i < nRow; i++ )
        pOut->pRowIndex[i] *= pOut->fRowIsLoad ? pLib->CurCapScale : pLib->CurTimeScale;
    for ( i = 0; i < nCol; i++ )
        pOut->pColIndex[i] *= pOut->fColIsSlew ? pLib->CurTimeScale : pLib->CurCapScale;
    for ( i = 0; i < nRow * nCol; i++ )
        pOut->pValues[i] *= pLib->CurTimeScale;
    return 1;
}

/* =====================================================================
   B/C. 一个 cell 的解析
   ===================================================================== */

static void Msta_LibCellFinish( MstaCell *pCell );

/* 读一个 timing() 组。它是本文件的核心：组合延迟、clk2q、setup、hold
   全都长在同一个语法形状里，区别只在 timing_type。 */
static void Msta_LibReadTiming( Scl_Tree_t *pTree, Scl_Item_t *pTiming,
                                MstaLib *pLib, MstaCell *pCell, MstaId OutPin,
                                MstaTemplateArray *pTemplates )
{
    Scl_Item_t *pRel, *pType, *pSense;
    MstaArc *pArc;

    pRel   = Scl_LibertyFindChild( pTree, pTiming, "related_pin" );
    pType  = Scl_LibertyFindChild( pTree, pTiming, "timing_type" );
    pSense = Scl_LibertyFindChild( pTree, pTiming, "timing_sense" );

    /* 没有 related_pin 的 timing 组（例如只描述噪声的）跳过。 */
    if ( pRel == NULL )
        return;

    pArc = MstaArcArrayAppend( &pCell->vArcs );
    pArc->InPin  = Msta_NameTableId( Msta_Names(), Scl_LibertyItemName(pTree, pRel) );
    pArc->OutPin = OutPin;
    pArc->Type   = ( pType == NULL ) ? MSTA_TT_COMBINATIONAL
              : Msta_TimingTypeFromName( Scl_LibertyItemName(pTree, pType) );
    pArc->Sense  = MSTA_SENSE_UNKNOWN;
    if ( pSense )
    {
        const char *p = Scl_LibertyItemName(pTree, pSense);
        if      ( !strcasecmp(p, "positive_unate") ) pArc->Sense = MSTA_SENSE_POSITIVE;
        else if ( !strcasecmp(p, "negative_unate") ) pArc->Sense = MSTA_SENSE_NEGATIVE;
        else if ( !strcasecmp(p, "non_unate") )            pArc->Sense = MSTA_SENSE_NONUNATE;
    }
    pArc->MaxSlewLimit = -1.0;

    /* 四种延迟/摆率表 */
    Msta_LibReadTable( pTree, Scl_LibertyFindChild(pTree, pTiming, "cell_rise"),
                       "cell_rise",      pLib, pTemplates, &pArc->DelayRise );
    Msta_LibReadTable( pTree, Scl_LibertyFindChild(pTree, pTiming, "cell_fall"),
                       "cell_fall",      pLib, pTemplates, &pArc->DelayFall );
    Msta_LibReadTable( pTree, Scl_LibertyFindChild(pTree, pTiming, "rise_transition"),
                       "rise_transition",pLib, pTemplates, &pArc->TransRise );
    Msta_LibReadTable( pTree, Scl_LibertyFindChild(pTree, pTiming, "fall_transition"),
                       "fall_transition",pLib, pTemplates, &pArc->TransFall );

    /* 约束弧（setup/hold）在 Liberty 里把数值放在 rise_constraint / fall_constraint；
       少数库写成 cell_rise/cell_fall。只有 timing_type 是 setup/hold 时才去读它们，
       否则组合弧的 cell_rise 会被同时当成 setup，产生 "延迟==setup" 的假数据。 */
    if ( pArc->Type == MSTA_TT_SETUP_RISING || pArc->Type == MSTA_TT_SETUP_FALLING ||
         pArc->Type == MSTA_TT_RECOVERY_RISING || pArc->Type == MSTA_TT_RECOVERY_FALLING ||
         pArc->Type == MSTA_TT_HOLD_RISING || pArc->Type == MSTA_TT_HOLD_FALLING ||
         pArc->Type == MSTA_TT_REMOVAL_RISING || pArc->Type == MSTA_TT_REMOVAL_FALLING )
    {
        if ( !Msta_LibReadTable( pTree, Scl_LibertyFindChild(pTree, pTiming, "rise_constraint"),
                                 "rise_constraint", pLib, pTemplates, &pArc->ConstraintRise ) )
            Msta_LibReadTable( pTree, Scl_LibertyFindChild(pTree, pTiming, "cell_rise"),
                               "cell_rise", pLib, pTemplates, &pArc->ConstraintRise );
        if ( !Msta_LibReadTable( pTree, Scl_LibertyFindChild(pTree, pTiming, "fall_constraint"),
                                 "fall_constraint", pLib, pTemplates, &pArc->ConstraintFall ) )
            Msta_LibReadTable( pTree, Scl_LibertyFindChild(pTree, pTiming, "cell_fall"),
                               "cell_fall", pLib, pTemplates, &pArc->ConstraintFall );
    }

    /* 对未支持的 timing_type 发出告警。 */
    if ( pType != NULL && pArc->Type == MSTA_TT_UNKNOWN )
        Msta_WarnOnce( "timing_type \"%s\" is not modeled by msta (it is kept as an unused arc)",
                       Scl_LibertyItemName(pTree, pType) );
}

/* 读一个 pin 组。 */
static void Msta_LibReadPin( Scl_Tree_t *pTree, Scl_Item_t *pPin,
                             MstaLib *pLib, MstaCell *pCell, MstaTemplateArray *pTemplates )
{
    const char *pName = Scl_LibertyItemName( pTree, pPin );
    Scl_Item_t *pItem;
    MstaPin *pNew;

    /* 电源脚（pg_pin）不会以 pin 形式出现，但 VGND 有时也写成 pin：不参与时序。 */
    pNew = MstaPinArrayAppend( &pCell->vPins );
    pNew->Name    = Msta_NameTableId( Msta_Names(), pName );
    pNew->Dir     = MSTA_DIR_NO;
    pNew->Cap     = pLib->CurDefaultCap;
    pNew->MaxCap  = -1.0;
    pNew->MinCap  = -1.0;
    pNew->MaxSlew = pLib->CurDefaultMaxSlew;
    pNew->fClock  = 0;
    pNew->fGateClock = pNew->fGateEnable = pNew->fGateOut = 0;
    pNew->pFunc   = NULL;

    pItem = Scl_LibertyFindChild( pTree, pPin, "direction" );
    if ( pItem )
    {
        const char *p = Scl_LibertyItemName(pTree, pItem);
        if      ( !strcasecmp(p, "input") )  pNew->Dir = MSTA_DIR_INPUT;
        else if ( !strcasecmp(p, "output") ) pNew->Dir = MSTA_DIR_OUTPUT;
        else if ( !strcasecmp(p, "inout") )  pNew->Dir = MSTA_DIR_INOUT;
        else                                 pNew->Dir = MSTA_DIR_INTERNAL;
    }
    pItem = Scl_LibertyFindChild( pTree, pPin, "capacitance" );
    if ( pItem )
        pNew->Cap = Scl_LibertyItemDouble(pTree, pItem, 0.0, NULL) * pLib->CurCapScale;
    pItem = Scl_LibertyFindChild( pTree, pPin, "max_capacitance" );
    if ( pItem )
        pNew->MaxCap = Scl_LibertyItemDouble(pTree, pItem, -1.0, NULL) * pLib->CurCapScale;
    pItem = Scl_LibertyFindChild( pTree, pPin, "min_capacitance" );
    if ( pItem )
        pNew->MinCap = Scl_LibertyItemDouble(pTree, pItem, -1.0, NULL) * pLib->CurCapScale;
    pItem = Scl_LibertyFindChild( pTree, pPin, "max_transition" );
    if ( pItem )
        pNew->MaxSlew = Scl_LibertyItemDouble(pTree, pItem, -1.0, NULL) * pLib->CurTimeScale;
    pItem = Scl_LibertyFindChild( pTree, pPin, "clock" );
    if ( pItem )
        pNew->fClock = Scl_LibertyItemBool(pTree, pItem, 0);
    pItem = Scl_LibertyFindChild( pTree, pPin, "clock_gate_clock_pin" );
    if ( pItem )
        pNew->fGateClock = Scl_LibertyItemBool(pTree, pItem, 0);
    pItem = Scl_LibertyFindChild( pTree, pPin, "clock_gate_enable_pin" );
    if ( pItem )
        pNew->fGateEnable = Scl_LibertyItemBool(pTree, pItem, 0);
    pItem = Scl_LibertyFindChild( pTree, pPin, "clock_gate_out_pin" );
    if ( pItem )
        pNew->fGateOut = Scl_LibertyItemBool(pTree, pItem, 0);
    pItem = Scl_LibertyFindChild( pTree, pPin, "function" );
    if ( pItem )
        pNew->pFunc = Msta_StrDup( Scl_LibertyItemName(pTree, pItem) );

    /* 同一个 pin 组里可以挂多条 timing()：一条来自某个 related_pin。 */
    {
        Scl_Item_t *pTiming;
        Scl_ItemForEachChildName( pTree, pPin, pTiming, "timing" )
            Msta_LibReadTiming( pTree, pTiming, pLib, pCell, pNew->Name, pTemplates );
    }
}

/* ff()/latch() 组：告诉我们要把哪些脚当时钟、哪些当数据。
   这里只取 "clocked_on"（ff）和 "enable"/"clocked_on"（latch），
   标记时钟脚；不计算 next_state 的布尔表达式。 */
static void Msta_LibReadStore( Scl_Tree_t *pTree, Scl_Item_t *pStore, MstaCell *pCell, int fIsFF )
{
    Scl_Item_t *pItem = Scl_LibertyFindChild( pTree, pStore, "clocked_on" );
    if ( pItem == NULL )
        pItem = Scl_LibertyFindChild( pTree, pStore, "enable" );
    if ( pItem == NULL )
        return;
    /* 用 pin 的 fClock 位记下 "这是时钟脚"。多个时钟脚（双边沿）就都标上。 */
    {
        MstaId Name = Msta_NameTableId( Msta_Names(), Scl_LibertyItemName(pTree, pItem) );
        int i;
        for ( i = 0; i < pCell->vPins.nSize; i++ )
            if ( pCell->vPins.pData[i].Name == Name )
                pCell->vPins.pData[i].fClock = 1;
    }
    pCell->fSequential = fIsFF ? 1 : pCell->fSequential;
    if ( !fIsFF )
        pCell->fLatch = 1;
}

static MstaCell *Msta_LibFindCellByName( MstaLib *pLib, MstaId Name );

/* 检查同一 library 里是否已经定义过同名单元。 */
static int Msta_LibHasCellInLibrary( MstaLib *pLib, MstaId Name, MstaId LibraryName )
{
    int i;
    for ( i = 0; i < pLib->vCells.nSize; i++ )
    {
        MstaCell *pCell = MstaCellArrayAt(&pLib->vCells,i);
        if ( pCell->Name == Name && pCell->LibName == LibraryName ) return 1;
    }
    for ( i = 0; i < pLib->vCornerCells.nSize; i++ )
    {
        MstaCell *pCell = MstaCellArrayAt(&pLib->vCornerCells,i);
        if ( pCell->Name == Name && pCell->LibName == LibraryName ) return 1;
    }
    return 0;
}

/* 时钟门控单元：把 clock_gate_enable_pin 上的 setup/hold 约束弧收成一条检查。
   约束值就放在使能脚的约束弧里（相对 clock_gate_clock_pin 的时钟沿）。 */
static void Msta_LibBuildGateChecks( MstaCell *pCell )
{
    MstaId nClkPin = MSTA_NO_ID;
    int i, j, k;
    /* 时钟脚：优先 clock_gate_clock_pin，退回库里标成时钟的输入脚。 */
    for ( i = 0; i < pCell->vPins.nSize; i++ )
        if ( pCell->vPins.pData[i].fGateClock ) { nClkPin = pCell->vPins.pData[i].Name; break; }
    if ( nClkPin == MSTA_NO_ID )
        for ( i = 0; i < pCell->vPins.nSize; i++ )
            if ( pCell->vPins.pData[i].fClock && pCell->vPins.pData[i].Dir == MSTA_DIR_INPUT )
            { nClkPin = pCell->vPins.pData[i].Name; break; }
    if ( nClkPin == MSTA_NO_ID )
        return;
    for ( i = 0; i < pCell->vPins.nSize; i++ )
    {
        MstaPin *pEnable = &pCell->vPins.pData[i];
        MstaGateCheck *pGate;
        if ( !pEnable->fGateEnable )
            continue;
        pGate = MstaGateCheckArrayAppend( &pCell->vGates );
        pGate->ClkPin    = nClkPin;
        pGate->EnablePin = pEnable->Name;
        pGate->fClkRises = 1;                   /* 库没说时按上升沿 */
        pGate->SetupArc  = MSTA_NO_ID;
        pGate->HoldArc   = MSTA_NO_ID;
    }
    /* 使能脚上的 setup/hold 弧给出约束表与有效沿。 */
    for ( j = 0; j < pCell->vArcs.nSize; j++ )
    {
        MstaArc *pArc = MstaArcArrayAt( &pCell->vArcs, j );
        if ( pArc->InPin != nClkPin )
            continue;
        if ( pArc->Type != MSTA_TT_SETUP_RISING && pArc->Type != MSTA_TT_SETUP_FALLING &&
             pArc->Type != MSTA_TT_HOLD_RISING  && pArc->Type != MSTA_TT_HOLD_FALLING )
            continue;
        for ( k = 0; k < pCell->vGates.nSize; k++ )
            if ( pCell->vGates.pData[k].EnablePin == pArc->OutPin )
            {
                MstaGateCheck *pGate = &pCell->vGates.pData[k];
                if ( pArc->Type == MSTA_TT_SETUP_RISING || pArc->Type == MSTA_TT_SETUP_FALLING )
                    pGate->SetupArc = (MstaId)j;
                else
                    pGate->HoldArc = (MstaId)j;
                pGate->fClkRises = ( pArc->Type == MSTA_TT_SETUP_RISING ||
                                     pArc->Type == MSTA_TT_HOLD_RISING );
                break;
            }
    }
}

static void Msta_LibReadCell( Scl_Tree_t *pTree, Scl_Item_t *pCellItem,
                              MstaLib *pLib, MstaTemplateArray *pTemplates )
{
    const char *pName = Scl_LibertyItemName( pTree, pCellItem );
    Scl_Item_t *pItem, *pChild;
    MstaCell *pCell;
    int i;

    {
        MstaId nCellName = Msta_NameTableId(Msta_Names(),pName);
        if ( Msta_LibHasCellInLibrary(pLib,nCellName,pLib->CurLibName) )
        {
            pLib->nCellsSkipped++;
            return;
        }
        if ( Msta_LibFindCellByName(pLib,nCellName) != NULL )
        {
            pCell = MstaCellArrayAppend(&pLib->vCornerCells);
            pLib->nCornerVariants++;
        }
        else
            pCell = MstaCellArrayAppend(&pLib->vCells);
    }
    pCell->Name     = Msta_NameTableId( Msta_Names(), pName );
    pCell->LibName  = pLib->CurLibName;
    pCell->pLibName = Msta_StrDup( pName );
    pCell->Area     = 0.0;
    pCell->Leakage  = 0.0;
    pCell->fBlackBox = 0;

    pItem = Scl_LibertyFindChild( pTree, pCellItem, "area" );
    if ( pItem )
        pCell->Area = Scl_LibertyItemDouble(pTree, pItem, 0.0, NULL);
    pItem = Scl_LibertyFindChild( pTree, pCellItem, "cell_leakage_power" );
    if ( pItem )
        pCell->Leakage = Scl_LibertyItemDouble(pTree, pItem, 0.0, NULL);

    Scl_ItemForEachChildName( pTree, pCellItem, pChild, "pin" )
        Msta_LibReadPin( pTree, pChild, pLib, pCell, pTemplates );
    Scl_ItemForEachChildName( pTree, pCellItem, pChild, "ff" )
        Msta_LibReadStore( pTree, pChild, pCell, 1 );
    Scl_ItemForEachChildName( pTree, pCellItem, pChild, "latch" )
        Msta_LibReadStore( pTree, pChild, pCell, 0 );

    /* 电源单元（只有 VDD/VSS 脚）没有任何时序信息，标掉免得网表里出现时报错。 */
    {
        int fOnlyPower = pCell->vPins.nSize > 0;
        for ( i = 0; i < pCell->vPins.nSize; i++ )
        {
            const char *p = Msta_NameTableName( Msta_Names(), pCell->vPins.pData[i].Name );
            if ( strcasecmp(p,"VDD") && strcasecmp(p,"VSS") && strcasecmp(p,"VPWR")
                 && strcasecmp(p,"VGND") && strcasecmp(p,"VPB") && strcasecmp(p,"VNB") )
                fOnlyPower = 0;
        }
        pCell->fIgnore = fOnlyPower;
    }

    Msta_LibCellFinish( pCell );
    Msta_LibBuildGateChecks( pCell );
    pLib->nCellsRead++;
}

/* ---------------------------------------------------------------------
   C. 把散落的 timing 组归并成 "寄存器检查项"
   ---------------------------------------------------------------------
   归并规则：
     - 一条 setup_rising 弧 = 一次检查的 "一半"，它说：
         数据脚 = 本弧的 OutPin，时钟脚 = 本弧的 InPin(related_pin)，上升沿有效。
     - 同 (时钟, 数据) 的 hold_rising 弧挂到同一条检查上。
     - 时钟脚 -> Q 的 rising_edge 弧提供 clk2q；同一条检查记下来。
   检查表用不到的时序单元（纯组合、latch 型时钟门控）会得到 0 条 vRegs，
   时序引擎据此区分 "终点" 和 "通路"。 */
static void Msta_LibCellFinish( MstaCell *pCell )
{
    int i, j;

    /* 有些库（TAU/竞赛库、部分厂商库）不写 ff() 组，只靠 timing_type 标出
       setup/hold 检查。这类单元按"有时序检查弧 + 时钟脚到输出有边沿弧"补认成
       寄存器：前者说明终点存在，后者说明它能提供 clk2q。 */
    if ( !pCell->fSequential )
    {
        int fHasCheck = 0, fHasEdge = 0;
        for ( i = 0; i < pCell->vArcs.nSize; i++ )
        {
            MstaArc *pArc = MstaArcArrayAt( &pCell->vArcs, i );
            int fCheck = ( pArc->Type == MSTA_TT_SETUP_RISING   || pArc->Type == MSTA_TT_SETUP_FALLING ||
                           pArc->Type == MSTA_TT_HOLD_RISING    || pArc->Type == MSTA_TT_HOLD_FALLING ||
                           pArc->Type == MSTA_TT_RECOVERY_RISING|| pArc->Type == MSTA_TT_RECOVERY_FALLING ||
                           pArc->Type == MSTA_TT_REMOVAL_RISING || pArc->Type == MSTA_TT_REMOVAL_FALLING );
            int fEdge = ( pArc->Type == MSTA_TT_RISE_EDGE || pArc->Type == MSTA_TT_FALL_EDGE ||
                          pArc->Type == MSTA_TT_RISE_BOTH || pArc->Type == MSTA_TT_FALL_BOTH );
            if ( fCheck )
            {
                int k;
                fHasCheck = 1;
                for ( k = 0; k < pCell->vPins.nSize; k++ )
                    if ( pCell->vPins.pData[k].Name == pArc->InPin )
                        pCell->vPins.pData[k].fClock = 1;
            }
            if ( fEdge )
                fHasEdge = 1;
        }
        if ( fHasCheck && fHasEdge )
            pCell->fSequential = 1;
    }
    if ( !pCell->fSequential )
        return;

    /* ---- 第一遍：每个 (时钟脚, 数据脚) 建一条 setup/hold 检查 ---- */
    for ( i = 0; i < pCell->vArcs.nSize; i++ )
    {
        MstaArc *pArc = MstaArcArrayAt( &pCell->vArcs, i );
        int fIsSetup = ( pArc->Type == MSTA_TT_SETUP_RISING  || pArc->Type == MSTA_TT_SETUP_FALLING );
        int fIsHold  = ( pArc->Type == MSTA_TT_HOLD_RISING   || pArc->Type == MSTA_TT_HOLD_FALLING );
        MstaRegCheck *pReg = NULL;
        if ( !fIsSetup && !fIsHold )
            continue;

        for ( j = 0; j < pCell->vRegs.nSize; j++ )
        {
            MstaRegCheck *pTry = MstaRegCheckArrayAt( &pCell->vRegs, j );
            if ( pTry->ClkPin == pArc->InPin && pTry->DataPin == pArc->OutPin )
            {   pReg = pTry; break;  }
        }
        if ( pReg == NULL )
        {
            pReg = MstaRegCheckArrayAppend( &pCell->vRegs );
            pReg->ClkPin       = pArc->InPin;
            pReg->DataPin      = pArc->OutPin;
            pReg->QPin         = MSTA_NO_ID;
            pReg->SetupArc     = MSTA_NO_ID;
            pReg->HoldArc      = MSTA_NO_ID;
            pReg->ClkToQArc    = MSTA_NO_ID;
            pReg->SetupFallArc = MSTA_NO_ID;
            pReg->HoldFallArc  = MSTA_NO_ID;
            pReg->fClkRises    = ( pArc->Type == MSTA_TT_SETUP_RISING || pArc->Type == MSTA_TT_HOLD_RISING );
        }
        if ( fIsSetup )
        {
            if ( pArc->Type == MSTA_TT_SETUP_RISING ) pReg->SetupArc     = (MstaId)i;
            else                                      pReg->SetupFallArc = (MstaId)i;
        }
        else
        {
            if ( pArc->Type == MSTA_TT_HOLD_RISING )  pReg->HoldArc      = (MstaId)i;
            else                                      pReg->HoldFallArc  = (MstaId)i;
        }
    }

    /* ---- recovery/removal 弧描述的是异步控制信号释放时的检查。 ---- */
    for ( i = 0; i < pCell->vArcs.nSize; i++ )
    {
        MstaArc *pArc = MstaArcArrayAt(&pCell->vArcs,i);
        int fRecovery = pArc->Type == MSTA_TT_RECOVERY_RISING ||
                        pArc->Type == MSTA_TT_RECOVERY_FALLING;
        int fRemoval = pArc->Type == MSTA_TT_REMOVAL_RISING ||
                       pArc->Type == MSTA_TT_REMOVAL_FALLING;
        MstaAsyncCheck *pAsync = NULL;
        if ( !fRecovery && !fRemoval ) continue;
        for ( j = 0; j < pCell->vAsync.nSize; j++ )
        {
            MstaAsyncCheck *pTry = MstaAsyncCheckArrayAt(&pCell->vAsync,j);
            if ( pTry->ClkPin == pArc->InPin && pTry->AsyncPin == pArc->OutPin )
            { pAsync = pTry; break; }
        }
        if ( pAsync == NULL )
        {
            pAsync = MstaAsyncCheckArrayAppend(&pCell->vAsync);
            pAsync->ClkPin = pArc->InPin;
            pAsync->AsyncPin = pArc->OutPin;
            pAsync->RecoveryArc = pAsync->RecoveryFallArc = MSTA_NO_ID;
            pAsync->RemovalArc = pAsync->RemovalFallArc = MSTA_NO_ID;
            pAsync->fClkRises = (pArc->Type == MSTA_TT_RECOVERY_RISING ||
                                 pArc->Type == MSTA_TT_REMOVAL_RISING);
        }
        if ( fRecovery )
        {
            if ( pArc->Type == MSTA_TT_RECOVERY_RISING ) pAsync->RecoveryArc = i;
            else pAsync->RecoveryFallArc = i;
        }
        else
        {
            if ( pArc->Type == MSTA_TT_REMOVAL_RISING ) pAsync->RemovalArc = i;
            else pAsync->RemovalFallArc = i;
        }
    }

    /* ---- 第二遍：把 clk2q 弧挂到同一时钟脚的所有检查上 ----
       一个 FF 的 D/SCD 等多个数据脚共用同一段 CLK->Q 延迟，这是硬件事实。 */
    for ( i = 0; i < pCell->vArcs.nSize; i++ )
    {
        MstaArc *pArc = MstaArcArrayAt( &pCell->vArcs, i );
        int fIsEdge = ( pArc->Type == MSTA_TT_RISE_EDGE || pArc->Type == MSTA_TT_FALL_EDGE
                     || pArc->Type == MSTA_TT_RISE_BOTH || pArc->Type == MSTA_TT_FALL_BOTH );
        if ( !fIsEdge )
            continue;
        for ( j = 0; j < pCell->vRegs.nSize; j++ )
        {
            MstaRegCheck *pReg = MstaRegCheckArrayAt( &pCell->vRegs, j );
            if ( pReg->ClkPin != pArc->InPin )
                continue;
            if ( pReg->QPin != MSTA_NO_ID && pReg->QPin != pArc->OutPin )
                continue;                       /* 已经有别的输出脚负责 launch 了 */
            pReg->QPin      = pArc->OutPin;
            pReg->ClkToQArc = (MstaId)i;
        }
    }

    /* 注：库里没有 setup 表的检查（有些竞赛/老库只写 hold）仍然保留成端点，
       检查值按 0 处理——参考工具也是这么算的。 */
}

/* =====================================================================
   D. 库级解析入口
   ===================================================================== */

/* "1ns"/"1ps"/"1us" -> 乘多少倍能变成 ps。 */
static double Msta_LibTimeScaleOf( const char *pUnit )
{
    while ( *pUnit && !isalpha((unsigned char)*pUnit) ) pUnit++;
    switch ( *pUnit )
    {
        case 'f': return 0.001;      /* fs */
        case 'p': return 1.0;        /* ps */
        case 'n': return 1000.0;     /* ns */
        case 'u': return 1000000.0;  /* us */
        case 'm': return 1000000000.0;
        default:  return 1000.0;     /* 没写就按 ns，和大多数库一致 */
    }
}

/* "ff"/"pf"/"uf" -> 乘多少倍能变成 fF。 */
static double Msta_LibCapScaleOf( const char *pUnit )
{
    while ( *pUnit && !isalpha((unsigned char)*pUnit) ) pUnit++;
    switch ( *pUnit )
    {
        case 'a': return 1e-3;       /* af */
        case 'f': return 1.0;        /* ff */
        case 'p': return 1000.0;     /* pf */
        case 'n': return 1e6;        /* nf */
        default:  return 1.0;
    }
}

/* 在已读的库里按名字找 operating condition，可再用库名限定。 */
MstaLibInfo *Msta_LibFindOpCond( MstaLib *pLib, const char *pName, MstaId LibraryName,
                                 MstaOpCond **ppCond )
{
    MstaId nName = Msta_NameId( pName );
    int i, j;
    if ( ppCond ) *ppCond = NULL;
    for ( i = 0; i < pLib->vLibs.nSize; i++ )
    {
        MstaLibInfo *pInfo = MstaLibInfoArrayAt( &pLib->vLibs, i );
        if ( LibraryName != MSTA_NO_ID && pInfo->Name != LibraryName )
            continue;
        for ( j = 0; j < pInfo->vOpConds.nSize; j++ )
        {
            MstaOpCond *pCond = MstaOpCondArrayAt( &pInfo->vOpConds, j );
            if ( pCond->Name != nName )
                continue;
            if ( ppCond ) *ppCond = pCond;
            return pInfo;
        }
    }
    return NULL;
}

/* 库级属性：单位、默认值、工艺角、以及 lu_table_template。 */
/* fFirstLib：只有第一个库决定单位（SDC 默认时间/电容单位，与参考工具一致）。 */
static void Msta_LibReadLibraryAttrs( Scl_Tree_t *pTree, Scl_Item_t *pLibrary, MstaLib *pLib,
                                      MstaTemplateArray *pTemplates, int fFirstLib )
{
    Scl_Item_t *pItem, *pChild;

    /* 工艺角：标称值、默认角，以及所有 operating_conditions(...) 组。 */
    {
        MstaLibInfo *pInfo = MstaLibInfoArrayAt( &pLib->vLibs, pLib->vLibs.nSize - 1 );
        pItem = Scl_LibertyFindChild( pTree, pLibrary, "nom_voltage" );
        if ( pItem ) pInfo->NomVoltage = Scl_LibertyItemDouble( pTree, pItem, -1.0, NULL );
        pItem = Scl_LibertyFindChild( pTree, pLibrary, "nom_temperature" );
        if ( pItem ) pInfo->NomTemperature = Scl_LibertyItemDouble( pTree, pItem, -1.0, NULL );
        pItem = Scl_LibertyFindChild( pTree, pLibrary, "nom_process" );
        if ( pItem ) pInfo->NomProcess = Scl_LibertyItemDouble( pTree, pItem, -1.0, NULL );
        pItem = Scl_LibertyFindChild( pTree, pLibrary, "default_operating_conditions" );
        if ( pItem )
            pInfo->OpCondName = Msta_NameTableId( Msta_Names(),
                                                  Scl_LibertyItemName(pTree,pItem) );
        Scl_ItemForEachChildName( pTree, pLibrary, pChild, "operating_conditions" )
        {
            MstaOpCond *pCond = MstaOpCondArrayAppend( &pInfo->vOpConds );
            pCond->Name = Msta_NameTableId( Msta_Names(), Scl_LibertyItemName(pTree,pChild) );
            pCond->Voltage = pCond->Temperature = pCond->Process = -1.0;
            pCond->KVolt = pCond->KTemp = pCond->KProcess = -1.0;
            {
                Scl_Item_t *pField;
                if ( ( pField = Scl_LibertyFindChild(pTree,pChild,"voltage") ) != NULL )
                    pCond->Voltage = Scl_LibertyItemDouble( pTree, pField, -1.0, NULL );
                if ( ( pField = Scl_LibertyFindChild(pTree,pChild,"temperature") ) != NULL )
                    pCond->Temperature = Scl_LibertyItemDouble( pTree, pField, -1.0, NULL );
                if ( ( pField = Scl_LibertyFindChild(pTree,pChild,"process") ) != NULL )
                    pCond->Process = Scl_LibertyItemDouble( pTree, pField, -1.0, NULL );
                if ( ( pField = Scl_LibertyFindChild(pTree,pChild,"k_volt") ) != NULL )
                    pCond->KVolt = Scl_LibertyItemDouble( pTree, pField, -1.0, NULL );
                if ( ( pField = Scl_LibertyFindChild(pTree,pChild,"k_temp") ) != NULL )
                    pCond->KTemp = Scl_LibertyItemDouble( pTree, pField, -1.0, NULL );
                if ( ( pField = Scl_LibertyFindChild(pTree,pChild,"k_process") ) != NULL )
                    pCond->KProcess = Scl_LibertyItemDouble( pTree, pField, -1.0, NULL );
            }
        }
    }
    pLib->CurTimeScale = pLib->TimeScale;
    pLib->CurCapScale = pLib->CapScale;
    pLib->CurDefaultMaxSlew = pLib->DefaultMaxSlew;
    pLib->CurDefaultCap = pLib->DefaultCap;
    pItem = Scl_LibertyFindChild( pTree, pLibrary, "time_unit" );
    if ( pItem )
        pLib->CurTimeScale = Msta_LibTimeScaleOf( Scl_LibertyItemName(pTree, pItem) );
    if ( fFirstLib ) pLib->TimeScale = pLib->CurTimeScale;
    pItem = Scl_LibertyFindChild( pTree, pLibrary, "capacitive_load_unit" );
    if ( pItem )
    {
        /* capacitive_load_unit(1.0, "pf") —— 第一个数是倍数，第二个是单位 */
        char *pText; ssize_t nLen; double *pNums; int nNums;
        Scl_LibertyItemRaw( pTree, pItem, &pText, &nLen );
        Msta_LibParseNumbers( pText, nLen, &pNums, &nNums );
        if ( nNums >= 1 && pNums[0] > 0 )
        {
            double Scale = pNums[0] * Msta_LibCapScaleOf( pText );
            pLib->CurCapScale = Scale;
        }
        free( pNums );
    }
    if ( fFirstLib ) pLib->CapScale = pLib->CurCapScale;
    pItem = Scl_LibertyFindChild( pTree, pLibrary, "default_max_transition" );
    if ( pItem )
        pLib->CurDefaultMaxSlew = Scl_LibertyItemDouble(pTree, pItem, -1.0, NULL) * pLib->CurTimeScale;
    pItem = Scl_LibertyFindChild( pTree, pLibrary, "default_input_pin_cap" );
    if ( pItem )
        pLib->CurDefaultCap = Scl_LibertyItemDouble(pTree, pItem, 0.0, NULL) * pLib->CurCapScale;
    if ( fFirstLib )
    {
        pLib->DefaultMaxSlew = pLib->CurDefaultMaxSlew;
        pLib->DefaultCap = pLib->CurDefaultCap;
    }

    /* 索引模板表：单位先不动，等到真正被某张表使用时再换算，避免乘两次。 */
    Scl_ItemForEachChildName( pTree, pLibrary, pChild, "lu_table_template" )
    {
        MstaTemplate *pT = MstaTemplateArrayAppend( pTemplates );
        Scl_Item_t *pVar1 = Scl_LibertyFindChild( pTree, pChild, "variable_1" );
        Scl_Item_t *pVar2 = Scl_LibertyFindChild( pTree, pChild, "variable_2" );
        pT->Name  = Msta_NameTableId( Msta_Names(), Scl_LibertyItemName(pTree, pChild) );
        pT->pRow  = Msta_LibItemNumbers( pTree, Scl_LibertyFindChild(pTree, pChild, "index_1"), &pT->nRow );
        pT->pCol  = Msta_LibItemNumbers( pTree, Scl_LibertyFindChild(pTree, pChild, "index_2"), &pT->nCol );
        pT->fRowIsLoad = pVar1 && !strcasecmp( Scl_LibertyItemName(pTree,pVar1),
                                               "total_output_net_capacitance" );
        pT->fColIsSlew = pVar2 && ( !strcasecmp(Scl_LibertyItemName(pTree,pVar2),
                                                 "input_net_transition")
                                  || !strcasecmp(Scl_LibertyItemName(pTree,pVar2),
                                                  "constrained_pin_transition")
                                  || !strcasecmp(Scl_LibertyItemName(pTree,pVar2),
                                                  "related_pin_transition") );
        pT->fRowUsesSecond = pT->fRowIsLoad || (pVar1 &&
            !strcasecmp(Scl_LibertyItemName(pTree,pVar1), "constrained_pin_transition"));
        pT->fColUsesFirst = pVar2 && ( !strcasecmp(Scl_LibertyItemName(pTree,pVar2),
                                                  "input_net_transition")
                                      || !strcasecmp(Scl_LibertyItemName(pTree,pVar2),
                                                     "input_transition_time")
                                      || !strcasecmp(Scl_LibertyItemName(pTree,pVar2),
                                                     "related_pin_transition") );
    }
}

int Msta_LibRead( MstaLib *pLib, const char *pFileName, int fVerbose )
{
    Scl_Tree_t *pTree = Scl_LibertyParse( pFileName );
    Scl_Item_t *pRoot, *pLibrary, *pCell;
    MstaTemplateArray vTemplates;
    int nCellsBefore, nSkippedBefore;

    if ( pTree == NULL )
        return 0;

    /* Liberty 的顶层形状是
         library("NAME"){ define(...); cell(...){...} }
       绝大多数库里 library 就是文件的第一个条目（ABC 也这么假定）。
       个别库会先写几行 define/version，所以不满足时再沿顶层链找一次。 */
    pRoot    = Scl_LibertyRoot( pTree );
    pLibrary = ( Scl_LibertyCompare(pTree, pRoot->Key, "library") == 0 )
             ? pRoot : Scl_LibertyFindTop( pTree, "library" );
    if ( pLibrary == NULL )
    {
        Msta_Error( "\"%s\": no \"library\" entry found at top level.\n", pFileName );
        Scl_LibertyStop( pTree );
        return 0;
    }

    {
        /* 每个读进来的库都登记一条，方便 report_lib 和多重阈值流程看清单。 */
        MstaLibInfo *pInfo = MstaLibInfoArrayAppend( &pLib->vLibs );
        pInfo->Name      = Msta_NameTableId( Msta_Names(), Scl_LibertyItemName(pTree,pLibrary) );
        pInfo->pFileName = Msta_StrDup( pFileName );
        pInfo->nCells    = 0;
        pInfo->OpCondName = MSTA_NO_ID;
        pInfo->NomVoltage = pInfo->NomTemperature = pInfo->NomProcess = -1.0;
        MstaOpCondArrayInit( &pInfo->vOpConds );
        pLib->CurLibName = pInfo->Name;
    }
    if ( pLib->pLibName == NULL )
    {
        pLib->pLibName  = Msta_StrDup( Scl_LibertyItemName(pTree, pLibrary) );
        pLib->pFileName = Msta_StrDup( pFileName );
    }

    MstaTemplateArrayInit( &vTemplates );
    nCellsBefore = pLib->vCells.nSize + pLib->vCornerCells.nSize;
    nSkippedBefore = pLib->nCellsSkipped;
    Msta_LibReadLibraryAttrs( pTree, pLibrary, pLib, &vTemplates, pLib->vLibs.nSize == 1 );
    Scl_ItemForEachChildName( pTree, pLibrary, pCell, "cell" )
        Msta_LibReadCell( pTree, pCell, pLib, &vTemplates );

    /* 模板只在一次解析里有效 */
    {
        int i;
        for ( i = 0; i < vTemplates.nSize; i++ )
        {
            free( vTemplates.pData[i].pRow );
            free( vTemplates.pData[i].pCol );
        }
        MstaTemplateArrayFree( &vTemplates );
    }

    {
        MstaLibInfo *pInfo = MstaLibInfoArrayAt( &pLib->vLibs, pLib->vLibs.nSize - 1 );
        pInfo->nCells = pLib->vCells.nSize + pLib->vCornerCells.nSize - nCellsBefore;
        if ( fVerbose )
            Msta_Info( "liberty \"%s\": %d cells (library \"%s\", %d duplicate definitions skipped)\n",
                       pFileName, pInfo->nCells, Msta_NameStr(pInfo->Name),
                       pLib->nCellsSkipped - nSkippedBefore );
    }

    Scl_LibertyStop( pTree );
    Msta_LibRehashCells( pLib );
    return 1;
}

/* =====================================================================
   E. 查询接口
   ===================================================================== */

MstaLib *Msta_LibStart( void )
{
    MstaLib *p = (MstaLib *)calloc( 1, sizeof(MstaLib) );
    assert( p );
    MstaCellArrayInit( &p->vCells );
    MstaCellArrayInit( &p->vCornerCells );
    MstaLibInfoArrayInit( &p->vLibs );
    p->CurLibName = MSTA_NO_ID;
    p->TimeScale      = 1000.0;   /* 默认 ns -> ps */
    p->CapScale       = 1.0;      /* 默认 ff -> fF */
    p->DefaultMaxSlew = -1.0;
    p->DefaultCap     = 0.0;
    p->CurTimeScale = p->TimeScale;
    p->CurCapScale = p->CapScale;
    p->CurDefaultMaxSlew = p->DefaultMaxSlew;
    p->CurDefaultCap = p->DefaultCap;
    p->nCellHash      = 1 << 18;  /* 降低散列表探测长度。 */
    p->pCellHash      = (int *)malloc( (size_t)p->nCellHash * sizeof(int) );
    assert( p->pCellHash );
    Msta_LibRehashCells( p );
    return p;
}

/* 重建 cell 名散列。追加新库后调用一次。 */
int Msta_LibRehashCells( MstaLib *pLib )
{
    int i, nEmpty = -1;
    for ( i = 0; i < pLib->nCellHash; i++ )
        pLib->pCellHash[i] = nEmpty;
    for ( i = 0; i < pLib->vCells.nSize; i++ )
    {
        unsigned nProbe = (unsigned)pLib->vCells.pData[i].Name % (unsigned)pLib->nCellHash;
        while ( pLib->pCellHash[nProbe] != nEmpty )
            nProbe = (nProbe + 1) % (unsigned)pLib->nCellHash;
        pLib->pCellHash[nProbe] = i;
    }
    return pLib->vCells.nSize;
}

/* 按散列找同名 cell（多库时返回第一个定义的那个）。 */
static MstaCell *Msta_LibFindCellByName( MstaLib *pLib, MstaId Name )
{
    int nEmpty = -1;
    unsigned nProbe = (unsigned)Name % (unsigned)pLib->nCellHash;
    while ( pLib->pCellHash[nProbe] != nEmpty )
    {
        int i = pLib->pCellHash[nProbe];
        if ( pLib->vCells.pData[i].Name == Name )
            return &pLib->vCells.pData[i];
        nProbe = (nProbe + 1) % (unsigned)pLib->nCellHash;
    }
    return NULL;
}

/* cell 名可以用 "库名/cell 名" 写（多库时用来消歧）。 */
MstaCell *Msta_LibFindCell( MstaLib *pLib, const char *pCellName )
{
    const char *pSlash = strchr( pCellName, '/' );
    char sLib[256], sCell[256];
    size_t nLib;
    int i;
    if ( pSlash == NULL )
        return Msta_LibFindCellByName( pLib, Msta_NameTableId(Msta_Names(),pCellName) );
    nLib = (size_t)( pSlash - pCellName );
    if ( nLib >= sizeof(sLib) )
        return NULL;
    memcpy( sLib, pCellName, nLib );
    sLib[nLib] = 0;
    snprintf( sCell, sizeof(sCell), "%s", pSlash + 1 );
    for ( i = 0; i < pLib->vCells.nSize + pLib->vCornerCells.nSize; i++ )
    {
        MstaCell *pCell = i < pLib->vCells.nSize
                        ? MstaCellArrayAt(&pLib->vCells,i)
                        : MstaCellArrayAt(&pLib->vCornerCells,i-pLib->vCells.nSize);
        if ( pCell->Name == Msta_NameTableId(Msta_Names(),sCell) &&
             pCell->LibName != MSTA_NO_ID &&
             !strcmp( Msta_NameStr(pCell->LibName), sLib ) )
            return pCell;
    }
    /* 显式限定库名时必须精确匹配，不能静默改用另一个库的同名单元。 */
    return NULL;
}

/* 取指定 library 的同名单元；没指定或该库没有定义时用默认 cell。 */
MstaCell *Msta_LibCellForCorner( MstaLib *pLib, MstaCell *pCell, MstaId LibraryName )
{
    int i;
    if ( pCell == NULL || LibraryName == MSTA_NO_ID || pCell->LibName == LibraryName )
        return pCell;
    for ( i = 0; i < pLib->vCornerCells.nSize; i++ )
    {
        MstaCell *pAlt = MstaCellArrayAt(&pLib->vCornerCells,i);
        if ( pAlt->Name == pCell->Name && pAlt->LibName == LibraryName )
            return pAlt;
    }
    return pCell;
}

/* 角库的弧按 related pin、constrained pin 和 timing_type 对齐默认定义。 */
MstaArc *Msta_LibArcForCorner( MstaLib *pLib, MstaCell *pCell, MstaId ArcId,
                               MstaId LibraryName )
{
    MstaCell *pAlt = Msta_LibCellForCorner(pLib,pCell,LibraryName);
    MstaArc *pBaseArc;
    int i, nOrdinal = 0;
    if ( pCell == NULL || ArcId < 0 || ArcId >= pCell->vArcs.nSize ) return NULL;
    pBaseArc = MstaArcArrayAt(&pCell->vArcs,ArcId);
    if ( pAlt == pCell ) return pBaseArc;
    for ( i = 0; i < ArcId; i++ )
    {
        MstaArc *pTry = MstaArcArrayAt(&pCell->vArcs,i);
        if ( pTry->InPin == pBaseArc->InPin && pTry->OutPin == pBaseArc->OutPin &&
             pTry->Type == pBaseArc->Type )
            nOrdinal++;
    }
    for ( i = 0; i < pAlt->vArcs.nSize; i++ )
    {
        MstaArc *pTry = MstaArcArrayAt(&pAlt->vArcs,i);
        if ( pTry->InPin != pBaseArc->InPin || pTry->OutPin != pBaseArc->OutPin ||
             pTry->Type != pBaseArc->Type )
            continue;
        if ( nOrdinal-- == 0 ) return pTry;
    }
    return NULL;
}

/* 角库的脚按名字对齐默认定义，便于使用角专属输入电容。 */
MstaPin *Msta_LibPinForCorner( MstaLib *pLib, MstaCell *pCell, MstaId PinId,
                               MstaId LibraryName )
{
    MstaCell *pAlt = Msta_LibCellForCorner(pLib,pCell,LibraryName);
    MstaPin *pBasePin;
    int nAlt;
    if ( pCell == NULL || PinId < 0 || PinId >= pCell->vPins.nSize ) return NULL;
    pBasePin = MstaPinArrayAt(&pCell->vPins,PinId);
    if ( pAlt == pCell ) return pBasePin;
    nAlt = Msta_CellPinIndexOf(pAlt,pBasePin->Name);
    return nAlt >= 0 ? MstaPinArrayAt(&pAlt->vPins,nAlt) : pBasePin;
}

int Msta_CellPinIndexOf( MstaCell *pCell, MstaId NameId )
{
    int i;
    for ( i = 0; i < pCell->vPins.nSize; i++ )
        if ( pCell->vPins.pData[i].Name == NameId )
            return i;
    return -1;
}

/* 组合弧查找：同一个 (in,out) 可能有多条（不同 timing_type），
   寄存器单元里 in->out 的边沿弧不算组合弧，除非这个单元不是 FF。 */
/* Liberty function 的小型布尔求值器。只在侧输入已有 case/常量值时用于时钟弧裁剪；
   未识别的语法或过多自由变量回退声明的 timing_sense，保持保守。 */
typedef struct {
    const char *Text;
    MstaCell *Cell;
    const signed char *Values;
    int Valid, Depth;
} MstaBoolExpr;

static void BoolSpace( MstaBoolExpr *p )
{ while ( isspace((unsigned char)*p->Text) ) p->Text++; }
static int BoolOr( MstaBoolExpr *p );
static int BoolXor( MstaBoolExpr *p );
static int BoolAtom( MstaBoolExpr *p )
{
    int Value = 0, i;
    if ( ++p->Depth > 256 ) { p->Valid = 0; p->Depth--; return 0; }
    BoolSpace(p);
    if ( *p->Text == '!' ) { p->Text++; Value = !BoolAtom(p); }
    else if ( *p->Text == '(' )
    {
        p->Text++; Value = BoolOr(p); BoolSpace(p);
        if ( *p->Text != ')' ) p->Valid = 0;
        else p->Text++;
    }
    else
    {
        const char *Start = p->Text;
        while ( *p->Text && (isalnum((unsigned char)*p->Text) || strchr("_[].$/",*p->Text)) ) p->Text++;
        size_t Length = (size_t)(p->Text - Start);
        if ( Length == 1 && (*Start == '0' || *Start == '1') ) Value = *Start - '0';
        else
        {
            int Found = 0;
            for ( i = 0; i < p->Cell->vPins.nSize; i++ )
            {
                const char *Name = Msta_NameStr(p->Cell->vPins.pData[i].Name);
                if ( strlen(Name) == Length && !strncmp(Name,Start,Length) )
                { Value = p->Values[i]; if ( Value < 0 || Value > 1 ) p->Valid = 0; Found = 1; break; }
            }
            if ( !Found || Length == 0 ) p->Valid = 0;
        }
    }
    BoolSpace(p);
    while ( *p->Text == '\'' ) { Value = !Value; p->Text++; BoolSpace(p); }
    p->Depth--;
    return Value;
}
static int BoolAnd( MstaBoolExpr *p )
{
    int Value = BoolXor(p);
    while ( p->Valid )
    {
        BoolSpace(p);
        char Op = *p->Text;
        int Implicit = isalnum((unsigned char)Op) || Op == '_' || Op == '(' || Op == '!';
        if ( Op != '&' && Op != '*' && !Implicit ) break;
        if ( !Implicit ) p->Text++;
        Value &= BoolXor(p);
    }
    return Value;
}
static int BoolXor( MstaBoolExpr *p )
{
    int Value = BoolAtom(p);
    while ( p->Valid )
    {
        BoolSpace(p); if ( *p->Text != '^' ) break;
        p->Text++; Value ^= BoolAtom(p);
    }
    return Value;
}
static int BoolOr( MstaBoolExpr *p )
{
    int Value = BoolAnd(p);
    while ( p->Valid )
    {
        BoolSpace(p); if ( *p->Text != '|' && *p->Text != '+' ) break;
        p->Text++; Value |= BoolAnd(p);
    }
    return Value;
}

int Msta_LibClockSense( MstaCell *pCell, const MstaArc *pArc, const signed char *pCases )
{
    int In = Msta_CellPinIndexOf(pCell,pArc->InPin);
    int Out = Msta_CellPinIndexOf(pCell,pArc->OutPin);
    int i, nFree = 0, Known = 0, Positive = 0, Negative = 0;
    int Free[8];
    signed char *Values;
    if ( In < 0 || Out < 0 || pCell->vPins.pData[Out].pFunc == NULL ) return pArc->Sense;
    for ( i = 0; i < pCell->vPins.nSize; i++ )
        if ( i != In && pCell->vPins.pData[i].Dir != MSTA_DIR_OUTPUT )
        {
            if ( pCases[i] >= 0 ) Known = 1;
            else if ( nFree < 8 ) Free[nFree++] = i;
            else return pArc->Sense;
        }
    if ( !Known ) return pArc->Sense;
    Values = (signed char *)malloc((size_t)pCell->vPins.nSize);
    assert(Values);
    memcpy(Values,pCases,(size_t)pCell->vPins.nSize);
    for ( i = 0; i < (1 << nFree); i++ )
    {
        int k, f, Result[2];
        for ( k = 0; k < nFree; k++ ) Values[Free[k]] = (i >> k) & 1;
        for ( f = 0; f < 2; f++ )
        {
            MstaBoolExpr Expr = {pCell->vPins.pData[Out].pFunc,pCell,Values,1,0};
            Values[In] = (signed char)f;
            Result[f] = BoolOr(&Expr);
            BoolSpace(&Expr);
            if ( !Expr.Valid || *Expr.Text ) { free(Values); return pArc->Sense; }
        }
        if ( Result[0] == 0 && Result[1] == 1 ) Positive = 1;
        if ( Result[0] == 1 && Result[1] == 0 ) Negative = 1;
    }
    free(Values);
    return Positive && Negative ? MSTA_SENSE_NONUNATE : Positive ? MSTA_SENSE_POSITIVE
           : Negative ? MSTA_SENSE_NEGATIVE : -1;
}

MstaArc *Msta_CellCombArc( MstaCell *pCell, MstaId InPin, MstaId OutPin )
{
    int i;
    for ( i = 0; i < pCell->vArcs.nSize; i++ )
    {
        MstaArc *pArc = MstaArcArrayAt( &pCell->vArcs, i );
        if ( pArc->InPin != InPin || pArc->OutPin != OutPin )
            continue;
        if ( pCell->fSequential && pArc->Type != MSTA_TT_COMBINATIONAL )
            continue;
        return pArc;
    }
    /* FF 内部时钟脚->Q 之类在组合查找里查不到，返回 NULL 由调用方决定怎么办。 */
    return NULL;
}

MstaArc *Msta_CellArcById( MstaCell *pCell, MstaId ArcId )
{
    if ( ArcId == MSTA_NO_ID || ArcId >= pCell->vArcs.nSize )
        return NULL;
    return MstaArcArrayAt( &pCell->vArcs, ArcId );
}

/* ---------------------------------------------------------------------
   NLDM 插值：和 ABC sclLib.h:Scl_LibLookup 同一算法
   ---------------------------------------------------------------------
   1) 轴上定位：找到 x 落在 [x_i, x_{i+1}] 的区间，算出权重；越界 clamp 到端点。
   2) 双线性：四个角点加权。
   退化情况：1x1 表直接返回；只有一行/一列时退化为线性插值。 */
static int Msta_TableAxisFind( const double *pAxis, int n, double x, double *pfWeight, const char *pWhat )
{
    int i;
    if ( n <= 1 )
    {   *pfWeight = 0.0; return 0;  }
    if ( x <= pAxis[0] )
    {
        /* 低于第一个索引时按 Liberty 语义线性外插，而不是截断。 */
        double d = pAxis[1] - pAxis[0];
        *pfWeight = (d > 0.0) ? (x - pAxis[0]) / d : 0.0;
        return 0;
    }
    if ( x >= pAxis[n-1] )
    {
        /* 高于最后一个索引时同样线性外插。 */
        double d = pAxis[n-1] - pAxis[n-2];
        *pfWeight = (d > 0.0) ? (x - pAxis[n-2]) / d : 1.0;
        return n-2;
    }
    for ( i = 0; i < n-1; i++ )
        if ( x >= pAxis[i] && x <= pAxis[i+1] )
        {
            double d = pAxis[i+1] - pAxis[i];
            *pfWeight = (d > 0.0) ? (x - pAxis[i]) / d : 0.0;
            return i;
        }
    *pfWeight = 0.0;
    return 0;
}

double Msta_TableLookup( const MstaTable *pTable, double Slew, double Load )
{
    double wRow, wCol;
    double RowValue = pTable->fRowUsesSecond ? Load : Slew;
    double ColValue = pTable->fColUsesFirst ? Slew : Load;
    int iRow, iCol;
    const double *v;

    if ( pTable->nRows == 0 )
        return 0.0;
    if ( pTable->nRows == 1 && pTable->nCols == 1 )
        return pTable->pValues[0];

    iRow = Msta_TableAxisFind( pTable->pRowIndex, pTable->nRows, RowValue, &wRow, "row" );
    iCol = Msta_TableAxisFind( pTable->pColIndex, pTable->nCols, ColValue, &wCol, "column" );

    v = pTable->pValues;
    if ( pTable->nCols == 1 )
        return v[(iRow+0)*pTable->nCols] * (1-wRow) + v[(iRow+1)*pTable->nCols] * wRow;
    if ( pTable->nRows == 1 )
        return v[iCol+0] * (1-wCol) + v[iCol+1] * wCol;

    return ( v[(iRow+0)*pTable->nCols + iCol+0] * (1-wCol) + v[(iRow+0)*pTable->nCols + iCol+1] * wCol ) * (1-wRow)
         + ( v[(iRow+1)*pTable->nCols + iCol+0] * (1-wCol) + v[(iRow+1)*pTable->nCols + iCol+1] * wCol ) * wRow;
}

int Msta_TableExists( const MstaTable *pTable ) { return pTable->nRows > 0; }

void Msta_TableFree( MstaTable *pTable )
{
    free( pTable->pRowIndex ); pTable->pRowIndex = NULL;
    free( pTable->pColIndex ); pTable->pColIndex = NULL;
    free( pTable->pValues );   pTable->pValues = NULL;
    pTable->nRows = pTable->nCols = 0;
}

static void Msta_CellFree( MstaCell *pCell )
{
    int i;
    free( pCell->pLibName );
    for ( i = 0; i < pCell->vPins.nSize; i++ )
        free( pCell->vPins.pData[i].pFunc );
    for ( i = 0; i < pCell->vArcs.nSize; i++ )
    {
        MstaArc *pArc = MstaArcArrayAt( &pCell->vArcs, i );
        Msta_TableFree( &pArc->DelayRise );  Msta_TableFree( &pArc->DelayFall );
        Msta_TableFree( &pArc->TransRise );  Msta_TableFree( &pArc->TransFall );
        Msta_TableFree( &pArc->ConstraintRise );  Msta_TableFree( &pArc->ConstraintFall );
    }
    MstaPinArrayFree( &pCell->vPins );
    MstaArcArrayFree( &pCell->vArcs );
    MstaRegCheckArrayFree( &pCell->vRegs );
    MstaAsyncCheckArrayFree( &pCell->vAsync );
    MstaGateCheckArrayFree( &pCell->vGates );
}

void Msta_LibFree( MstaLib *pLib )
{
    int i;
    if ( pLib == NULL )
        return;
    for ( i = 0; i < pLib->vCells.nSize; i++ )
        Msta_CellFree( MstaCellArrayAt( &pLib->vCells, i ) );
    MstaCellArrayFree( &pLib->vCells );
    for ( i = 0; i < pLib->vCornerCells.nSize; i++ )
        Msta_CellFree( MstaCellArrayAt( &pLib->vCornerCells, i ) );
    MstaCellArrayFree( &pLib->vCornerCells );
    free( pLib->pCellHash );
    for ( i = 0; i < pLib->vLibs.nSize; i++ )
    {
        MstaLibInfo *pInfo = MstaLibInfoArrayAt(&pLib->vLibs,i);
        free( pInfo->pFileName );
        MstaOpCondArrayFree( &pInfo->vOpConds );
    }
    MstaLibInfoArrayFree( &pLib->vLibs );
    free( pLib->pLibName );
    free( pLib->pFileName );
    free( pLib );
}

/* ---------------------------------------------------------------------
   timing_type 文本 -> 枚举
   --------------------------------------------------------------------- */
static const char *s_pTimingTypes[] = {
    "", "combinational", "rising_edge", "falling_edge", "rise_both", "fall_both",
    "setup_rising", "setup_falling", "hold_rising", "hold_falling",
    "clear", "clearp", "preset", "presetp",
    "recovery_rising", "recovery_falling", "removal_rising", "removal_falling",
    "setup_falling_rising", "hold_falling_rising"
};

MstaTimingType Msta_TimingTypeFromName( const char *pName )
{
    if ( !strcasecmp(pName, "combinational") )                     return MSTA_TT_COMBINATIONAL;
    if ( !strcasecmp(pName, "rising_edge") )                       return MSTA_TT_RISE_EDGE;
    if ( !strcasecmp(pName, "falling_edge") )                      return MSTA_TT_FALL_EDGE;
    if ( !strcasecmp(pName, "rise_both") )                         return MSTA_TT_RISE_BOTH;
    if ( !strcasecmp(pName, "fall_both") )                         return MSTA_TT_FALL_BOTH;
    if ( !strcasecmp(pName, "setup_rising") )                      return MSTA_TT_SETUP_RISING;
    if ( !strcasecmp(pName, "setup_falling") )                     return MSTA_TT_SETUP_FALLING;
    if ( !strcasecmp(pName, "hold_rising") )                       return MSTA_TT_HOLD_RISING;
    if ( !strcasecmp(pName, "hold_falling") )                      return MSTA_TT_HOLD_FALLING;
    if ( !strcasecmp(pName, "clear") || !strcasecmp(pName, "clearp") )   return MSTA_TT_CLEAR;
    if ( !strcasecmp(pName, "preset") || !strcasecmp(pName, "presetp") ) return MSTA_TT_PRESET;
    if ( !strcasecmp(pName, "recovery_rising") )                   return MSTA_TT_RECOVERY_RISING;
    if ( !strcasecmp(pName, "recovery_falling") )                  return MSTA_TT_RECOVERY_FALLING;
    if ( !strcasecmp(pName, "removal_rising") )                    return MSTA_TT_REMOVAL_RISING;
    if ( !strcasecmp(pName, "removal_falling") )                   return MSTA_TT_REMOVAL_FALLING;
    (void)s_pTimingTypes;
    return MSTA_TT_UNKNOWN;
}

const char *Msta_TimingTypeName( MstaTimingType Type )
{
    switch ( Type )
    {
        case MSTA_TT_COMBINATIONAL:  return "combinational";
        case MSTA_TT_RISE_EDGE:      return "rising_edge";
        case MSTA_TT_FALL_EDGE:      return "falling_edge";
        case MSTA_TT_RISE_BOTH:      return "rise_both";
        case MSTA_TT_FALL_BOTH:      return "fall_both";
        case MSTA_TT_SETUP_RISING:   return "setup_rising";
        case MSTA_TT_SETUP_FALLING:  return "setup_falling";
        case MSTA_TT_HOLD_RISING:    return "hold_rising";
        case MSTA_TT_HOLD_FALLING:   return "hold_falling";
        case MSTA_TT_CLEAR:          return "clear";
        case MSTA_TT_PRESET:         return "preset";
        case MSTA_TT_RECOVERY_RISING:return "recovery_rising";
        case MSTA_TT_RECOVERY_FALLING:return "recovery_falling";
        case MSTA_TT_REMOVAL_RISING: return "removal_rising";
        case MSTA_TT_REMOVAL_FALLING:return "removal_falling";
        default:                     return "unknown";
    }
}

/* ---------------------------------------------------------------------
   report_lib 用的小结
   --------------------------------------------------------------------- */
void Msta_LibPrintStats( MstaLib *pLib, FILE *pFile )
{
    int i, nFF = 0, nLatch = 0, nComb = 0, nArcs = 0, nChecks = 0, nGates = 0;
    for ( i = 0; i < pLib->vCells.nSize; i++ )
    {
        MstaCell *pCell = MstaCellArrayAt( &pLib->vCells, i );
        nArcs += pCell->vArcs.nSize;
        nGates += pCell->vGates.nSize;
        if ( pCell->fSequential ) nFF++;
        else if ( pCell->fLatch ) nLatch++;
        else if ( !pCell->fIgnore ) nComb++;
        nChecks += pCell->vRegs.nSize;
    }
    if ( pLib->vLibs.nSize <= 1 )
        fprintf( pFile, "Library      : %s  (%s)\n", pLib->pLibName ? pLib->pLibName : "?",
                 pLib->pFileName ? pLib->pFileName : "-" );
    else
    {
        fprintf( pFile, "Libraries    : %d\n", pLib->vLibs.nSize );
        for ( i = 0; i < pLib->vLibs.nSize; i++ )
        {
            MstaLibInfo *pInfo = MstaLibInfoArrayAt( &pLib->vLibs, i );
            fprintf( pFile, "  %-40s %5d cells  %s\n", Msta_NameStr(pInfo->Name),
                     pInfo->nCells, pInfo->pFileName ? pInfo->pFileName : "-" );
        }
    }
    if ( pLib->nCornerVariants > 0 )
        fprintf( pFile, "Corner cells : %d same-name cell definition(s) retained for corner selection\n",
                 pLib->nCornerVariants );
    if ( pLib->nCellsSkipped > 0 )
        fprintf( pFile, "Skipped      : %d duplicate definition(s) within a library\n",
                 pLib->nCellsSkipped );
    fprintf( pFile, "Units        : time %.0f ps/lib-unit, cap %.0f fF/lib-unit\n",
             pLib->TimeScale, pLib->CapScale );
    fprintf( pFile, "Cells        : %d total, %d combinational, %d sequential, %d latch, %d ignored\n",
             pLib->vCells.nSize, nComb, nFF, nLatch, pLib->vCells.nSize - nComb - nFF - nLatch );
    if ( pLib->vCornerCells.nSize > 0 )
        fprintf( pFile, "Corner variants: %d alternate library cell definition(s)\n",
                 pLib->vCornerCells.nSize );
    fprintf( pFile, "Timing arcs  : %d  (setup/hold checks: %d)\n", nArcs, nChecks );
    if ( nGates > 0 )
        fprintf( pFile, "Clock gating : %d enable check(s) declared in the library\n", nGates );
}

/* 打印指定单元的引脚、时序弧和检查信息。 */
void Msta_LibPrintCell( MstaLib *pLib, FILE *pFile, const char *pCellName )
{
    MstaCell *pCell = Msta_LibFindCell( pLib, pCellName );
    int i, j;
    if ( pCell == NULL )
    {
        fprintf( pFile, "cell \"%s\" not found\n", pCellName );
        return;
    }
    fprintf( pFile, "cell %s : %d pins, %d arcs, %d reg checks, area %.4f, %s\n",
             pCell->pLibName, pCell->vPins.nSize, pCell->vArcs.nSize, pCell->vRegs.nSize,
             pCell->Area, pCell->fSequential ? "FF" : (pCell->fLatch ? "LATCH" : "COMB") );
    for ( i = 0; i < pCell->vPins.nSize; i++ )
    {
        MstaPin *pPin = MstaPinArrayAt( &pCell->vPins, i );
        fprintf( pFile, "  pin %-9s dir=%-8s cap=%.6ff%s%s\n",
                 Msta_NameTableName(Msta_Names(), pPin->Name),
                 pPin->Dir == MSTA_DIR_INPUT ? "input" : (pPin->Dir == MSTA_DIR_OUTPUT ? "output" : "other"),
                 pPin->Cap, pPin->fClock ? " clock" : "", pPin->pFunc ? "" : "" );
    }
    for ( i = 0; i < pCell->vArcs.nSize; i++ )
    {
        MstaArc *pArc = MstaArcArrayAt( &pCell->vArcs, i );
        fprintf( pFile, "  arc %s -> %s  %-16s",
                 Msta_NameTableName(Msta_Names(), pArc->InPin),
                 Msta_NameTableName(Msta_Names(), pArc->OutPin),
                 Msta_TimingTypeName(pArc->Type) );
        if ( Msta_TableExists(&pArc->DelayRise) )
            fprintf( pFile, " cell_rise %dx%d [%.3f..%.3f]ps", pArc->DelayRise.nRows, pArc->DelayRise.nCols,
                     pArc->DelayRise.pValues[0], pArc->DelayRise.pValues[pArc->DelayRise.nRows*pArc->DelayRise.nCols-1] );
        if ( Msta_TableExists(&pArc->ConstraintRise) )
            fprintf( pFile, " rise_constraint[0]=%.3fps", pArc->ConstraintRise.pValues[0] );
        if ( Msta_TableExists(&pArc->ConstraintFall) )
            fprintf( pFile, " fall_constraint[0]=%.3fps", pArc->ConstraintFall.pValues[0] );
        fprintf( pFile, "\n" );
    }
    for ( i = 0; i < pCell->vRegs.nSize; i++ )
    {
        MstaRegCheck *pReg = MstaRegCheckArrayAt( &pCell->vRegs, i );
        fprintf( pFile, "  check clk=%s data=%s q=%s %s (setup arc %d, hold arc %d, clk2q arc %d)\n",
                 Msta_NameTableName(Msta_Names(), pReg->ClkPin),
                 Msta_NameTableName(Msta_Names(), pReg->DataPin),
                 Msta_NameTableName(Msta_Names(), pReg->QPin),
                 pReg->fClkRises ? "posedge" : "negedge",
                 pReg->SetupArc, pReg->HoldArc, pReg->ClkToQArc );
    }
    (void)j;
}
