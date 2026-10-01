/**CFile***************************************************************

  FileName    [abc_scl_liberty_tree.inc]

  SystemName  [msta]

  Synopsis    [Liberty 语法树解析器 —— 抄自 berkeley-abc/abc 的 src/map/scl/sclLiberty.c]

  ---------------------------------------------------------------------
  来历与改动
  ---------------------------------------------------------------------
  本文件是 ABC 的 Liberty "语法层"（不是语义层）的最小可独立编译版本：
  作者 Alan Mishchenko / Niklas Een, UC Berkeley, 2012。
  函数名、结构体名和控制流尽量与 ABC 原文件一致。主要改动：

    1) 去掉 ABC 依赖：ABC_ALLOC/ABC_FREE -> malloc/free，
       Vec_Str_t 临时缓冲 -> 树里的一块 char*，Abc_Clock 计时 -> 删除。
    2) 删掉语法层以外的东西：ABC 在同一个文件里还做了
       "从语法树提取 SC_Cell/SC_Pin 用于工艺映射" 的工作
       (Scl_LibertyReadCell 等)。本文件只保留语法树解析。
    3) 修正 ABC 的一个内存安全缺陷：原文件用 long 保存 offset，
       并对 pError 用 sprintf 拼接；这里统一改成 snprintf，
       并把 offset 的类型写成 ssize_t，避免 32 位 long 截断。

  ABC 的 sclLiberty 只有语法层和工艺映射用的语义层（不读 ff/latch、不认
  timing_type），STA 需要的 setup/hold、时钟脚等信息它没有，因此这里只复用
  语法树，语义层（cell/pin/arc/表）在 msta_lib.c 里实现。

  ---------------------------------------------------------------------
  Liberty 的语法形状（这个解析器只认这三种条目）
  ---------------------------------------------------------------------
    过程   key(head){ 条目... }   —— 例如 cell("INV"){ pin("Y"){ ... } }
    等式   key : value ;          —— 例如 direction : "input";
    列表   key(head) ; 或 key(head)   —— 例如 index_1("0.01, 0.02");
  树里每个条目只保存三段 offset：Key / Head / Body（ABC 的 Scl_Pair_t）。
  字符串永远指回原文件缓冲，不复制 —— 这是 ABC 解析器快的原因之一，
  代价：树的生命周期内不能释放文件内容。

  用法（语义层常用的几个函数）：
    Scl_Tree_t *p = Scl_LibertyParse( pFileName );
    Scl_Item_t *pLib = Scl_LibertyFindTop( p, "library" );
    Scl_ItemForEachChildName( p, pLib, pCell, "cell" ) { ... }
    const char *pName = Scl_LibertyItemName( p, pCell );     // head 去引号，指向 scratch 缓冲
    double d = Scl_LibertyItemDouble( p, pItem, 0.0, &fOk ); // EQUA 的数值
    Scl_LibertyStop( p );

***********************************************************************/

#include <stdarg.h>

/* 本文件由 msta_lib.c 包含；部分解析辅助函数无需由该模块调用。 */
#define SCL_LIBERTY_UNUSED __attribute__((unused))

/* ---------------------------------------------------------------------
   数据结构（对应 ABC sclLiberty.c:43-82）
   --------------------------------------------------------------------- */

typedef enum {
    SCL_LIBERTY_NONE = 0,
    SCL_LIBERTY_PROC,            /* 1: key(head){body} */
    SCL_LIBERTY_EQUA,            /* 2: key : head;     */
    SCL_LIBERTY_LIST             /* 3: key(head)       */
} Scl_LibertyType_t;

typedef struct Scl_Pair_t_ {     /* ABC: Scl_Pair_t —— 一段 [Beg,End) 的字节区间 */
    ssize_t  Beg;
    ssize_t  End;
} Scl_Pair_t;

typedef struct Scl_Item_t_ {     /* ABC: Scl_Item_t —— 语法树的一个节点 */
    int        Type;
    ssize_t    iLine;            /* 出错时报行号用 */
    Scl_Pair_t Key;
    Scl_Pair_t Head;
    Scl_Pair_t Body;
    ssize_t    Next;             /* 兄弟节点（同一层的下一个条目） */
    ssize_t    Child;            /* 第一个孩子节点 */
} Scl_Item_t;

typedef struct {                 /* ABC: Scl_Tree_t */
    char       *pFileName;
    char       *pContents;       /* 整个文件，一次性读入 */
    ssize_t     nContents;
    ssize_t     nLines;          /* 解析过程中递增，用于报错行号 */
    ssize_t     nItems;
    ssize_t     nItemsAlloc;
    Scl_Item_t *pItems;          /* 所有节点存在一个池子里，用下标互指 */
    char       *pError;
    char       *pScratch;        /* 取代 ABC 的 Vec_Str_t，给 Scl_LibertyReadString 用 */
    ssize_t     nScratch;
} Scl_Tree_t;

/* 前向声明：出错信息里要打印条目名字。 */
static const char *Scl_LibertyReadString( Scl_Tree_t *pTree, Scl_Pair_t Range );

/* ---------------------------------------------------------------------
   小工具（对应 ABC sclLiberty.c:85-102）
   --------------------------------------------------------------------- */

static inline Scl_Item_t *Scl_LibertyRoot( Scl_Tree_t *p )
{   return p->pItems;  }

static inline Scl_Item_t *Scl_LibertyItem( Scl_Tree_t *p, ssize_t v )
{   assert( v <= p->nItems ); return v < 0 ? NULL : p->pItems + v;  }

/* 判断某段区间是否恰好等于一个 C 字符串字面量。不复制、不分配。
   返回值语义与 strcmp 相同：0 表示相等。Scl_ItemForEachChildName 依赖这一点。 */
static inline int Scl_LibertyCompare( Scl_Tree_t *p, Scl_Pair_t Pair, const char *pStr )
{
    size_t n = (size_t)(Pair.End - Pair.Beg);
    if ( strlen(pStr) != n )
        return 1;
    return strncmp( p->pContents + Pair.Beg, pStr, n );
}

static inline ssize_t Scl_LibertyItemId( Scl_Tree_t *p, Scl_Item_t *pItem )
{   return pItem - p->pItems;  }

/* 遍历 pItem 的所有孩子；以及"只遍历 key 等于某个名字的孩子"。 */
#define Scl_ItemForEachChild( p, pItem, pChild ) \
    for ( pChild = Scl_LibertyItem(p, pItem->Child); pChild; pChild = Scl_LibertyItem(p, pChild->Next) )

#define Scl_ItemForEachChildName( p, pItem, pChild, pName ) \
    for ( pChild = Scl_LibertyItem(p, pItem->Child); pChild; pChild = Scl_LibertyItem(p, pChild->Next) ) \
        if ( Scl_LibertyCompare(p, pChild->Key, pName) ) {} else

/* ---------------------------------------------------------------------
   词法级辅助（对应 ABC sclLiberty.c:208-360）
   --------------------------------------------------------------------- */

/* 把 C 风格块注释和行注释原地涂成空格。原地涂改可以保持所有 offset 有效。 */
static void Scl_LibertyWipeOutComments( char *pBeg, char *pEnd )
{
    char *pCur, *pStart;
    for ( pCur = pBeg; pCur < pEnd-1; pCur++ )
    {
        if ( pCur[0] == '/' && pCur[1] == '*' )
        {
            for ( pStart = pCur; pCur < pEnd-1; pCur++ )
                if ( pCur[0] == '*' && pCur[1] == '/' )
                {
                    for ( ; pStart < pCur + 2; pStart++ )
                        if ( *pStart != '\n' ) *pStart = ' ';
                    break;
                }
        }
        else if ( pCur[0] == '/' && pCur[1] == '/' )
        {
            for ( pStart = pCur; pCur < pEnd; pCur++ )
                if ( pCur[0] == '\n' || pCur == pEnd-1 )
                {
                    for ( ; pStart < pCur; pStart++ ) *pStart = ' ';
                    break;
                }
        }
    }
}

/* 注意：反斜杠被当作空白。Liberty 里 '\' 是转义标识符的前缀（如 \_BUF_ ），
   ABC 用它顺带实现 "行末续行" 和 "丢掉转义符"。本文件保持同样行为，
   语义层看到的 cell 名因此不带反斜杠。 */
static inline int Scl_LibertyCharIsSpace( char c )
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\\';
}

/* 跳过空白；返回 1 表示已经到了 pEnd。fStopAtNewLine 用于 "key : value" 这种
   一行一条的等式：等式的值不能跨行吞掉下一个条目的名字。 */
static inline int Scl_LibertySkipSpaces( Scl_Tree_t *p, char **ppPos, char *pEnd, int fStopAtNewLine )
{
    char *pPos = *ppPos;
    for ( ; pPos < pEnd; pPos++ )
    {
        if ( *pPos == '\n' )
        {
            p->nLines++;
            if ( fStopAtNewLine )
                break;
        }
        if ( !Scl_LibertyCharIsSpace(*pPos) )
            break;
    }
    *ppPos = pPos;
    return pPos == pEnd;
}

/* 读掉一个 "词"：引号串整体算一个词；否则到下一个分隔符为止。 */
static inline int Scl_LibertySkipEntry( char **ppPos, char *pEnd )
{
    char *pPos = *ppPos;
    if ( *pPos == '\"' )
    {
        for ( pPos++; pPos < pEnd; pPos++ )
            if ( *pPos == '\"' )
            {
                pPos++;
                break;
            }
    }
    else
    {
        for ( ; pPos < pEnd; pPos++ )
            if ( *pPos == ' '  || *pPos == '\r' || *pPos == '\n' || *pPos == '\t' ||
                 *pPos == ':'  || *pPos == ';'  || *pPos == '('  || *pPos == ')'  ||
                 *pPos == '{'  || *pPos == '}' )
                break;
    }
    *ppPos = pPos;
    return pPos == pEnd;
}

/* 从 pPos 指向的 '(' 或 '{' 找到配对的 ')' 或 '}'。 */
static inline char *Scl_LibertyFindMatch( char *pPos, char *pEnd )
{
    long nCounter = 0;
    char This = *pPos;
    char That = (This == '(') ? ')' : '}';
    assert( This == '(' || This == '{' );
    ++nCounter;
    for ( pPos++; pPos < pEnd; pPos++ )
    {
        if ( *pPos == This )
            nCounter++;
        else if ( *pPos == That )
            if ( --nCounter == 0 )
                break;
    }
    return pPos;
}

/* 去掉 head 两端的空白（Liberty 里 head 常带换行）。 */
static inline Scl_Pair_t Scl_LibertyUpdateHead( Scl_Tree_t *p, Scl_Pair_t Head )
{
    Scl_Pair_t Res = Head;
    char *pBeg  = p->pContents + Head.Beg;
    char *pEnd  = p->pContents + Head.End;
    char *pFirst = NULL, *pLast = NULL, *pChar;
    for ( pChar = pBeg; pChar < pEnd; pChar++ )
    {
        if ( *pChar == '\n' ) { p->nLines++; continue; }
        if ( Scl_LibertyCharIsSpace(*pChar) ) continue;
        pLast = pChar;
        if ( pFirst == NULL ) pFirst = pChar;
    }
    if ( pFirst && pLast )
    {
        Res.Beg = pFirst - p->pContents;
        Res.End = pLast  - p->pContents + 1;
    }
    return Res;
}

static inline Scl_Item_t *Scl_LibertyNewItem( Scl_Tree_t *p, int Type )
{
    Scl_Item_t *pItem = p->pItems + p->nItems++;
    pItem->iLine = p->nLines;
    pItem->Type  = Type;
    pItem->Child = -1;
    pItem->Next  = -1;
    return pItem;
}

/* ---------------------------------------------------------------------
   递归下降：读一个条目，返回它的下标。
     -2 表示 "读到结尾了"（正常结束）
     -1 表示 "语法错误"（ABC 会记下 pError 并继续用残缺的树）
   对应 ABC sclLiberty.c:411-521。
   --------------------------------------------------------------------- */
static ssize_t Scl_LibertyBuildItem( Scl_Tree_t *p, char **ppPos, char *pEnd )
{
    Scl_Item_t *pItem;
    Scl_Pair_t Key, Head, Body;
    char *pNext, *pStop;

    Key.End = 0;
    if ( Scl_LibertySkipSpaces(p, ppPos, pEnd, 0) )
        return -2;

    /* 第一段：key */
    Key.Beg = *ppPos - p->pContents;
    if ( Scl_LibertySkipEntry(ppPos, pEnd) )
        goto exit;
    Key.End = *ppPos - p->pContents;
    if ( Scl_LibertySkipSpaces(p, ppPos, pEnd, 0) )
        goto exit;
    pNext = *ppPos;

    /* ---- 形状 1: key : value ;  （等式） ---- */
    if ( *pNext == ':' )
    {
        *ppPos = pNext + 1;
        if ( Scl_LibertySkipSpaces(p, ppPos, pEnd, 0) )
            goto exit;
        Head.Beg = *ppPos - p->pContents;
        if ( Scl_LibertySkipEntry(ppPos, pEnd) )
            goto exit;
        Head.End = *ppPos - p->pContents;
        if ( Scl_LibertySkipSpaces(p, ppPos, pEnd, 1) )
            goto exit;
        pNext = *ppPos;
        /* 支持 `a : b + c;` 这类算式，把整段当成一个值 */
        while ( *pNext == '+' || *pNext == '-' || *pNext == '*' || *pNext == '/' )
        {
            (*ppPos) += 1;
            if ( Scl_LibertySkipSpaces(p, ppPos, pEnd, 0) ) goto exit;
            if ( Scl_LibertySkipEntry(ppPos, pEnd) )        goto exit;
            Head.End = *ppPos - p->pContents;
            if ( Scl_LibertySkipSpaces(p, ppPos, pEnd, 1) ) goto exit;
            pNext = *ppPos;
        }
        if ( *pNext != ';' && *pNext != '\n' )
            goto exit;
        *ppPos = pNext + 1;
        pItem = Scl_LibertyNewItem( p, SCL_LIBERTY_EQUA );
        pItem->Key  = Key;
        pItem->Head = Scl_LibertyUpdateHead( p, Head );
        pItem->Next = Scl_LibertyBuildItem( p, ppPos, pEnd );
        if ( pItem->Next == -1 )
            goto exit;
        return Scl_LibertyItemId( p, pItem );
    }

    /* ---- 形状 2/3: key(head)... （过程 或 列表） ---- */
    if ( *pNext == '(' )
    {
        pStop    = Scl_LibertyFindMatch( pNext, pEnd );
        Head.Beg = pNext - p->pContents + 1;
        Head.End = pStop - p->pContents;
        *ppPos   = pStop + 1;
        if ( Scl_LibertySkipSpaces(p, ppPos, pEnd, 0) )
        {   /* 文件到此结束：只能是列表 */
            pItem = Scl_LibertyNewItem( p, SCL_LIBERTY_LIST );
            pItem->Key  = Key;
            pItem->Head = Scl_LibertyUpdateHead( p, Head );
            return Scl_LibertyItemId( p, pItem );
        }
        pNext = *ppPos;
        if ( *pNext == '{' )
        {   /* 带 body 的过程：递归解析 body 里的条目 */
            ssize_t nChild, nNext;
            pStop    = Scl_LibertyFindMatch( pNext, pEnd );
            Body.Beg = pNext - p->pContents + 1;
            Body.End = pStop - p->pContents;
            pItem = Scl_LibertyNewItem( p, SCL_LIBERTY_PROC );
            pItem->Key  = Key;
            pItem->Head = Scl_LibertyUpdateHead( p, Head );
            pItem->Body = Body;
            *ppPos = pNext + 1;
            nChild = Scl_LibertyBuildItem( p, ppPos, pStop );
            if ( nChild == -1 )
                goto exit;
            pItem->Child = (nChild == -2) ? -1 : nChild;
            *ppPos = pStop + 1;
            nNext  = Scl_LibertyBuildItem( p, ppPos, pEnd );
            if ( nNext == -1 )
                goto exit;
            pItem->Next = nNext;
            return Scl_LibertyItemId( p, pItem );
        }
        /* 没有 body：key(head); 形式，仍然是列表 */
        if ( *pNext == ';' )
            *ppPos = pNext + 1;
        pItem = Scl_LibertyNewItem( p, SCL_LIBERTY_LIST );
        pItem->Key  = Key;
        pItem->Head = Scl_LibertyUpdateHead( p, Head );
        pItem->Next = Scl_LibertyBuildItem( p, ppPos, pEnd );
        if ( pItem->Next == -1 )
            goto exit;
        return Scl_LibertyItemId( p, pItem );
    }

    /* ---- 形状 4: 光一个 ';' —— 空语句，跳过继续 ---- */
    if ( *pNext == ';' )
    {
        *ppPos = pNext + 1;
        return Scl_LibertyBuildItem( p, ppPos, pEnd );
    }

exit:
    if ( p->pError == NULL )
    {
        p->pError = (char *)malloc( 1000 );
        assert( p->pError );
        snprintf( p->pError, 1000, "File \"%s\". Line %6ld. Failed to parse entry \"%.60s\".\n",
            p->pFileName ? p->pFileName : "(memory)", (long)p->nLines,
            Scl_LibertyReadString(p, Key) );
    }
    return -1;
}

/* 数一数文件里大概有多少个条目，一次把节点池子分足（ABC 的优化：
   每个 '(' 或 ':' 大致对应一个条目）。这里把 ';' 也计入，宁多勿少：
   Scl_LibertyNewItem 不检查池子是否用尽。 */
static ssize_t Scl_LibertyCountItems( const char *pBeg, const char *pEnd )
{
    ssize_t nCounter = 0;
    for ( ; pBeg < pEnd; pBeg++ )
        nCounter += (*pBeg == '(' || *pBeg == ':' || *pBeg == ';');
    return nCounter;
}

/* ---------------------------------------------------------------------
   对外接口：建树 / 析构 / 取值
   --------------------------------------------------------------------- */

static void Scl_LibertyStop( Scl_Tree_t *p )
{
    if ( p == NULL )
        return;
    free( p->pScratch );
    free( p->pFileName );
    free( p->pContents );
    free( p->pItems );
    free( p->pError );
    free( p );
}

/* 直接对内存里的文本建树；pContents 会被就地修改（涂注释），所有权转移给树。 */
static Scl_Tree_t *Scl_LibertyParseString( const char *pFileName, char *pContents, ssize_t nContents )
{
    Scl_Tree_t *p;
    char *pPos;

    p = (Scl_Tree_t *)calloc( 1, sizeof(Scl_Tree_t) );
    assert( p );
    p->pFileName  = Msta_StrDup( pFileName );
    p->pContents  = pContents;
    p->nContents  = nContents;
    p->nLines     = 1;
    p->nItemsAlloc = 10 + 2 * Scl_LibertyCountItems( pContents, pContents + nContents );
    p->pItems     = (Scl_Item_t *)calloc( (size_t)p->nItemsAlloc, sizeof(Scl_Item_t) );
    assert( p->pItems );
    p->nScratch   = 1024;
    p->pScratch   = (char *)malloc( (size_t)p->nScratch );
    assert( p->pScratch );

    Scl_LibertyWipeOutComments( p->pContents, p->pContents + p->nContents );
    pPos = p->pContents;
    if ( Scl_LibertyBuildItem( p, &pPos, p->pContents + p->nContents ) == -1 )
    {
        if ( p->pError )
            Msta_Error( "%s", p->pError );
        Msta_Error( "Liberty syntax error: the tree is incomplete.\n" );
    }
    return p;
}

static Scl_Tree_t *Scl_LibertyParse( const char *pFileName )
{
    size_t nSize = 0;
    char *pContents = Msta_FileReadAll( pFileName, &nSize );
    if ( pContents == NULL )
        return NULL;
    return Scl_LibertyParseString( pFileName, pContents, (ssize_t)nSize );
}

/* 把区间取成一个 C 字符串（去掉外层引号）。返回的指针在下次调用前有效，
   语义层要长期保留的话必须自己复制 —— msta_lib.c 里都会立即 intern。 */
static const char *Scl_LibertyReadString( Scl_Tree_t *p, Scl_Pair_t Pair )
{
    ssize_t n = Pair.End - Pair.Beg;
    if ( n < 0 ) n = 0;
    if ( n + 1 > p->nScratch )
    {
        p->nScratch = (int)(n + 100);
        p->pScratch = (char *)realloc( p->pScratch, (size_t)p->nScratch );
        assert( p->pScratch );
    }
    memcpy( p->pScratch, p->pContents + Pair.Beg, (size_t)n );
    p->pScratch[n] = 0;
    if ( n >= 2 && p->pScratch[0] == '\"' && p->pScratch[n-1] == '\"' )
    {
        p->pScratch[n-1] = 0;
        return p->pScratch + 1;
    }
    return p->pScratch;
}

/* 条目的 head 文本（cell("INV") -> INV；area : 1.2 -> 1.2）。 */
#define Scl_LibertyItemName(p, pItem)  Scl_LibertyReadString( (p), (pItem)->Head )

/* 条目 head 的原始字节区间，不去引号、不复制。msta_lib.c 用它读 LIST 条目
   （index_1("...")、capacitive_load_unit(...)）里的数字。 */
#define Scl_LibertyItemRaw(pTree_, pItem_, ppBeg_, pnLen_)          \
    do {                                                            \
        *(ppBeg_) = (pTree_)->pContents + (pItem_)->Head.Beg;       \
        *(pnLen_) = (ssize_t)((pItem_)->Head.End - (pItem_)->Head.Beg); \
    } while (0)

/* 语法树的 key 文本，用于打印调试。 */
static SCL_LIBERTY_UNUSED const char *Scl_LibertyItemKey( Scl_Tree_t *p, Scl_Item_t *pItem )
{
    return Scl_LibertyReadString( p, pItem->Key );
}

static SCL_LIBERTY_UNUSED int Scl_LibertyItemIsEqua( Scl_Item_t *pItem )
{   return pItem && pItem->Type == SCL_LIBERTY_EQUA;  }

/* 取一个等式条目的 double 值；取不到就返回默认值并把 *pfOk 置 0。
   Liberty 里 "area : 3.2;" 和 "capacitance : 4.2e-05;" 都走这里。 */
static SCL_LIBERTY_UNUSED double Scl_LibertyItemDouble( Scl_Tree_t *p, Scl_Item_t *pItem, double Default, int *pfOk )
{
    char *pEndPtr;
    double Value;
    if ( pfOk ) *pfOk = 0;
    if ( pItem == NULL || pItem->Type != SCL_LIBERTY_EQUA )
        return Default;
    Value = strtod( Scl_LibertyReadString(p, pItem->Head), &pEndPtr );
    if ( pEndPtr == Scl_LibertyReadString(p, pItem->Head) )   /* 一个数字都没读到 */
        return Default;
    if ( pfOk ) *pfOk = 1;
    return Value;
}

/* Liberty 的布尔值写成 "true"/"false"，也有直接写 1/0 的。 */
static SCL_LIBERTY_UNUSED int Scl_LibertyItemBool( Scl_Tree_t *pTree, Scl_Item_t *pItem, int fDefault )
{
    const char *pText;
    if ( pItem == NULL || pItem->Type != SCL_LIBERTY_EQUA )
        return fDefault;
    pText = Scl_LibertyReadString(pTree, pItem->Head);
    if ( !strcasecmp(pText, "true") || !strcmp(pText, "1") )  return 1;
    if ( !strcasecmp(pText, "false") || !strcmp(pText, "0") ) return 0;
    return fDefault;
}

/* 在 pRoot 的孩子里找第一个 key 等于 pName 的条目。 */
static Scl_Item_t *Scl_LibertyFindChild( Scl_Tree_t *p, Scl_Item_t *pRoot, const char *pName )
{
    Scl_Item_t *pChild;
    if ( pRoot == NULL ) return NULL;
    Scl_ItemForEachChildName( p, pRoot, pChild, pName )
        return pChild;
    return NULL;
}

/* 在顶层条目链上找 key 等于 pName 的条目。
   ABC 直接假定 pItems[0] 就是 library（大多数库确实如此）；这里沿 Next 链兜底，
   因为少数库文件会以 version/define 之类的条目开头。 */
static SCL_LIBERTY_UNUSED Scl_Item_t *Scl_LibertyFindTop( Scl_Tree_t *p, const char *pName )
{
    Scl_Item_t *pItem;
    for ( pItem = Scl_LibertyRoot(p); pItem; pItem = Scl_LibertyItem(p, pItem->Next) )
        if ( Scl_LibertyCompare(p, pItem->Key, pName) == 0 )
            return pItem;
    return NULL;
}

/* 统计 key 等于 pName 的孩子个数。 */
static SCL_LIBERTY_UNUSED int Scl_LibertyCountChildren( Scl_Tree_t *p, Scl_Item_t *pRoot, const char *pName )
{
    Scl_Item_t *pChild;
    int n = 0;
    Scl_ItemForEachChildName( p, pRoot, pChild, pName )
        n++;
    return n;
}

/* 按 Liberty 语法输出语法树。 */
static void Scl_LibertyDumpItem( FILE *pFile, Scl_Tree_t *p, Scl_Item_t *pItem, int nOffset, int fDepth );

static void Scl_LibertyDumpChildren( FILE *pFile, Scl_Tree_t *p, Scl_Item_t *pItem, int nOffset, int fDepth )
{
    Scl_Item_t *pChild;
    if ( !fDepth ) return;
    Scl_ItemForEachChild( p, pItem, pChild )
        Scl_LibertyDumpItem( pFile, p, pChild, nOffset + 2, fDepth - 1 );
}

static void Scl_LibertyDumpItem( FILE *pFile, Scl_Tree_t *p, Scl_Item_t *pItem, int nOffset, int fDepth )
{
    int i;
    for ( i = 0; i < nOffset; i++ ) fputc( ' ', pFile );
    if ( pItem->Type == SCL_LIBERTY_EQUA )
        fprintf( pFile, "%s : %s;\n", Scl_LibertyItemKey(p, pItem), Scl_LibertyItemName(p, pItem) );
    else if ( pItem->Type == SCL_LIBERTY_LIST )
        fprintf( pFile, "%s(%s);\n", Scl_LibertyItemKey(p, pItem), Scl_LibertyItemName(p, pItem) );
    else
    {
        fprintf( pFile, "%s(%s) {\n", Scl_LibertyItemKey(p, pItem), Scl_LibertyItemName(p, pItem) );
        Scl_LibertyDumpChildren( pFile, p, pItem, nOffset, fDepth );
        for ( i = 0; i < nOffset; i++ ) fputc( ' ', pFile );
        fprintf( pFile, "}\n" );
    }
}

static SCL_LIBERTY_UNUSED void Scl_LibertyDump( Scl_Tree_t *p, FILE *pFile, int fDepth )
{
    Scl_Item_t *pItem;
    Scl_ItemForEachChild( p, Scl_LibertyRoot(p), pItem )
        Scl_LibertyDumpItem( pFile, p, pItem, 0, fDepth );
}
