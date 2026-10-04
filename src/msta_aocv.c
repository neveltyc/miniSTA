/**CFile***************************************************************

  FileName    [msta_aocv.c]

  Synopsis    [原生 AOCV 深度表：读入校验、对象匹配、插值与报告。]

  文件的每行是一条采样点。全部行验证成功后才替换旧表；读入、匹配和
  插值均不改变时序图，完整路径的结构深度由时序引擎计算。

***********************************************************************/

#include "msta_sdc.h"
#include "msta_net.h"
#include "msta_util.h"
#include <fnmatch.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>

#define MSTA_AOCV_MAX_DEPTH 1000000

typedef struct {
    char   Kind;
    MstaId Pattern;
    int    fClock, fRise, fMax;
    int    nDepth;
    double Factor;
} MstaAocvRow;

/* 返回 1=读到一行，0=文件结束，-1=I/O 错误、NUL 或超过缓冲区。 */
static int AocvReadLine( FILE *pIn, char *pLine, size_t nCapacity )
{
    int Char = fgetc( pIn );
    size_t nLength = 0;
    pLine[0] = 0;
    if ( Char == EOF )
        return ferror( pIn ) ? -1 : 0;
    do
    {
        if ( Char == 0 || nLength + 1 >= nCapacity )
        {
            pLine[nLength] = 0;
            return -1;
        }
        pLine[nLength++] = (char)Char;
        if ( Char == '\n' )
            break;
        Char = fgetc( pIn );
    } while ( Char != EOF );
    pLine[nLength] = 0;
    return ferror( pIn ) ? -1 : 1;
}

/* 去掉注释和行首空白；空行交给调用方跳过。 */
static char *AocvRowText( char *pLine )
{
    char *pComment = strchr( pLine, '#' );
    if ( pComment )
        *pComment = 0;
    while ( isspace( (unsigned char)*pLine ) )
        pLine++;
    return pLine;
}

/* 七列：scope pattern clock|data rise|fall early|late depth factor。 */
static int AocvParseRow( const char *pText, MstaAocvRow *pRow )
{
    char Scope[32], Pattern[256], Path[32], Edge[32], Corner[32];
    char DepthText[64], FactorText[64], Extra[2];
    char *pEnd;
    long nDepth;
    double Factor;

    if ( sscanf( pText, "%31s %255s %31s %31s %31s %63s %63s %1s",
                 Scope, Pattern, Path, Edge, Corner, DepthText, FactorText, Extra ) != 7 )
        return 0;

    errno = 0;
    nDepth = strtol( DepthText, &pEnd, 10 );
    if ( errno || *pEnd || nDepth < 1 || nDepth > MSTA_AOCV_MAX_DEPTH )
        return 0;
    errno = 0;
    Factor = strtod( FactorText, &pEnd );
    if ( errno || *pEnd || !isfinite( Factor ) )
        return 0;

    if ( strcmp( Scope, "design" ) && strcmp( Scope, "cell" ) && strcmp( Scope, "instance" ) )
        return 0;
    if ( !strcmp( Scope, "design" ) && strcmp( Pattern, "*" ) )
        return 0;
    if ( strcmp( Path, "clock" ) && strcmp( Path, "data" ) )
        return 0;
    if ( strcmp( Edge, "rise" ) && strcmp( Edge, "fall" ) )
        return 0;
    if ( strcmp( Corner, "early" ) && strcmp( Corner, "late" ) )
        return 0;
    if ( Factor <= 0 || ( !strcmp( Corner, "early" ) ? Factor > 1 : Factor < 1 ) )
        return 0;

    pRow->Kind = !strcmp( Scope, "design" ) ? 'G' : !strcmp( Scope, "instance" ) ? 'I' : 'L';
    pRow->Pattern = Msta_NameId( Pattern );
    pRow->fClock = !strcmp( Path, "clock" );
    pRow->fRise = !strcmp( Edge, "rise" );
    pRow->fMax = !strcmp( Corner, "late" );
    pRow->nDepth = (int)nDepth;
    pRow->Factor = Factor;
    return 1;
}

/* 相同 scope、对象模式和时序维度的行归入同一张表，保留首次出现的表顺序。 */
static MstaAocvTable *AocvFindOrCreateTable( MstaAocvTableArray *pTables, const MstaAocvRow *pRow )
{
    MstaAocvTable *pTable;
    int i;
    for ( i = 0; i < pTables->nSize; i++ )
    {
        pTable = MstaAocvTableArrayAt( pTables, i );
        if ( pTable->Kind == pRow->Kind && pTable->Pattern == pRow->Pattern &&
             pTable->fClock == pRow->fClock && pTable->fRise == pRow->fRise && pTable->fMax == pRow->fMax )
            return pTable;
    }
    pTable = MstaAocvTableArrayAppend( pTables );
    pTable->Kind = pRow->Kind;
    pTable->Pattern = pRow->Pattern;
    pTable->fClock = pRow->fClock;
    pTable->fRise = pRow->fRise;
    pTable->fMax = pRow->fMax;
    return pTable;
}

/* 深度严格递增；单调系数保证最短结构上下文可用于保守 GBA。 */
static int AocvAppendRow( MstaAocvTable *pTable, const MstaAocvRow *pRow )
{
    int nPoints = pTable->nPoints;
    if ( nPoints == MSTA_AOCV_MAX_POINTS )
        return 0;
    if ( nPoints > 0 )
    {
        if ( pRow->nDepth <= pTable->Depth[nPoints - 1] )
            return 0;
        if ( pRow->fMax ? pRow->Factor > pTable->Factor[nPoints - 1]
                       : pRow->Factor < pTable->Factor[nPoints - 1] )
            return 0;
    }
    pTable->Depth[nPoints] = pRow->nDepth;
    pTable->Factor[nPoints] = pRow->Factor;
    pTable->nPoints++;
    return 1;
}

/* 读入临时表，整份文件验证成功后再替换模型。 */
int Msta_AocvRead( MstaSdc *p, const char *pFile, int fVerbose )
{
    FILE *pIn = fopen( pFile, "r" );
    MstaAocvTableArray vPending;
    char Line[2048];
    int nLine = 0, nRows = 0, fOk = 1, ReadStatus;

    MstaAocvTableArrayInit( &vPending );
    if ( !pIn )
    {
        Msta_Error( "read_aocv：无法读取 %s\n", pFile );
        return 0;
    }
    while ( ( ReadStatus = AocvReadLine( pIn, Line, sizeof(Line) ) ) != 0 )
    {
        MstaAocvRow Row;
        MstaAocvTable *pTable;
        char *pText;
        nLine++;
        if ( ReadStatus < 0 )
        {
            fOk = 0;
            break;
        }
        pText = AocvRowText( Line );
        if ( !*pText )
            continue;
        if ( !AocvParseRow( pText, &Row ) )
        {
            fOk = 0;
            break;
        }
        pTable = AocvFindOrCreateTable( &vPending, &Row );
        if ( !AocvAppendRow( pTable, &Row ) )
        {
            fOk = 0;
            break;
        }
        nRows++;
    }
    if ( ferror( pIn ) )
        fOk = 0;
    fclose( pIn );
    if ( !fOk || !nRows )
    {
        Msta_Error( "read_aocv：%s:%d 无效表格；需正整数递增 depth、有限系数，"
                    "early <=1 单调递增，late >=1 单调递减；原表保留\n", pFile, nLine );
        MstaAocvTableArrayFree( &vPending );
        return 0;
    }
    MstaAocvTableArrayFree( &p->vAocv );
    p->vAocv = vPending;
    p->fAocv = 1;
    if ( fVerbose )
        printf( "AOCV：%d 张深度表（%d 行）\n", vPending.nSize, nRows );
    return 1;
}

/* 中间深度线性插值，范围外沿用边界采样点。 */
static double AocvLookup( const MstaAocvTable *pTable, int nDepth )
{
    int i;
    if ( nDepth <= pTable->Depth[0] )
        return pTable->Factor[0];
    for ( i = 1; i < pTable->nPoints; i++ )
        if ( nDepth <= pTable->Depth[i] )
        {
            double Fraction = (double)( nDepth - pTable->Depth[i - 1] )
                            / ( pTable->Depth[i] - pTable->Depth[i - 1] );
            return pTable->Factor[i - 1] + Fraction * ( pTable->Factor[i] - pTable->Factor[i - 1] );
        }
    return pTable->Factor[pTable->nPoints - 1];
}

/* 对象作用域的覆盖优先级：实例 > 库单元 > 设计。 */
static int AocvScopePriority( char Kind )
{
    return Kind == 'I' ? 2 : Kind == 'L' ? 1 : 0;
}

/* 设计表不筛选对象；实例和库单元表按对应名字做 glob 匹配。 */
static int AocvTableMatchesObject( const MstaAocvTable *pTable, const MstaInst *pInst )
{
    const char *pName = pTable->Kind == 'I' ? Msta_NameStr( pInst->Name )
                                          : Msta_NameStr( pInst->pCell->Name );
    return pTable->Kind == 'G' || !fnmatch( Msta_NameStr( pTable->Pattern ), pName, 0 );
}

/* 选择匹配维度中优先级最高的表；同优先级按表顺序后者覆盖前者。 */
double Msta_AocvFactor( MstaSdc *p, MstaInst *pInst, int fClock, int fRise, int fMax, int nDepth )
{
    const MstaAocvTable *pBest = NULL;
    int i, nBestPriority = -1;
    if ( !p->fAocv || !pInst )
        return 1;
    for ( i = 0; i < p->vAocv.nSize; i++ )
    {
        const MstaAocvTable *pTable = MstaAocvTableArrayAt( &p->vAocv, i );
        int nPriority = AocvScopePriority( pTable->Kind );
        if ( pTable->fClock != fClock || pTable->fRise != fRise || pTable->fMax != fMax ||
             nPriority < nBestPriority )
            continue;
        if ( !AocvTableMatchesObject( pTable, pInst ) )
            continue;
        pBest = pTable;
        nBestPriority = nPriority;
    }
    return pBest ? AocvLookup( pBest, nDepth ) : 1;
}

/* 列出表的匹配维度与采样点，不参与时序计算。 */
void Msta_AocvReport( MstaSdc *p, FILE *pOut )
{
    int i, j;
    fprintf( pOut, "AOCV depth GBA : %s, %d tables\n", p->fAocv ? "enabled" : "disabled", p->vAocv.nSize );
    for ( i = 0; i < p->vAocv.nSize; i++ )
    {
        MstaAocvTable *pTable = MstaAocvTableArrayAt( &p->vAocv, i );
        const char *pScope = pTable->Kind == 'G' ? "design" : pTable->Kind == 'I' ? "instance" : "cell";
        fprintf( pOut, "%s %s %s %s %s:", pScope, Msta_NameStr( pTable->Pattern ),
                 pTable->fClock ? "clock" : "data", pTable->fRise ? "rise" : "fall", pTable->fMax ? "late" : "early" );
        for ( j = 0; j < pTable->nPoints; j++ )
            fprintf( pOut, " %d:%.6g", pTable->Depth[j], pTable->Factor[j] );
        fprintf( pOut, "\n" );
    }
}
