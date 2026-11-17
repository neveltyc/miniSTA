/**CFile***************************************************************

  FileName    [msta_cmds.c]

  Synopsis    [命令表与脚本切分。]

***********************************************************************/

#include "msta_cmds.h"
#include "msta_util.h"
#include "msta_net.h"
#include "msta_sdc.h"
#include "msta_timing.h"
#include "msta_report.h"
#include <math.h>
#include <stdlib.h>
#include <unistd.h>

/* ---------------------------------------------------------------------
   全局开关（-q / -o）
   --------------------------------------------------------------------- */
static int   s_fQuiet = 0;
static char *s_pOutName = NULL;

void Msta_CmdsSetQuiet( int fQuiet )   { s_fQuiet = fQuiet; }
void Msta_CmdsSetOutput( const char *pFileName )
{
    free( s_pOutName );
    s_pOutName = pFileName ? Msta_StrDup( pFileName ) : NULL;
}

/* ---------------------------------------------------------------------
   MstaApp
   --------------------------------------------------------------------- */
MstaApp *Msta_AppStart( void )
{
    MstaApp *pApp = (MstaApp *)calloc( 1, sizeof(MstaApp) );
    assert( pApp );
    pApp->pLib    = Msta_LibStart();
    pApp->pNet    = Msta_DesignStart();
    pApp->pSdc    = Msta_SdcStart();
    pApp->pTime   = NULL;
    Msta_PluginSetInitDefaults( &pApp->Plugins );
    pApp->pOut    = s_pOutName ? fopen( s_pOutName, "w" ) : stdout;
    pApp->fQuiet  = s_fQuiet;
    if ( pApp->pOut == NULL )
    {
        Msta_Error( "无法把报告写到 \"%s\"\n", s_pOutName );
        pApp->pOut = stdout;
    }
    return pApp;
}

void Msta_AppFree( MstaApp *pApp )
{
    if ( pApp == NULL )
        return;
    Msta_LibFree( pApp->pLib );
    Msta_DesignFree( pApp->pNet );
    Msta_SdcFree( pApp->pSdc );
    Msta_TimingFree( pApp->pTime );
    if ( pApp->pOut != stdout && pApp->pOut )
        fclose( pApp->pOut );
    free( pApp );
}

/* 网表/约束一旦变化，旧的分析结果就不再可信。 */
static void Msta_AppDropTiming( MstaApp *pApp )
{
    Msta_TimingFree( pApp->pTime );
    pApp->pTime = NULL;
}

/* ---------------------------------------------------------------------
   命令实现
   --------------------------------------------------------------------- */

static int Msta_CmdHelp( MstaApp *pApp, int argc, char **argv );

/* 当前 dofile 所在目录；-c "命令串" 时为空，表示按进程当前目录解释相对路径。 */
static char s_pScriptDir[1024] = "";

/* 解析脚本里写的相对路径：先按进程当前目录找，打不开再拼到 dofile 所在目录下。
   这样用例可以整目录搬走、在别处运行，脚本内容不用改（商用工具靠 Tcl 的
   [file dirname [info script]] 做到同样的事）。
   结果写进调用方给的缓冲区 —— 不用静态缓冲，免得一次命令里解析多个路径时互相覆盖。 */
static void Msta_ResolvePath( const char *pIn, char *pOut, int nOut )
{
    if ( pIn[0] == '/' || s_pScriptDir[0] == 0 )
    {
        snprintf( pOut, (size_t)nOut, "%s", pIn );
        return;
    }
    {
        FILE *pTry = fopen( pIn, "rb" );
        if ( pTry )
        {
            fclose( pTry );
            snprintf( pOut, (size_t)nOut, "%s", pIn );
            return;
        }
    }
    {
        size_t nDir = strlen( s_pScriptDir );
        size_t nIn  = strlen( pIn );
        if ( nDir + nIn + 2 > (size_t)nOut )
        {
            pOut[0] = 0;
            Msta_Error( "路径太长：\"%s/%s\"\n", s_pScriptDir, pIn );
            return;
        }
        memcpy( pOut, s_pScriptDir, nDir );
        pOut[nDir] = '/';
        memcpy( pOut + nDir + 1, pIn, nIn + 1 );
    }
}

/* 记下 dofile 所在目录，供 Msta_ResolvePath 在当前目录找不到文件时兜底。 */
void Msta_CmdsSetScriptFile( const char *pScriptPath )
{
    char *pSlash;
    snprintf( s_pScriptDir, sizeof(s_pScriptDir), "%s", pScriptPath );
    pSlash = strrchr( s_pScriptDir, '/' );
    if ( pSlash )
        *pSlash = 0;
    else
        s_pScriptDir[0] = 0;
}

#define MSTA_MAX_FILES_PER_COMMAND 8

static int Msta_CmdReadLiberty( MstaApp *pApp, int argc, char **argv )
{
    char sPath[MSTA_MAX_FILES_PER_COMMAND][512];
    int i, fOk = 1;
    if ( argc < 2 || argc - 1 > MSTA_MAX_FILES_PER_COMMAND )
    {
        Msta_Error( "用法：read_liberty <file.lib> [more.lib ...]\n" );
        return 0;
    }
    Msta_AppDropTiming( pApp );
    for ( i = 1; i < argc; i++ )
    {
        Msta_ResolvePath( argv[i], sPath[i-1], (int)sizeof(sPath[0]) );
        if ( !Msta_LibRead( pApp->pLib, sPath[i-1], !pApp->fQuiet ) )
            fOk = 0;
    }
    if ( pApp->pNet->vInsts.nSize > 0 )
        Msta_DesignRefreshInstCells( pApp->pNet, pApp->pLib );
    return fOk;
}

static int Msta_CmdReportLib( MstaApp *pApp, int argc, char **argv )
{
    (void)argc; (void)argv;
    Msta_LibPrintStats( pApp->pLib, pApp->pOut );
    return 1;
}

static int Msta_CmdPrintCell( MstaApp *pApp, int argc, char **argv )
{
    int i;
    if ( argc < 2 )
    {
        Msta_Error( "用法：print_cell <cell_name> [...]\n" );
        return 0;
    }
    for ( i = 1; i < argc; i++ )
        Msta_LibPrintCell( pApp->pLib, pApp->pOut, argv[i] );
    return 1;
}

static int Msta_CmdReadVerilog( MstaApp *pApp, int argc, char **argv )
{
    char sResolved[MSTA_MAX_FILES_PER_COMMAND][512];
    const char *pFiles[MSTA_MAX_FILES_PER_COMMAND];
    const char *pTmp = getenv( "TMPDIR" );
    char sWorkDir[512];
    int i, fOk;
    if ( argc < 2 || argc - 1 > MSTA_MAX_FILES_PER_COMMAND )
    {
        Msta_Error( "用法：read_verilog <a.v> [b.v ...]（最多 %d 个文件）\n",
                    MSTA_MAX_FILES_PER_COMMAND );
        return 0;
    }
    Msta_AppDropTiming( pApp );
    if ( pApp->pNet->vNets.nSize > 0 )
    {
        Msta_DesignClearFlattened( pApp->pNet );
        Msta_DesignClearModules( pApp->pNet );
        Msta_SdcResetForDesign( &pApp->pSdc );
    }
    for ( i = 1; i < argc; i++ )
    {
        Msta_ResolvePath( argv[i], sResolved[i-1], (int)sizeof(sResolved[0]) );
        pFiles[i-1] = sResolved[i-1];
    }
    snprintf( sWorkDir, sizeof(sWorkDir), "%s/msta-XXXXXX",
              ( pTmp != NULL && pTmp[0] != 0 ) ? pTmp : "/tmp" );
    if ( mkdtemp( sWorkDir ) == NULL )
    {
        Msta_Error( "read_verilog：无法创建工作目录 \"%s\"\n", sWorkDir );
        return 0;
    }
    fOk = Msta_DesignReadVerilog( pApp->pNet, pFiles, argc - 1, sWorkDir, !pApp->fQuiet );
    rmdir( sWorkDir );
    return fOk;
}

static int Msta_CmdReadJson( MstaApp *pApp, int argc, char **argv )
{
    char sPath[512];
    if ( argc < 2 )
    {
        Msta_Error( "用法：read_json <yosys write_json 生成的文件>\n" );
        return 0;
    }
    Msta_AppDropTiming( pApp );
    if ( pApp->pNet->vNets.nSize > 0 )
    {
        Msta_DesignClearFlattened( pApp->pNet );
        Msta_DesignClearModules( pApp->pNet );
        Msta_SdcResetForDesign( &pApp->pSdc );
    }
    Msta_ResolvePath( argv[1], sPath, (int)sizeof(sPath) );
    return Msta_DesignReadYosysJson( pApp->pNet, sPath, !pApp->fQuiet );
}

/* current_design 会做展平：展平要查库里的引脚，所以必须在 read_liberty 之后。 */
static int Msta_CmdCurrentDesign( MstaApp *pApp, int argc, char **argv )
{
    const char *pTop = ( argc > 1 ) ? argv[1] : NULL;
    if ( pApp->pNet->vModules.nSize == 0 )
    {
        Msta_Error( "current_design：还没有读入网表\n" );
        return 0;
    }
    Msta_AppDropTiming( pApp );
    if ( pApp->pNet->vNets.nSize > 0 )
        Msta_SdcResetForDesign( &pApp->pSdc );
    return Msta_DesignFlatten( pApp->pNet, pApp->pLib, pTop, !pApp->fQuiet );
}

static int Msta_CmdReportDesign( MstaApp *pApp, int argc, char **argv )
{
    (void)argc; (void)argv;
    if ( pApp->pNet->vInsts.nSize == 0 )
    {
        Msta_Error( "report_design：设计还没有展平（请先执行 current_design）\n" );
        return 0;
    }
    Msta_DesignPrintStats( pApp->pNet, pApp->pOut );
    return 1;
}

static int Msta_CmdReadSdc( MstaApp *pApp, int argc, char **argv )
{
    char sPath[512];
    if ( argc < 2 )
    {
        Msta_Error( "用法：read_sdc <file.sdc>\n" );
        return 0;
    }
    if ( pApp->pNet->vNets.nSize == 0 )
    {
        Msta_Error( "read_sdc：请先展平设计（current_design）\n" );
        return 0;
    }
    Msta_AppDropTiming( pApp );
    Msta_ResolvePath( argv[1], sPath, (int)sizeof(sPath) );
    return Msta_SdcReadFile( pApp->pSdc, pApp->pNet, pApp->pLib, sPath, !pApp->fQuiet );
}

/* 成功换表后丢弃旧时序结果；读表失败仍保留旧模型和缓存。 */
static int Msta_CmdReadAocv( MstaApp *pApp, int argc, char **argv )
{
    char Path[1024];
    if ( argc != 2 )
    {
        Msta_Error( "用法：read_aocv file.aocv\n" );
        return 0;
    }
    Msta_ResolvePath( argv[1], Path, sizeof(Path) );
    if ( !Msta_AocvRead( pApp->pSdc, Path, !pApp->fQuiet ) )
        return 0;
    Msta_AppDropTiming( pApp );
    return 1;
}

/* 表报告只读配置，不触发时序分析。 */
static int Msta_CmdReportAocv( MstaApp *pApp, int argc, char **argv )
{
    if ( argc != 1 )
    {
        Msta_Error( "用法：report_aocv\n" );
        return 0;
    }
    Msta_AocvReport( pApp->pSdc, pApp->pOut );
    return 1;
}

static int Msta_CmdReportClocks( MstaApp *pApp, int argc, char **argv )
{
    (void)argc; (void)argv;
    Msta_SdcPrintClocks( pApp->pSdc, pApp->pNet, pApp->pOut );
    return 1;
}

/* 时序分析：没有 pTime 就现建一个，之后各个 report_* 共用它，直到网表/约束变化被丢弃。 */
static MstaTiming *Msta_AppTiming( MstaApp *pApp )
{
    if ( pApp->pTime == NULL )
    {
        pApp->pTime = Msta_TimingStart( pApp->pNet, pApp->pLib, pApp->pSdc );
        Msta_TimingSetPlugins( pApp->pTime, &pApp->Plugins );
    }
    return pApp->pTime;
}

/* 若设计已展平但尚未跑过时序分析，则触发一遍；结果在下次网表/约束修改前一直复用。 */
static int Msta_EnsureAnalyzed( MstaApp *pApp, const char *pCmdName )
{
    MstaTiming *pTime;
    if ( pApp->pNet->vNets.nSize == 0 )
    {
        Msta_Error( "%s：请先展平设计（current_design）\n", pCmdName );
        return 0;
    }
    pTime = Msta_AppTiming( pApp );
    if ( pTime->fAnalyzed )
        return 1;
    return Msta_TimingAnalyze( pTime, !pApp->fQuiet );
}

static int Msta_CmdReportChecks( MstaApp *pApp, int argc, char **argv )
{
    int nMaxPaths = 1;
    int fSetup = 1;
    int Flags = 0;
    int i;
    for ( i = 1; i < argc; i++ )
    {
        if ( !strcmp(argv[i], "-hold") )       fSetup = 0;
        else if ( !strcmp(argv[i], "-setup") ) fSetup = 1;
        else if ( !strcmp(argv[i], "-tran") )  Flags |= MSTA_RPT_TRAN;
        else if ( !strcmp(argv[i], "-cap") )   Flags |= MSTA_RPT_CAP;
        else if ( !strcmp(argv[i], "-clock_path") ) Flags |= MSTA_RPT_CLOCK_PATH;
        else if ( !strcmp(argv[i], "-max_paths") )
        {
            if ( i + 1 >= argc )
            {
                Msta_Error( "report_checks：选项 \"-max_paths\" 缺少参数\n" );
                return 0;
            }
            nMaxPaths = atoi( argv[++i] );
        }
        else Msta_WarnOnce( "report_checks：选项 \"%s\" 不支持，已忽略", argv[i] );
    }
    if ( nMaxPaths < 1 )
    {
        Msta_WarnOnce( "report_checks：-max_paths 必须大于 0；按 1 处理" );
        nMaxPaths = 1;
    }
    if ( !Msta_EnsureAnalyzed(pApp, "report_checks") )
        return 0;
    Msta_ReportChecksEx( Msta_AppTiming(pApp), pApp->pOut, nMaxPaths, fSetup, Flags );
    Msta_ReportSummary( Msta_AppTiming(pApp), pApp->pOut );
    return 1;
}

static int Msta_CmdReportSummary( MstaApp *pApp, int argc, char **argv )
{
    (void)argc; (void)argv;
    if ( !Msta_EnsureAnalyzed(pApp, "report_summary") )
        return 0;
    Msta_ReportSummary( Msta_AppTiming(pApp), pApp->pOut );
    return 1;
}

static int Msta_CmdReportClockTree( MstaApp *pApp, int argc, char **argv )
{
    (void)argc; (void)argv;
    if ( !Msta_EnsureAnalyzed(pApp, "report_clock_tree") )
        return 0;
    Msta_ReportClockTree( Msta_AppTiming(pApp), pApp->pOut );
    return 1;
}

static int Msta_CmdExplainArc( MstaApp *pApp, int argc, char **argv )
{
    const char *pPos[3];
    int nPos = 0, fMax = 1, i;
    for ( i = 1; i < argc; i++ )
    {
        if ( !strcmp(argv[i], "-max") )      fMax = 1;
        else if ( !strcmp(argv[i], "-min") ) fMax = 0;
        else if ( argv[i][0] == '-' )
        {
            Msta_Error( "explain_arc：不支持的选项 \"%s\"\n", argv[i] );
            return 0;
        }
        else if ( nPos < 3 )
            pPos[nPos++] = argv[i];
        else
            nPos++;
    }
    if ( nPos != 3 )
    {
        Msta_Error( "用法：explain_arc <inst> <in_pin> <out_pin> [-max|-min]\n" );
        return 0;
    }
    if ( !Msta_EnsureAnalyzed(pApp, "explain_arc") )
        return 0;
    return Msta_ReportExplainArc( Msta_AppTiming(pApp), pApp->pOut,
                                  pPos[0], pPos[1], pPos[2], fMax );
}

static int Msta_CmdReportNet( MstaApp *pApp, int argc, char **argv )
{
    if ( argc != 2 )
    {
        Msta_Error( "用法：report_net <net_name>\n" );
        return 0;
    }
    if ( !Msta_EnsureAnalyzed(pApp, "report_net") )
        return 0;
    return Msta_ReportNet( Msta_AppTiming(pApp), pApp->pOut, argv[1] );
}

static int Msta_CmdDumpGraph( MstaApp *pApp, int argc, char **argv )
{
    const char *pOutPath = NULL;
    int nMaxNodes = 200, i, fOk;
    FILE *pFile;
    for ( i = 1; i < argc; i++ )
    {
        if ( !strcmp(argv[i], "-o") && i + 1 < argc )
            pOutPath = argv[++i];
        else if ( !strcmp(argv[i], "-max_nodes") && i + 1 < argc )
            nMaxNodes = atoi( argv[++i] );
        else
        {
            Msta_Error( "用法：dump_graph [-o <dot_file>] [-max_nodes N]\n" );
            return 0;
        }
    }
    if ( nMaxNodes < 1 )
    {
        Msta_WarnOnce( "dump_graph：-max_nodes 必须大于 0；按 200 处理" );
        nMaxNodes = 200;
    }
    if ( !Msta_EnsureAnalyzed(pApp, "dump_graph") )
        return 0;
    if ( pOutPath == NULL )
        return Msta_ReportDumpGraph( Msta_AppTiming(pApp), pApp->pOut, nMaxNodes );
    pFile = fopen( pOutPath, "w" );
    if ( pFile == NULL )
    {
        Msta_Error( "dump_graph：无法打开输出文件 \"%s\"\n", pOutPath );
        return 0;
    }
    fOk = Msta_ReportDumpGraph( Msta_AppTiming(pApp), pFile, nMaxNodes );
    fclose( pFile );
    return fOk;
}

static int Msta_ParseNonNegDouble( const char *pStr, double *pOut )
{
    char *pEnd = NULL;
    double v;
    if ( pStr == NULL || !pStr[0] )
        return 0;
    v = strtod( pStr, &pEnd );
    if ( pEnd == pStr || *pEnd != 0 || !isfinite(v) || v < 0.0 )
        return 0;
    *pOut = v;
    return 1;
}

/* set_wire_model zero_rc | elmore_rc [-r <kohm>] [-c <ff>] */
static int Msta_CmdSetWireModel( MstaApp *pApp, int argc, char **argv )
{
    int i;
    double R = 0.05, C = 2.0;
    const char *pName;
    if ( argc < 2 )
    {
        Msta_Error( "用法：set_wire_model zero_rc|elmore_rc [-r <kohm>] [-c <ff>]\n" );
        return 0;
    }
    pName = argv[1];
    if ( !strcmp(pName, "zero_rc") )
    {
        if ( argc > 2 )
        {
            Msta_Error( "set_wire_model：zero_rc 不接受额外选项 \"%s\"\n", argv[2] );
            return 0;
        }
        pApp->Plugins.pWire = Msta_PluginWireZeroRc();
    }
    else if ( !strcmp(pName, "elmore_rc") )
    {
        for ( i = 2; i < argc; i++ )
        {
            if ( !strcmp(argv[i], "-r") )
            {
                if ( i + 1 >= argc || !Msta_ParseNonNegDouble(argv[++i], &R) )
                {
                    Msta_Error( "set_wire_model：-r 需要非负数值（kOhm）\n" );
                    return 0;
                }
            }
            else if ( !strcmp(argv[i], "-c") )
            {
                if ( i + 1 >= argc || !Msta_ParseNonNegDouble(argv[++i], &C) )
                {
                    Msta_Error( "set_wire_model：-c 需要非负数值（fF）\n" );
                    return 0;
                }
            }
            else
            {
                Msta_Error( "set_wire_model：不支持选项 \"%s\"\n", argv[i] );
                return 0;
            }
        }
        Msta_PluginInitElmoreWire( &pApp->CustomWireOps, &pApp->ElmoreCfg, R, C );
        pApp->Plugins.pWire = &pApp->CustomWireOps;
    }
    else
    {
        Msta_Error( "set_wire_model：未知线网模型 \"%s\"（可选 zero_rc / elmore_rc）\n", pName );
        return 0;
    }
    if ( pApp->pTime != NULL )
        Msta_TimingSetPlugins( pApp->pTime, &pApp->Plugins );
    return 1;
}

/* set_path_refiner none | pba_retrace */
static int Msta_CmdSetPathRefiner( MstaApp *pApp, int argc, char **argv )
{
    if ( argc != 2 )
    {
        Msta_Error( "用法：set_path_refiner none|pba_retrace\n" );
        return 0;
    }
    if ( !strcmp(argv[1], "none") )
        pApp->Plugins.pRefiner = Msta_PluginRefinerNone();
    else if ( !strcmp(argv[1], "pba_retrace") )
        pApp->Plugins.pRefiner = Msta_PluginRefinerPbaRetrace();
    else
    {
        Msta_Error( "set_path_refiner：未知精修器 \"%s\"（可选 none / pba_retrace）\n", argv[1] );
        return 0;
    }
    if ( pApp->pTime != NULL )
        Msta_TimingSetPlugins( pApp->pTime, &pApp->Plugins );
    return 1;
}

/* swap_cell <inst> <new_cell> [-verify] */
static int Msta_CmdSwapCell( MstaApp *pApp, int argc, char **argv )
{
    int i, fVerify = 0, nInst;
    const char *pInstName, *pCellName;
    MstaCell *pNewCell;
    MstaSwapStats Stats;
    char sSetup[24], sHold[24];

    if ( argc < 3 )
    {
        Msta_Error( "用法：swap_cell <inst> <new_cell> [-verify]\n" );
        return 0;
    }
    pInstName = argv[1];
    pCellName = argv[2];
    for ( i = 3; i < argc; i++ )
    {
        if ( !strcmp(argv[i], "-verify") )
            fVerify = 1;
        else
        {
            Msta_Error( "swap_cell：不支持选项 \"%s\"\n", argv[i] );
            return 0;
        }
    }
    if ( !Msta_EnsureAnalyzed(pApp, "swap_cell") )
        return 0;
    nInst = Msta_DesignFindInstByName( pApp->pNet, pInstName );
    if ( nInst < 0 )
    {
        Msta_Error( "swap_cell：找不到实例 \"%s\"\n", pInstName );
        return 0;
    }
    pNewCell = Msta_LibFindCell( pApp->pLib, pCellName );
    if ( pNewCell == NULL )
    {
        Msta_Error( "swap_cell：库里找不到单元 \"%s\"\n", pCellName );
        return 0;
    }
    if ( !Msta_TimingSwapCell(pApp->pTime, nInst, pNewCell, fVerify, &Stats) )
        return 0;

    if ( Msta_TimingTimeValid(pApp->pTime->WorstSetupSlack) )
        snprintf( sSetup, sizeof(sSetup), "%.3f ns", pApp->pTime->WorstSetupSlack / 1000.0 );
    else
        snprintf( sSetup, sizeof(sSetup), "N/A" );
    if ( Msta_TimingTimeValid(pApp->pTime->WorstHoldSlack) )
        snprintf( sHold, sizeof(sHold), "%.3f ns", pApp->pTime->WorstHoldSlack / 1000.0 );
    else
        snprintf( sHold, sizeof(sHold), "N/A" );

    fprintf( pApp->pOut,
             "swap_cell: %s -> %s  mode=%s  dirty_nets=%d/%d  checks=%d  "
             "setup_wns=%s  hold_wns=%s  area=%.3f  elapsed=%.3f ms",
             pInstName, pCellName,
             Stats.fIncremental ? "incremental" : "full_fallback",
             Stats.nDirtyNets, Stats.nTotalNets, Stats.nChecksUpdated,
             sSetup, sHold, pApp->pTime->DesignArea, Stats.ElapsedMs );
    if ( fVerify )
        fprintf( pApp->pOut, "  verify_max_diff=%.3e ps", Stats.MaxDiffPs );
    fprintf( pApp->pOut, "\n" );
    return 1;
}

/* help 遍历命令表打印说明，帮助文本与命令分发共用数据。 */
typedef struct {
    const char *pName;
    int (*pFunc)( MstaApp *, int, char ** );
    const char *pHelp;
} MstaCmd;

static MstaCmd s_vCmds[] = {
    { "read_liberty",     Msta_CmdReadLiberty,    "读入一个或多个 Liberty 库文件" },
    { "read_verilog",     Msta_CmdReadVerilog,    "读入门级 Verilog 网表（内部调用 yosys 转 JSON）" },
    { "read_json",        Msta_CmdReadJson,       "读取 yosys write_json 生成的网表" },
    { "current_design",   Msta_CmdCurrentDesign,  "指定顶层并把层次展平成一张扁平网表" },
    { "read_aocv",        Msta_CmdReadAocv,       "读取原生 AOCV 深度表（见 docs/variation.md）" },
    { "report_aocv",      Msta_CmdReportAocv,     "列出 AOCV 深度表和开关状态" },
    { "read_sdc",         Msta_CmdReadSdc,        "读入 SDC 约束（子集，见 msta_sdc.h）" },
    { "set_wire_model",   Msta_CmdSetWireModel,   "切换互连线插件：zero_rc | elmore_rc [-r kohm] [-c ff]" },
    { "set_path_refiner", Msta_CmdSetPathRefiner, "切换路径精修插件：none | pba_retrace" },
    { "swap_cell",        Msta_CmdSwapCell,       "替换实例单元并执行分级增量 STA：swap_cell <inst> <cell> [-verify]" },
    { "report_clocks",    Msta_CmdReportClocks,   "打印已定义的时钟" },
    { "report_checks",    Msta_CmdReportChecks,   "按 slack 打印最差路径：-max_paths N [-setup|-hold] [-tran] [-cap] [-clock_path]" },
    { "report_summary",   Msta_CmdReportSummary,  "只打印 WNS/TNS 汇总" },
    { "report_clock_tree",Msta_CmdReportClockTree,"打印时钟树规模与插入延迟" },
    { "explain_arc",      Msta_CmdExplainArc,     "拆解单条单元弧的 NLDM 双线性查表、derate 与 AOCV 计算" },
    { "report_net",       Msta_CmdReportNet,      "打印单个网络的驱动、扇出、电容、AT/RAT/Slack 与逻辑级数" },
    { "dump_graph",       Msta_CmdDumpGraph,      "导出 Graphviz DOT 时序图：[-o file.dot] [-max_nodes N]" },
    { "report_design",    Msta_CmdReportDesign,   "打印展平后的规模统计" },
    { "report_lib",       Msta_CmdReportLib,      "打印库的统计信息" },
    { "print_cell",       Msta_CmdPrintCell,      "打印一个单元的所有时序弧和 setup/hold 检查" },
    { "help",             Msta_CmdHelp,           "显示命令表" },
    { NULL, NULL, NULL }
};

static int Msta_CmdHelp( MstaApp *pApp, int argc, char **argv )
{
    int i;
    (void)argc; (void)argv;
    fprintf( pApp->pOut, "%-18s %s\n", "命令", "说明" );
    for ( i = 0; s_vCmds[i].pName; i++ )
        fprintf( pApp->pOut, "%-18s %s\n", s_vCmds[i].pName, s_vCmds[i].pHelp );
    fprintf( pApp->pOut, "\n脚本格式：一行一条命令，# 开头是注释，双引号包住带空格的参数。\n" );
    return 1;
}

/* ---------------------------------------------------------------------
   脚本执行：一行一条命令，';' 也可以当分隔符
   --------------------------------------------------------------------- */

int Msta_CmdsRunOne( MstaApp *pApp, int argc, char **argv )
{
    int i;
    if ( argc == 0 )
        return 1;
    for ( i = 0; s_vCmds[i].pName; i++ )
        if ( !strcmp( argv[0], s_vCmds[i].pName ) )
            return s_vCmds[i].pFunc( pApp, argc, argv );
    Msta_Error( "未知命令 \"%s\"（可以试试 help）\n", argv[0] );
    return 0;
}

int Msta_CmdsRun( MstaApp *pApp, const char *pScript )
{
    char *pText = Msta_StrDup( pScript );
    char *pLine, *pNext;
    int fOk = 1, i;

    /* 允许一行里用 ';' 串多条命令（忽略双引号内和 # 注释里的 ';'）。 */
    {
        int fInQuote = 0, fInComment = 0;
        for ( i = 0; pText[i]; i++ )
        {
            char c = pText[i];
            if ( c == '\n' )      { fInComment = 0; fInQuote = 0; }
            else if ( fInComment )  continue;
            else if ( c == '"' )    fInQuote = !fInQuote;
            else if ( !fInQuote && c == '#' ) fInComment = 1;
            else if ( !fInQuote && c == ';' ) pText[i] = '\n';
        }
    }

    for ( pLine = pText; pLine; pLine = pNext )
    {
        char *argv[64];
        int argc;
        pNext = strchr( pLine, '\n' );
        if ( pNext )
            *pNext++ = 0;
        argc = Msta_SplitArgs( pLine, argv, 64 );
        if ( argc == 0 )
            continue;
        if ( !Msta_CmdsRunOne( pApp, argc, argv ) )
        {
            fOk = 0;
            break;
        }
    }
    free( pText );
    return fOk;
}
