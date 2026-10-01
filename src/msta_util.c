/**CFile***************************************************************

  FileName    [msta_util.c]

  Synopsis    [参数切分 / 日志 / 字符串 / 文件读入 / 名字表 / 整数哈希表。]

***********************************************************************/

#include <stdarg.h>
#include "msta_util.h"

/* ---------------------------------------------------------------------
   参数切分（dofile 用）
   --------------------------------------------------------------------- */

int Msta_SplitArgs( char *pLine, char **argv, int nMaxArgs )
{
    int argc = 0;
    char *p = pLine;
    while ( *p && argc < nMaxArgs )
    {
        while ( *p == ' ' || *p == '\t' || *p == '\r' ) p++;
        if ( *p == 0 || *p == '#' )
            break;
        if ( *p == '"' )
        {
            char *pStart = ++p;
            while ( *p && *p != '"' ) p++;
            if ( *p == '"' ) *p++ = 0;
            argv[argc++] = pStart;
        }
        else
        {
            char *pStart = p;
            while ( *p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '#' ) p++;
            if ( *p ) *p++ = 0;
            argv[argc++] = pStart;
        }
    }
    return argc;
}

/* ---------------------------------------------------------------------
   日志
   --------------------------------------------------------------------- */

void Msta_Log( MstaLogLevel Level, const char *pFormat, ... )
{
    static const char *s_pPrefix[] = { "", "** Warning: ", "** Error: " };
    va_list pArgs;
    fprintf( stderr, "%s", s_pPrefix[Level] );
    va_start( pArgs, pFormat );
    vfprintf( stderr, pFormat, pArgs );
    va_end( pArgs );
}

/* 分析阶段收集的告警：同一句只留一份，避免几万条实例刷屏。 */
MstaMsgArray g_vMstaWarnings;

void Msta_WarnOnce( const char *pFormat, ... )
{
    char sMsg[512];
    va_list pArgs;
    int i;
    va_start( pArgs, pFormat );
    vsnprintf( sMsg, sizeof(sMsg), pFormat, pArgs );
    va_end( pArgs );
    for ( i = 0; i < g_vMstaWarnings.nSize; i++ )
        if ( !strcmp( g_vMstaWarnings.pData[i], sMsg ) )
            return;
    *MstaMsgArrayAppend( &g_vMstaWarnings ) = Msta_StrDup( sMsg );
    Msta_Warn( "%s\n", sMsg );
}

/* ---------------------------------------------------------------------
   字符串
   --------------------------------------------------------------------- */

char *Msta_StrDup( const char *pStr )
{
    size_t n = strlen( pStr ) + 1;
    char *pNew = (char *)malloc( n );
    assert( pNew );
    memcpy( pNew, pStr, n );
    return pNew;
}

/* ---------------------------------------------------------------------
   文件读入
   --------------------------------------------------------------------- */

char *Msta_FileReadAll( const char *pFileName, size_t *pnSize )
{
    FILE  *pFile = fopen( pFileName, "rb" );
    long   nSize;
    char  *pContents;
    size_t nRead;

    if ( pFile == NULL )
    {
        Msta_Error( "cannot open \"%s\".\n", pFileName );
        return NULL;
    }
    fseek( pFile, 0, SEEK_END );
    nSize = ftell( pFile );
    rewind( pFile );
    if ( nSize <= 0 )
    {   /* 空文件不是错误，但也没有内容可解析。 */
        fclose( pFile );
        *pnSize = 0;
        return Msta_StrDup( "" );
    }
    pContents = (char *)malloc( (size_t)nSize + 1 );
    assert( pContents );
    nRead = fread( pContents, 1, (size_t)nSize, pFile );
    fclose( pFile );
    pContents[nRead] = 0;
    *pnSize = nRead;
    return pContents;
}

/* ---------------------------------------------------------------------
   名字表：字符串 <-> id
   --------------------------------------------------------------------- */

/* djb2 变体。哈希函数的好坏只影响性能（桶里链表长短），不影响正确性。 */
static unsigned Msta_HashString( const char *pStr, int nBuckets )
{
    unsigned nHash = 5381;
    while ( *pStr )
        nHash = ((nHash << 5) + nHash) + (unsigned)*pStr++;
    return nHash % (unsigned)nBuckets;
}

MstaNameTable *Msta_NameTableStart( void )
{
    MstaNameTable *p = (MstaNameTable *)malloc( sizeof(MstaNameTable) );
    assert( p );
    p->nBuckets  = 1 << 16;
    p->ppBuckets = (MstaNameEntry **)calloc( (size_t)p->nBuckets, sizeof(MstaNameEntry *) );
    assert( p->ppBuckets );
    p->nNamesCap = 4096;
    p->pNames    = (const char **)malloc( (size_t)p->nNamesCap * sizeof(char *) );
    assert( p->pNames );
    p->nNames    = 0;
    return p;
}

MstaId Msta_NameTableId( MstaNameTable *p, const char *pName )
{
    unsigned b = Msta_HashString( pName, p->nBuckets );
    MstaNameEntry *pEntry;
    for ( pEntry = p->ppBuckets[b]; pEntry; pEntry = pEntry->pNext )
        if ( !strcmp( pEntry->pName, pName ) )
            return pEntry->Id;

    /* 没找到：新建一个 id，同时挂进哈希桶和 id->名字 数组。 */
    if ( p->nNames == p->nNamesCap )
    {
        p->nNamesCap *= 2;
        p->pNames = (const char **)realloc( (void *)p->pNames, (size_t)p->nNamesCap * sizeof(char *) );
        assert( p->pNames );
    }
    pEntry = (MstaNameEntry *)malloc( sizeof(MstaNameEntry) );
    assert( pEntry );
    pEntry->pName = Msta_StrDup( pName );
    pEntry->Id    = p->nNames++;
    pEntry->pNext = p->ppBuckets[b];
    p->ppBuckets[b] = pEntry;
    p->pNames[pEntry->Id] = pEntry->pName;
    return pEntry->Id;
}

const char *Msta_NameTableName( MstaNameTable *p, MstaId id )
{
    if ( id == MSTA_NO_ID )   return "?";
    if ( id < 0 || id >= p->nNames ) return "?";
    return p->pNames[id];
}

/* ---------------------------------------------------------------------
   整数哈希表（开放寻址 + 线性探测）
   --------------------------------------------------------------------- */

#define MSTA_INTMAP_EMPTY (-1)

void Msta_IntMapInit( MstaIntMap *p, int nCapWanted )
{
    int i, nCap = 16;
    while ( nCap < nCapWanted )
        nCap *= 2;
    p->pKeys   = (int *)malloc( (size_t)nCap * sizeof(int) );
    p->pValues = (int *)malloc( (size_t)nCap * sizeof(int) );
    assert( p->pKeys && p->pValues );
    for ( i = 0; i < nCap; i++ )
        p->pKeys[i] = MSTA_INTMAP_EMPTY;
    p->nSize = 0;
    p->nCap  = nCap;
}

void Msta_IntMapFree( MstaIntMap *p )
{
    free( p->pKeys );   p->pKeys = NULL;
    free( p->pValues ); p->pValues = NULL;
    p->nSize = p->nCap = 0;
}

static void Msta_IntMapGrow( MstaIntMap *p )
{
    int i, nOldCap = p->nCap;
    int *pOldKeys = p->pKeys, *pOldValues = p->pValues;
    Msta_IntMapInit( p, nOldCap * 2 );
    for ( i = 0; i < nOldCap; i++ )
        if ( pOldKeys[i] != MSTA_INTMAP_EMPTY )
            Msta_IntMapSet( p, pOldKeys[i], pOldValues[i] );
    free( pOldKeys );
    free( pOldValues );
}

int Msta_IntMapGet( MstaIntMap *p, int Key, int DefaultValue )
{
    unsigned nProbe = (unsigned)Key & (unsigned)(p->nCap - 1);
    if ( p->nSize == 0 )
        return DefaultValue;
    while ( p->pKeys[nProbe] != MSTA_INTMAP_EMPTY )
    {
        if ( p->pKeys[nProbe] == Key )
            return p->pValues[nProbe];
        nProbe = (nProbe + 1) & (unsigned)(p->nCap - 1);
    }
    return DefaultValue;
}

void Msta_IntMapSet( MstaIntMap *p, int Key, int Value )
{
    unsigned nProbe = (unsigned)Key & (unsigned)(p->nCap - 1);
    assert( Key != MSTA_INTMAP_EMPTY );
    while ( p->pKeys[nProbe] != MSTA_INTMAP_EMPTY )
    {
        if ( p->pKeys[nProbe] == Key )
        {   p->pValues[nProbe] = Value; return;  }
        nProbe = (nProbe + 1) & (unsigned)(p->nCap - 1);
    }
    p->pKeys[nProbe]   = Key;
    p->pValues[nProbe] = Value;
    p->nSize++;
    /* 装到一半就扩容：线性探测的期望探测长度随负载因子急剧上升。 */
    if ( p->nSize * 2 > p->nCap )
        Msta_IntMapGrow( p );
}

/* 全局单例。用函数包一层是为了避免不同 .c 之间的初始化顺序问题。 */
static MstaNameTable *s_pNameTable = NULL;

MstaNameTable *Msta_Names( void )
{
    if ( s_pNameTable == NULL )
        s_pNameTable = Msta_NameTableStart();
    return s_pNameTable;
}
