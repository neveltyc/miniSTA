/**CFile***************************************************************

  FileName    [msta_main.c]

  Synopsis    [命令行入口：读一个 dofile（或 -c "命令串"），逐条执行。]

  命令表由 msta_cmds.c 提供；本文件处理命令行参数和 dofile 输入。

***********************************************************************/

#include "msta_types.h"
#include "msta_util.h"
#include "msta_cmds.h"
#include "msta_sdc.h"

int main( int argc, char **argv )
{
    MstaApp *pApp;
    const char *pScript = NULL;
    const char *pInline = NULL;
    int i;
    int fOk = 1;

    for ( i = 1; i < argc; i++ )
    {
        if ( !strcmp(argv[i], "-c") && i + 1 < argc )
            pInline = argv[++i];
        else if ( !strcmp(argv[i], "-h") || !strcmp(argv[i], "--help") )
        {
            printf( "用法：msta <dofile> | msta -c \"命令1; 命令2\"\n" );
            printf( "      msta -q <dofile>        安静模式\n" );
            printf( "      msta -o out.txt <file>  把报告写到文件\n" );
            return 0;
        }
        else if ( !strcmp(argv[i], "-q") )
            Msta_CmdsSetQuiet( 1 );
        else if ( !strcmp(argv[i], "-o") && i + 1 < argc )
            Msta_CmdsSetOutput( argv[++i] );
        else
            pScript = argv[i];
    }

    if ( pInline == NULL && pScript == NULL )
    {
        Msta_Error( "没有要执行的命令；可以试试 msta -c \"help\"\n" );
        return 1;
    }

    pApp = Msta_AppStart();
    Msta_SdcSetBridgePath( argv[0] );
    if ( pScript )
    {
        size_t nSize;
        char *pText;
        Msta_CmdsSetScriptFile( pScript );     /* 相对路径找不到时按脚本目录兜底 */
        pText = Msta_FileReadAll( pScript, &nSize );
        if ( pText == NULL )
            return 1;
        Msta_Info( "-- %s\n", pScript );
        fOk = Msta_CmdsRun( pApp, pText );
        free( pText );
    }
    if ( fOk && pInline )
        fOk = Msta_CmdsRun( pApp, pInline );
    Msta_AppFree( pApp );
    return fOk ? 0 : 1;
}
