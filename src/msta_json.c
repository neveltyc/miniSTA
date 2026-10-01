/**CFile***************************************************************

  FileName    [msta_json.c]

  Synopsis    [极简 JSON 读取器（只读，不写）。]

  把 JSON 文本读成 MJsonValue 树。用户有两个：yosys write_json 产生的网表，
  以及 SDC Tcl 桥接输出的命令记录。语法取 RFC 8259 的子集（不允许注释、
  尾逗号、NaN），数字直接交给 strtod，比标准略宽松；结构不对就报错而不是猜。

***********************************************************************/

#include <ctype.h>
#include <stdio.h>
#include "msta_json.h"
#include "msta_util.h"

typedef struct {
    const char *pCur;
    const char *pEnd;
    const char *pStart;
    char  *pErr;        /* 指向调用方给的缓冲区 */
    int    nErr;
    int    fFailed;
    size_t nDepth;      /* 递归深度保护，防止畸形输入撑爆栈 */
} MJsonReader;

#define MJSON_MAX_DEPTH 200

static MJsonValue *Msta_JsonParseValue( MJsonReader *p );

static void Msta_JsonFail( MJsonReader *p, const char *pMessage )
{
    if ( p->fFailed )
        return;
    p->fFailed = 1;
    if ( p->pErr && p->nErr > 0 )
        snprintf( p->pErr, (size_t)p->nErr, "JSON error near byte %d: %s",
                  (int)(p->pCur - p->pStart), pMessage );
}

static void Msta_JsonSkipSpace( MJsonReader *p )
{
    while ( p->pCur < p->pEnd && (*p->pCur == ' ' || *p->pCur == '\t' ||
                                  *p->pCur == '\n' || *p->pCur == '\r') )
        p->pCur++;
}

static MJsonValue *Msta_JsonNewValue( MJsonKind Kind )
{
    MJsonValue *p = (MJsonValue *)calloc( 1, sizeof(MJsonValue) );
    assert( p );
    p->Kind = Kind;
    return p;
}

/* 读字符串。调用时 *pCur 必须指向开头的引号。
   只做必要的转义还原：\" \\ \/ \b \f \n \r \t \uXXXX（\u 按 UTF-8 编码）。 */
static char *Msta_JsonReadStringRaw( MJsonReader *p )
{
    size_t nCap = 32, nLen = 0;
    char *pOut = (char *)malloc( nCap );
    assert( pOut );
    if ( p->pCur >= p->pEnd || *p->pCur != '\"' )
    {   Msta_JsonFail( p, "expected a string." ); free(pOut); return NULL;  }
    p->pCur++;
    while ( p->pCur < p->pEnd )
    {
        char c = *p->pCur++;
        if ( nLen + 5 >= nCap )
        {
            nCap *= 2;
            pOut = (char *)realloc( pOut, nCap );
            assert( pOut );
        }
        if ( c == '\"' )
        {   pOut[nLen] = 0; return pOut;  }
        if ( c != '\\' )
        {
            /* JSON 规范要求控制字符必须转义，裸控制字符直接判为非法输入。 */
            if ( (unsigned char)c < 0x20 )
            {   Msta_JsonFail( p, "unescaped control character in a string." );
                free(pOut); return NULL;  }
            pOut[nLen++] = c;
            continue;
        }
        if ( p->pCur >= p->pEnd )
            break;
        c = *p->pCur++;
        switch ( c )
        {
            case '\"': pOut[nLen++] = '\"';  break;
            case '\\': pOut[nLen++] = '\\';  break;
            case '/':  pOut[nLen++] = '/';   break;
            case 'b':  pOut[nLen++] = '\b';  break;
            case 'f':  pOut[nLen++] = '\f';  break;
            case 'n':  pOut[nLen++] = '\n';  break;
            case 'r':  pOut[nLen++] = '\r';  break;
            case 't':  pOut[nLen++] = '\t';  break;
            case 'u':
            {
                /* \uXXXX：yosys 只吐 ASCII，这里只需处理 <0x80 的情况；
                   更大的值按 UTF-8 编码写出去，避免名字被截断。 */
                unsigned nCode = 0;
                int i;
                for ( i = 0; i < 4 && p->pCur < p->pEnd; i++ )
                {
                    char h = *p->pCur++;
                    nCode *= 16;
                    if      ( h >= '0' && h <= '9' ) nCode += (unsigned)(h - '0');
                    else if ( h >= 'a' && h <= 'f' ) nCode += (unsigned)(h - 'a' + 10);
                    else if ( h >= 'A' && h <= 'F' ) nCode += (unsigned)(h - 'A' + 10);
                    else { Msta_JsonFail( p, "bad \\u escape." ); free(pOut); return NULL; }
                }
                if ( nCode < 0x80 )
                    pOut[nLen++] = (char)nCode;
                else if ( nCode < 0x800 )
                {
                    pOut[nLen++] = (char)(0xC0 | (nCode >> 6));
                    pOut[nLen++] = (char)(0x80 | (nCode & 0x3F));
                }
                else
                {
                    pOut[nLen++] = (char)(0xE0 | (nCode >> 12));
                    pOut[nLen++] = (char)(0x80 | ((nCode >> 6) & 0x3F));
                    pOut[nLen++] = (char)(0x80 | (nCode & 0x3F));
                }
                break;
            }
            default:
                Msta_JsonFail( p, "unknown escape." );
                free( pOut );
                return NULL;
        }
    }
    Msta_JsonFail( p, "unterminated string." );
    free( pOut );
    return NULL;
}

static MJsonValue *Msta_JsonReadNumber( MJsonReader *p )
{
    char *pEndPtr;
    double Value = strtod( p->pCur, &pEndPtr );
    MJsonValue *pNode;
    if ( pEndPtr == p->pCur )
    {   Msta_JsonFail( p, "bad number." ); return NULL;  }
    p->pCur = pEndPtr;
    pNode = Msta_JsonNewValue( MJSON_NUMBER );
    pNode->Num = Value;
    return pNode;
}

static void Msta_JsonPushItem( MJsonValue *pParent, const char *pKey, MJsonValue *pChild )
{
    pParent->ppItems = (MJsonValue **)realloc( pParent->ppItems,
                            (size_t)(pParent->nItems + 1) * sizeof(MJsonValue *) );
    assert( pParent->ppItems );
    pParent->ppItems[pParent->nItems] = pChild;
    if ( pKey )
    {
        pParent->pKeys = (char **)realloc( pParent->pKeys,
                            (size_t)(pParent->nItems + 1) * sizeof(char *) );
        assert( pParent->pKeys );
        pParent->pKeys[pParent->nItems] = (char *)pKey;
    }
    pParent->nItems++;
}

static MJsonValue *Msta_JsonParseValue( MJsonReader *p )
{
    MJsonValue *pNode;
    char c;

    if ( ++p->nDepth > MJSON_MAX_DEPTH )
    {   Msta_JsonFail( p, "nesting too deep." ); return NULL;  }
    Msta_JsonSkipSpace( p );
    if ( p->pCur >= p->pEnd )
    {   Msta_JsonFail( p, "unexpected end of file." ); p->nDepth--; return NULL;  }
    c = *p->pCur;

    if ( c == '{' )
    {
        p->pCur++;
        pNode = Msta_JsonNewValue( MJSON_OBJECT );
        Msta_JsonSkipSpace( p );
        if ( p->pCur < p->pEnd && *p->pCur == '}' )
        {   p->pCur++; p->nDepth--; return pNode;  }
        for (;;)
        {
            char *pKey;
            MJsonValue *pChild;
            Msta_JsonSkipSpace( p );
            pKey = Msta_JsonReadStringRaw( p );
            if ( pKey == NULL )
                goto fail;
            Msta_JsonSkipSpace( p );
            if ( p->pCur >= p->pEnd || *p->pCur != ':' )
            {   Msta_JsonFail( p, "expected \":\" in an object." ); free(pKey); goto fail;  }
            p->pCur++;
            pChild = Msta_JsonParseValue( p );
            if ( pChild == NULL )
            {   free( pKey ); goto fail;  }
            Msta_JsonPushItem( pNode, pKey, pChild );
            Msta_JsonSkipSpace( p );
            if ( p->pCur < p->pEnd && *p->pCur == ',' )
            {   p->pCur++; continue;  }
            if ( p->pCur < p->pEnd && *p->pCur == '}' )
            {   p->pCur++; p->nDepth--; return pNode;  }
            Msta_JsonFail( p, "expected \",\" or \"}\" in an object." );
            goto fail;
        }
    }
    if ( c == '[' )
    {
        p->pCur++;
        pNode = Msta_JsonNewValue( MJSON_ARRAY );
        Msta_JsonSkipSpace( p );
        if ( p->pCur < p->pEnd && *p->pCur == ']' )
        {   p->pCur++; p->nDepth--; return pNode;  }
        for (;;)
        {
            MJsonValue *pChild = Msta_JsonParseValue( p );
            if ( pChild == NULL )
                goto fail;
            Msta_JsonPushItem( pNode, NULL, pChild );
            Msta_JsonSkipSpace( p );
            if ( p->pCur < p->pEnd && *p->pCur == ',' )
            {   p->pCur++; continue;  }
            if ( p->pCur < p->pEnd && *p->pCur == ']' )
            {   p->pCur++; p->nDepth--; return pNode;  }
            Msta_JsonFail( p, "expected \",\" or \"]\" in an array." );
            goto fail;
        }
    }
    if ( c == '\"' )
    {
        char *pStr = Msta_JsonReadStringRaw( p );
        if ( pStr == NULL )
            return NULL;
        pNode = Msta_JsonNewValue( MJSON_STRING );
        pNode->pStr = pStr;
        p->nDepth--;
        return pNode;
    }
    if ( !strncmp( p->pCur, "true", 4 ) )
    {   p->pCur += 4; p->nDepth--; return Msta_JsonNewValue( MJSON_TRUE );   }
    if ( !strncmp( p->pCur, "false", 5 ) )
    {   p->pCur += 5; p->nDepth--; return Msta_JsonNewValue( MJSON_FALSE );  }
    if ( !strncmp( p->pCur, "null", 4 ) )
    {   p->pCur += 4; p->nDepth--; return Msta_JsonNewValue( MJSON_NULL );   }
    if ( c == '-' || isdigit((unsigned char)c) )
    {
        pNode = Msta_JsonReadNumber( p );
        p->nDepth--;
        return pNode;
    }
    Msta_JsonFail( p, "unexpected token." );
    return NULL;

fail:
    Msta_JsonFree( pNode );
    p->nDepth--;
    return NULL;
}

MJsonValue *Msta_JsonParse( const char *pText, size_t nLength, char *pErrBuf, int nErrBuf )
{
    MJsonReader r;
    MJsonValue *pRoot;
    memset( &r, 0, sizeof(r) );
    r.pCur   = pText;
    r.pEnd   = pText + nLength;
    r.pStart = pText;
    r.pErr   = pErrBuf;
    r.nErr   = nErrBuf;
    if ( pErrBuf && nErrBuf > 0 )
        pErrBuf[0] = 0;

    pRoot = Msta_JsonParseValue( &r );
    if ( pRoot == NULL )
        return NULL;
    Msta_JsonSkipSpace( &r );
    if ( r.pCur != r.pEnd )
    {
        Msta_JsonFail( &r, "trailing characters after the JSON value." );
        Msta_JsonFree( pRoot );
        return NULL;
    }
    return pRoot;
}

MJsonValue *Msta_JsonParseFile( const char *pFileName, char *pErrBuf, int nErrBuf )
{
    size_t nSize;
    char *pText = Msta_FileReadAll( pFileName, &nSize );
    MJsonValue *pRoot;
    if ( pText == NULL )
        return NULL;
    pRoot = Msta_JsonParse( pText, nSize, pErrBuf, nErrBuf );
    free( pText );
    return pRoot;
}

void Msta_JsonFree( MJsonValue *p )
{
    int i;
    if ( p == NULL )
        return;
    for ( i = 0; i < p->nItems; i++ )
    {
        Msta_JsonFree( p->ppItems[i] );
        if ( p->pKeys )
            free( p->pKeys[i] );
    }
    free( p->ppItems );
    free( p->pKeys );          /* key 字符串与 pKeys 同生命周期，一起释放 */
    free( p->pStr );
    free( p );
}

/* ---------------------------------------------------------------------
   查询助手
   --------------------------------------------------------------------- */

MJsonValue *Msta_JsonGet( MJsonValue *pObj, const char *pKey )
{
    int i;
    if ( pObj == NULL || pObj->Kind != MJSON_OBJECT )
        return NULL;
    for ( i = 0; i < pObj->nItems; i++ )
        if ( pObj->pKeys && !strcmp( pObj->pKeys[i], pKey ) )
            return pObj->ppItems[i];
    return NULL;
}

const char *Msta_JsonStr( MJsonValue *pObj, const char *pKey, const char *pDefault )
{
    MJsonValue *p = Msta_JsonGet( pObj, pKey );
    return ( p && p->Kind == MJSON_STRING ) ? p->pStr : pDefault;
}


int Msta_JsonIsArray( MJsonValue *pValue )
{   return pValue && pValue->Kind == MJSON_ARRAY;  }

int Msta_JsonCount( MJsonValue *pValue )
{   return ( pValue && (pValue->Kind == MJSON_ARRAY || pValue->Kind == MJSON_OBJECT) )
        ? pValue->nItems : 0;  }

const char *Msta_JsonKeyAt( MJsonValue *pObj, int i )
{
    if ( pObj == NULL || pObj->Kind != MJSON_OBJECT || i < 0 || i >= pObj->nItems )
        return NULL;
    return pObj->pKeys[i];
}

MJsonValue *Msta_JsonAt( MJsonValue *pValue, int i )
{
    if ( pValue == NULL || i < 0 || i >= pValue->nItems )
        return NULL;
    return pValue->ppItems[i];
}
