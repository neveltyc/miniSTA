/**CFile***************************************************************

  FileName    [msta_sdc_except.c]

  Synopsis    [SDC 路径例外与附加检查类命令：false path、多周期、max/min delay、group_path、data check、time borrow、门控检查。]

  每条命令一张选项表加一个处理函数；位置参数的形状写在分发表 s_vSdcCommands
  （msta_sdc.c）里。处理函数被调用时语法已经由 msta_sdc_parse.c 的通用解析器
  检查过，只需检查取值、查对象、写模型。
  路径类命令共用的 -from/-through/-to 路径描述（MstaSdcPath）只在本文件用。
  例外表的匹配与索引在 msta_sdc_query.c。

***********************************************************************/

#include "msta_sdc_int.h"

/* ---------------- 路径例外与附加检查 ---------------- */

/* 路径描述：路径类命令（set_false_path、set_multicycle_path、set_max_delay、
   set_min_delay、group_path）共用的 -from / -through / -to 三段，在选项表里写成
   三个带 MSTA_SDC_RF 的 MSTA_SDC_OBJECTS 选项（-through 还带 MSTA_SDC_REPEAT），
   所以 -rise_from、-fall_through 这类写法也认。每个选项带一个 Tcl 参数（名字、
   列表或集合），解析器已经把它拆成了对象名。
   每个 -through 是一组可以互相替代的对象（组内取"或"），多个 -through 按路径上
   的先后顺序依次经过。 */
typedef struct {
    char         **ppFrom;     /* -from 的对象名（指向 argv），nFrom 个；没写时为 0 个 */
    int            nFrom;
    char           FromRF;     /* -rise_from 记 'r'，-fall_from 记 'f'，否则 0 */
    char         **ppTo;
    int            nTo;
    char           ToRF;
    MstaThruObject Thru[MSTA_SDC_MAX_THRU];  /* 每个 -through 先放一个分组分隔条目，
                                               再放这一组的对象 */
    int            nThru;
} MstaSdcPath;

/* 从解析结果里取出路径描述。-through 的条目总数超过 MSTA_SDC_MAX_THRU 时
   告警作废并返回 0。 */
static int Msta_SdcGetPath( MstaSdc *p, const MstaSdcCmd *pCmd, MstaSdcPath *pPath )
{
    int l, k;
    memset( pPath, 0, sizeof(MstaSdcPath) );
    pPath->nFrom  = Msta_SdcOptList( pCmd, "-from", &pPath->ppFrom );
    pPath->FromRF = Msta_SdcOptEdge( pCmd, "-from" );
    pPath->nTo    = Msta_SdcOptList( pCmd, "-to", &pPath->ppTo );
    pPath->ToRF   = Msta_SdcOptEdge( pCmd, "-to" );
    for ( l = 0; l < pCmd->nLists; l++ )
    {
        char **ppWords = pCmd->pppListWords[l];
        if ( strcmp( pCmd->pOpts[ pCmd->pListOpt[l] ].pName, "-through" ) )
            continue;
        if ( pPath->nThru + 1 + pCmd->pListCount[l] > MSTA_SDC_MAX_THRU )
        {
            Msta_SdcReject( p, pCmd, "-through 对象超过 %d 个", MSTA_SDC_MAX_THRU );
            return 0;
        }
        pPath->Thru[pPath->nThru].Text = MSTA_NO_ID;      /* 新的一组从分隔条目开始 */
        pPath->Thru[pPath->nThru].Kind = MSTA_SDC_THRU_SEP;
        pPath->Thru[pPath->nThru].RF   = 0;
        pPath->nThru++;
        for ( k = 0; k < pCmd->pListCount[l]; k++ )
        {
            pPath->Thru[pPath->nThru].Text = Msta_NameId( ppWords[k] );
            pPath->Thru[pPath->nThru].Kind = Msta_SdcKindOf( ppWords[k] );
            pPath->Thru[pPath->nThru].RF   = pCmd->pListEdge[l];
            pPath->nThru++;
        }
    }
    return 1;
}

/* 把路径描述写进例外表：-from 与 -to 的对象取笛卡尔积，每一对一条记录；
   没写 -from（或 -to）时那一端不限定。 */
static void Msta_SdcAddPathExceptions( MstaSdc *p, const MstaSdcPath *pPath, int fFalse,
                                       int fSetup, int fHold, int nCycles,
                                       int fMaxDelay, int fMinDelay, double Delay )
{
    int nFrom = pPath->nFrom > 0 ? pPath->nFrom : 1;
    int nTo   = pPath->nTo   > 0 ? pPath->nTo   : 1;
    int j, k;
    for ( j = 0; j < nFrom; j++ )
        for ( k = 0; k < nTo; k++ )
        {
            MstaException *pEx = MstaExceptionArrayAppend(&p->vExceptions);
            const char *pFrom = pPath->nFrom > 0 ? pPath->ppFrom[j] : NULL;
            const char *pTo   = pPath->nTo   > 0 ? pPath->ppTo[k]   : NULL;
            pEx->FromText = pFrom ? Msta_NameId(pFrom) : MSTA_NO_ID;
            pEx->ToText = pTo ? Msta_NameId(pTo) : MSTA_NO_ID;
            pEx->FromKind = pFrom ? Msta_SdcKindOf(pFrom) : 0;
            pEx->ToKind = pTo ? Msta_SdcKindOf(pTo) : 0;
            pEx->FromRF = pPath->FromRF;
            pEx->ToRF = pPath->ToRF;
            memcpy( pEx->Thru, pPath->Thru, sizeof(pPath->Thru) );
            pEx->nThru = pPath->nThru;
            pEx->nSetupCycles = nCycles;
            /* 只在写了 -hold 时用（fApplyHold）：这时命令里的数字 M 不是周期数，
               而是 hold 沿从（由 setup 推出的）默认位置往回拉的拍数。 */
            pEx->nHoldShift = nCycles;
            pEx->fApplySetup = !fFalse && ( !fHold || fSetup );
            pEx->fApplyHold = !fFalse && fHold;
            pEx->fFalseSetup = fFalse && ( fSetup || !fHold );
            pEx->fFalseHold = fFalse && ( fHold || !fSetup );
            if ( fMaxDelay ) { pEx->fMaxDelay = 1; pEx->MaxDelay = Delay; }
            if ( fMinDelay ) { pEx->fMinDelay = 1; pEx->MinDelay = Delay; }
        }
}

/* set_false_path [-setup] [-hold] [-rise|-fall] [路径选项...]
   set_multicycle_path [-setup] [-hold] [-rise|-fall] [路径选项...] 周期数
   路径选项是 -from/-through/-to 及其 -rise_/-fall_ 写法；只写 -rise 或 -fall 时
   它限定 -to 一端的边沿。周期数必须是正整数。 */
const MstaSdcOpt Msta_SdcPathExceptionOpts[] = {
    { "-setup",   MSTA_SDC_FLAG,    0                             },
    { "-hold",    MSTA_SDC_FLAG,    0                             },
    { "-rise",    MSTA_SDC_FLAG,    0                             },
    { "-fall",    MSTA_SDC_FLAG,    0                             },
    { "-from",    MSTA_SDC_OBJECTS, MSTA_SDC_RF                   },
    { "-through", MSTA_SDC_OBJECTS, MSTA_SDC_RF | MSTA_SDC_REPEAT },
    { "-to",      MSTA_SDC_OBJECTS, MSTA_SDC_RF                   },
    { NULL,       MSTA_SDC_FLAG,    0                             } };

static void Msta_SdcSetPathException( MstaSdc *p, int fFalse, MstaSdcCmd *pCmd )
{
    double Cycles = 1.0;
    MstaSdcPath Path;
    if ( !fFalse )
    {
        if ( !Msta_SdcGetNumber( p, pCmd, "周期数", pCmd->pValue, MSTA_SDC_POSITIVE, &Cycles ) )
            return;
        if ( Cycles != (double)(int)Cycles )
        { Msta_SdcReject( p, pCmd, "周期数必须是正整数（给的是 %s）", pCmd->pValue ); return; }
    }
    if ( !Msta_SdcGetPath( p, pCmd, &Path ) )
        return;
    if ( Path.ToRF == 0 )
        Path.ToRF = Msta_SdcCmdEdge( pCmd );
    Msta_SdcAddPathExceptions( p, &Path, fFalse, Msta_SdcHasFlag( pCmd, "-setup" ),
                               Msta_SdcHasFlag( pCmd, "-hold" ), (int)Cycles, 0, 0, 0.0 );
}

void Msta_SdcSetFalsePath( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    Msta_SdcSetPathException( p, 1, pCmd );
}

void Msta_SdcSetMulticyclePath( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    Msta_SdcSetPathException( p, 0, pCmd );
}

/* set_max_delay / set_min_delay [路径选项...] 延迟
   路径选项同 set_false_path。 */
const MstaSdcOpt Msta_SdcPathDelayOpts[] = {
    { "-from",    MSTA_SDC_OBJECTS, MSTA_SDC_RF                   },
    { "-through", MSTA_SDC_OBJECTS, MSTA_SDC_RF | MSTA_SDC_REPEAT },
    { "-to",      MSTA_SDC_OBJECTS, MSTA_SDC_RF                   },
    { NULL,       MSTA_SDC_FLAG,    0                             } };

static void Msta_SdcSetPathDelay( MstaSdc *p, int fMax, MstaSdcCmd *pCmd )
{
    MstaSdcPath Path;
    if ( !Msta_SdcGetPath( p, pCmd, &Path ) )
        return;
    Msta_SdcAddPathExceptions( p, &Path, 0, 0, 0, 1, fMax, !fMax, Msta_SdcToPs( p, pCmd->pValue ) );
}

void Msta_SdcSetMaxDelay( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    Msta_SdcSetPathDelay( p, 1, pCmd );
}

void Msta_SdcSetMinDelay( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    Msta_SdcSetPathDelay( p, 0, pCmd );
}

/* group_path -name 组名 | -default [-weight 权重] [-critical_range 值] [路径选项...]
   把命中的路径归到一个分组里，报告按组统计 WNS/TNS。
   分组不影响 slack；-weight 只记录并在报告里显示，不参与 WNS/TNS 等数字的计算。
   -name 与 -default 必须二选一。不写路径选项就是"所有路径"（-default 常这么用）。
   -critical_range 不建模，分组收下所有命中的路径。 */
const MstaSdcOpt Msta_SdcGroupPathOpts[] = {
    { "-default",        MSTA_SDC_FLAG,    0                             },
    { "-name",           MSTA_SDC_VALUE,   0                             },
    { "-weight",         MSTA_SDC_VALUE,   0                             },
    { "-critical_range", MSTA_SDC_VALUE,   MSTA_SDC_IGNORE               },
    { "-from",           MSTA_SDC_OBJECTS, MSTA_SDC_RF                   },
    { "-through",        MSTA_SDC_OBJECTS, MSTA_SDC_RF | MSTA_SDC_REPEAT },
    { "-to",             MSTA_SDC_OBJECTS, MSTA_SDC_RF                   },
    { NULL,              MSTA_SDC_FLAG,    0                             } };

void Msta_SdcSetGroupPath( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    const char *pName   = Msta_SdcOptValue( pCmd, "-name" );
    const char *pWeight = Msta_SdcOptValue( pCmd, "-weight" );
    int fDefault = Msta_SdcHasFlag( pCmd, "-default" );
    double Weight = 1.0;
    MstaSdcPath Path;
    int j, k;

    if ( fDefault == ( pName != NULL ) )
    { Msta_SdcReject( p, pCmd, "-name 和 -default 必须写且只能写一个" ); return; }
    if ( pWeight && !Msta_SdcGetNumber( p, pCmd, "-weight", pWeight, MSTA_SDC_POSITIVE, &Weight ) )
        return;
    if ( !Msta_SdcGetPath( p, pCmd, &Path ) )
        return;
    /* -from/-to 的多个对象按笛卡尔积摊成多条记录，名字与权重相同（与例外一致）。 */
    for ( j = 0; j < ( Path.nFrom > 0 ? Path.nFrom : 1 ); j++ )
        for ( k = 0; k < ( Path.nTo > 0 ? Path.nTo : 1 ); k++ )
        {
            const char *pFrom = Path.nFrom > 0 ? Path.ppFrom[j] : NULL;
            const char *pTo   = Path.nTo   > 0 ? Path.ppTo[k]   : NULL;
            MstaPathGroup *pGroup = MstaPathGroupArrayAppend( &p->vPathGroups );
            pGroup->Name    = fDefault ? MSTA_NO_ID : Msta_NameId( pName );
            pGroup->fDefault= fDefault;
            pGroup->Weight  = Weight;
            pGroup->FromText= pFrom ? Msta_NameId(pFrom) : MSTA_NO_ID;
            pGroup->ToText  = pTo   ? Msta_NameId(pTo)   : MSTA_NO_ID;
            pGroup->FromKind= pFrom ? Msta_SdcKindOf(pFrom) : 0;
            pGroup->ToKind  = pTo   ? Msta_SdcKindOf(pTo)   : 0;
            pGroup->FromRF  = Path.FromRF;
            pGroup->ToRF    = Path.ToRF;
            memcpy( pGroup->Thru, Path.Thru, sizeof(Path.Thru) );
            pGroup->nThru   = Path.nThru;
        }
}

/* set_data_check -from|-rise_from|-fall_from A -to|-rise_to|-fall_to B
       [-setup|-hold] [-clock C] 余量
   两条数据路径之间的检查，-from 是参照；-from/-to 各是一个对象，解析成一个网络。
   -clock 读入但不使用；-rise/-fall 不建模。不写 -setup/-hold 时两个都查。 */
const MstaSdcOpt Msta_SdcDataCheckOpts[] = {
    { "-from",  MSTA_SDC_OBJECTS, MSTA_SDC_RF     },
    { "-to",    MSTA_SDC_OBJECTS, MSTA_SDC_RF     },
    { "-clock", MSTA_SDC_VALUE,   0               },
    { "-setup", MSTA_SDC_FLAG,    0               },
    { "-hold",  MSTA_SDC_FLAG,    0               },
    { "-rise",  MSTA_SDC_FLAG,    MSTA_SDC_IGNORE },
    { "-fall",  MSTA_SDC_FLAG,    MSTA_SDC_IGNORE },
    { NULL,     MSTA_SDC_FLAG,    0               } };

void Msta_SdcSetDataCheck( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    char **ppFrom, **ppTo;
    int nFrom = Msta_SdcOptList( pCmd, "-from", &ppFrom );
    int nTo   = Msta_SdcOptList( pCmd, "-to", &ppTo );
    int *pFromNets, *pToNets;
    MstaDataCheck *pCheck;

    if ( nFrom == 0 || nTo == 0 )
    { Msta_SdcReject( p, pCmd, "需要 -from 和 -to" ); return; }
    if ( nFrom != 1 || nTo != 1 ||
         Msta_SdcResolveNets( pDes, ppFrom[0], &pFromNets ) != 1 ||
         Msta_SdcResolveNets( pDes, ppTo[0], &pToNets ) != 1 )
    { Msta_SdcReject( p, pCmd, "-from/-to 必须各自对应唯一一个网络" ); return; }
    pCheck = MstaDataCheckArrayAppend( &p->vDataChecks );
    pCheck->FromNet  = pFromNets[0];
    pCheck->ToNet    = pToNets[0];
    pCheck->FromText = Msta_NameId( ppFrom[0] );
    pCheck->ToText   = Msta_NameId( ppTo[0] );
    pCheck->FromRF   = Msta_SdcOptEdge( pCmd, "-from" );
    pCheck->ToRF     = Msta_SdcOptEdge( pCmd, "-to" );
    pCheck->fSetup   = Msta_SdcHasFlag( pCmd, "-setup" ) || !Msta_SdcHasFlag( pCmd, "-hold" );
    pCheck->fHold    = Msta_SdcHasFlag( pCmd, "-hold" ) || !Msta_SdcHasFlag( pCmd, "-setup" );
    pCheck->Value    = Msta_SdcToPs( p, pCmd->pValue );
}

/* set_max_time_borrow 延迟 [锁存器实例或库单元...]
   锁存器 D 脚允许比使能脚关闭沿晚到多久。不写对象列表就是所有锁存器的默认值
   （手册里对象列表是必写的，这里放宽）。 */
void Msta_SdcSetMaxTimeBorrow( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    MstaBorrowSdc *pRec;
    double Value;
    int *pInsts, nInsts = 1, i;
    if ( !Msta_SdcGetNumber( p, pCmd, "值", pCmd->pValue, MSTA_SDC_NONNEG, &Value ) )
        return;
    if ( pCmd->nObjs > 0 &&
         ( nInsts = Msta_SdcObjectInsts( p, pDes, pLib, pCmd, pCmd->ppObjs, pCmd->nObjs, 1, &pInsts ) ) < 0 )
        return;
    for ( i = 0; i < nInsts; i++ )
    {
        pRec = MstaBorrowSdcArrayAppend( &p->vBorrow );
        pRec->Inst  = pCmd->nObjs > 0 ? pInsts[i] : -1;
        pRec->Value = Value * p->TimeScalePs;
    }
}

/* set_clock_gating_check [-setup 值] [-hold 值] [-rise|-fall|-high|-low] [门控实例或库单元...]
   门控单元使能脚相对时钟脚的 setup/hold 值。
   没有对象列表时作为全局默认；带对象列表时按实例（或库单元名）覆盖。
   -rise/-fall/-high/-low 不建模：检查对象总按库里声明的有效沿。 */
const MstaSdcOpt Msta_SdcClockGatingCheckOpts[] = {
    { "-setup", MSTA_SDC_VALUE, 0               },
    { "-hold",  MSTA_SDC_VALUE, 0               },
    { "-rise",  MSTA_SDC_FLAG,  MSTA_SDC_IGNORE },
    { "-fall",  MSTA_SDC_FLAG,  MSTA_SDC_IGNORE },
    { "-high",  MSTA_SDC_FLAG,  MSTA_SDC_IGNORE },
    { "-low",   MSTA_SDC_FLAG,  MSTA_SDC_IGNORE },
    { NULL,     MSTA_SDC_FLAG,  0               } };

void Msta_SdcSetClockGatingCheck( MstaSdc *p, MstaDesign *pDes, MstaLib *pLib, MstaSdcCmd *pCmd )
{
    const char *pSetup = Msta_SdcOptValue( pCmd, "-setup" );
    const char *pHold  = Msta_SdcOptValue( pCmd, "-hold" );
    double Setup = MSTA_UNSET, Hold = MSTA_UNSET;
    int *pInsts, nInsts = 1, i;
    if ( pSetup == NULL && pHold == NULL )
    { Msta_SdcReject( p, pCmd, "至少需要 -setup 或 -hold 之一" ); return; }
    if ( ( pSetup && !Msta_SdcGetNumber( p, pCmd, "-setup", pSetup, MSTA_SDC_ANY, &Setup ) ) ||
         ( pHold  && !Msta_SdcGetNumber( p, pCmd, "-hold",  pHold,  MSTA_SDC_ANY, &Hold ) ) )
        return;
    if ( pSetup ) Setup *= p->TimeScalePs;
    if ( pHold )  Hold  *= p->TimeScalePs;
    if ( pCmd->nObjs > 0 &&
         ( nInsts = Msta_SdcObjectInsts( p, pDes, pLib, pCmd, pCmd->ppObjs, pCmd->nObjs, 1, &pInsts ) ) < 0 )
        return;
    for ( i = 0; i < nInsts; i++ )
    {
        MstaClockGatingSdc *pRec = MstaClockGatingSdcArrayAppend( &p->vClockGating );
        pRec->Inst  = pCmd->nObjs > 0 ? pInsts[i] : -1;
        pRec->Setup = Setup;
        pRec->Hold  = Hold;
        pRec->fRise = pRec->fFall = pRec->fHigh = pRec->fLow = 0;
    }
}
