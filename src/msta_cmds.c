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
        pApp->pTime = Msta_TimingStart( pApp->pNet, pApp->pLib, pApp->pSdc );
    return pApp->pTime;
}

static int Msta_CmdReportChecks( MstaApp *pApp, int argc, char **argv )
{
    MstaTiming *pTime = Msta_AppTiming( pApp );
    int nMaxPaths = 1;
    int fSetup = 1;
    int i;
    if ( pApp->pNet->vNets.nSize == 0 )
    {
        Msta_Error( "report_checks：请先展平设计（current_design）\n" );
        return 0;
    }
    for ( i = 1; i < argc; i++ )
    {
        if ( !strcmp(argv[i], "-hold") )   fSetup = 0;
        else if ( !strcmp(argv[i], "-setup") ) fSetup = 1;
        else if ( !strcmp(argv[i], "-max_paths") && i + 1 < argc ) nMaxPaths = atoi( argv[++i] );
        else Msta_WarnOnce( "report_checks：选项 \"%s\" 不支持，已忽略", argv[i] );
    }
    if ( nMaxPaths < 1 )
    {
        Msta_WarnOnce( "report_checks：-max_paths 必须大于 0；按 1 处理" );
        nMaxPaths = 1;
    }
    if ( !Msta_TimingAnalyze( pTime, !pApp->fQuiet ) )
        return 0;
    Msta_ReportChecks( pTime, pApp->pOut, nMaxPaths, fSetup );
    Msta_ReportSummary( pTime, pApp->pOut );
    return 1;
}

static int Msta_CmdReportSummary( MstaApp *pApp, int argc, char **argv )
{
    MstaTiming *pTime = Msta_AppTiming( pApp );
    (void)argc; (void)argv;
    if ( !Msta_TimingAnalyze( pTime, !pApp->fQuiet ) )
        return 0;
    Msta_ReportSummary( pTime, pApp->pOut );
    return 1;
}

static int Msta_CmdReportClockTree( MstaApp *pApp, int argc, char **argv )
{
    MstaTiming *pTime = Msta_AppTiming( pApp );
    (void)argc; (void)argv;
    if ( !Msta_TimingAnalyze( pTime, !pApp->fQuiet ) )
        return 0;
    Msta_ReportClockTree( pTime, pApp->pOut );
    return 1;
}

/* 命令表：名字 / 处理函数 / 一句话说明。help 命令直接打印这张表，
   所以新增命令时只要在这里加一行，文档就自动同步。 */
typedef struct {
    const char *pName;
    int (*pFunc)( MstaApp *, int, char ** );
    const char *pHelp;
} MstaCmd;

static MstaCmd s_vCmds[] = {
    { "read_liberty",   Msta_CmdReadLiberty,  "读入一个或多个 Liberty 库文件" },
    { "read_verilog",   Msta_CmdReadVerilog,  "读入门级 Verilog 网表（内部调用 yosys 转 JSON）" },
    { "read_json",      Msta_CmdReadJson,     "读取 yosys write_json 生成的网表" },
    { "current_design", Msta_CmdCurrentDesign,"指定顶层并把层次展平成一张扁平网表" },
    { "read_sdc",       Msta_CmdReadSdc,      "读入 SDC 约束（子集，见 msta_sdc.h）" },
    { "report_clocks",  Msta_CmdReportClocks, "打印已定义的时钟" },
    { "report_checks",  Msta_CmdReportChecks, "按 slack 打印最多 N 个 endpoint 最差路径：-max_paths N [-setup|-hold]" },
    { "report_summary", Msta_CmdReportSummary,"只打印 WNS/TNS 汇总" },
    { "report_clock_tree", Msta_CmdReportClockTree, "打印时钟树规模与插入延迟" },
    { "report_design",  Msta_CmdReportDesign, "打印展平后的规模统计" },
    { "report_lib",     Msta_CmdReportLib,    "打印库的统计信息" },
    { "print_cell",     Msta_CmdPrintCell,    "打印一个单元的所有时序弧和 setup/hold 检查" },
    { "help",           Msta_CmdHelp,         "显示命令表" },
    { NULL, NULL, NULL }
};

static int Msta_CmdHelp( MstaApp *pApp, int argc, char **argv )
{
    int i;
    (void)argc; (void)argv;
    fprintf( pApp->pOut, "%-14s %s\n", "命令", "说明" );
    for ( i = 0; s_vCmds[i].pName; i++ )
        fprintf( pApp->pOut, "%-14s %s\n", s_vCmds[i].pName, s_vCmds[i].pHelp );
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

    /* 允许一行里用 ';' 串多条命令（方便 -c "a; b" 这种写法）。 */
    for ( i = 0; pText[i]; i++ )
        if ( pText[i] == ';' )
            pText[i] = '\n';

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
