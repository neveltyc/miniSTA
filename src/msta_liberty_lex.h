/**CHeader*************************************************************

  FileName    [msta_liberty_lex.h]

  SystemName  [msta]

  Synopsis    [Liberty 语法树解析器 —— 独立自 berkeley-abc/abc 的 src/map/scl/sclLiberty.c]

  ---------------------------------------------------------------------
  来源与版权声明（UC Berkeley ABC）
  ---------------------------------------------------------------------
  本文件及对应的 msta_liberty_lex.c 拆分自原本的 abc_scl_liberty_tree.inc，
  源自 UC Berkeley ABC (https://github.com/berkeley-abc/abc) 中 sclLiberty.c 的
  Liberty 语法层（作者 Alan Mishchenko / Niklas Een, UC Berkeley, 2012）。
  主要适配：用 ssize_t 保存偏移、snprintf 防溢出，去掉 ABC 容器依赖并在遇到未闭合
  括号时立即报错返回。第三方完整声明亦见仓库根目录 THIRD_PARTY_NOTICES.md。

  ABC: System for Sequential Synthesis and Verification
  http://www.eecs.berkeley.edu/~alanmi/abc/

  Copyright (c) The Regents of the University of California. All rights reserved.

  Permission is hereby granted, without written agreement and without license or
  royalty fees, to use, copy, modify, and distribute this software and its
  documentation for any purpose, provided that the above copyright notice and
  the following two paragraphs appear in all copies of this software.

  IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO ANY PARTY FOR
  DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES ARISING OUT OF
  THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF
  CALIFORNIA HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

  THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING,
  BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE. THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS" BASIS,
  AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO PROVIDE MAINTENANCE,
  SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.

***********************************************************************/

#ifndef MSTA_LIBERTY_LEX_H
#define MSTA_LIBERTY_LEX_H

#include <sys/types.h>
#include "msta_types.h"

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

/* 空文件、只有注释或第一个条目就解析失败时一个条目都没有，返回 NULL。 */
static inline Scl_Item_t *Scl_LibertyRoot( Scl_Tree_t *p )
{   return p->nItems > 0 ? p->pItems : NULL;  }

static inline Scl_Item_t *Scl_LibertyItem( Scl_Tree_t *p, ssize_t v )
{   assert( v <= p->nItems ); return v < 0 ? NULL : p->pItems + v;  }

/* 判断某段区间是否恰好等于一个 C 字符串字面量（0 表示相等）。 */
static inline int Scl_LibertyCompare( Scl_Tree_t *p, Scl_Pair_t Pair, const char *pStr )
{
    size_t n = (size_t)(Pair.End - Pair.Beg);
    if ( strlen(pStr) != n )
        return 1;
    return strncmp( p->pContents + Pair.Beg, pStr, n );
}

static inline ssize_t Scl_LibertyItemId( Scl_Tree_t *p, Scl_Item_t *pItem )
{   return pItem - p->pItems;  }

/* 只遍历 pItem 里 key 等于某个名字的孩子。 */
#define Scl_ItemForEachChildName( p, pItem, pChild, pName ) \
    for ( pChild = Scl_LibertyItem(p, pItem->Child); pChild; pChild = Scl_LibertyItem(p, pChild->Next) ) \
        if ( Scl_LibertyCompare(p, pChild->Key, pName) ) {} else

/* 条目的 head 文本（cell("INV") -> INV；area : 1.2 -> 1.2）。 */
#define Scl_LibertyItemName(p, pItem)  Scl_LibertyReadString( (p), (pItem)->Head )

/* 条目 head 的原始字节区间，不去引号、不复制。 */
#define Scl_LibertyItemRaw(pTree_, pItem_, ppBeg_, pnLen_)              \
    do {                                                                \
        *(ppBeg_) = (pTree_)->pContents + (pItem_)->Head.Beg;           \
        *(pnLen_) = (ssize_t)((pItem_)->Head.End - (pItem_)->Head.Beg); \
    } while (0)

Scl_Tree_t *Scl_LibertyParse( const char *pFileName );
void        Scl_LibertyStop( Scl_Tree_t *p );
const char *Scl_LibertyReadString( Scl_Tree_t *p, Scl_Pair_t Pair );
double      Scl_LibertyItemDouble( Scl_Tree_t *p, Scl_Item_t *pItem, double Default, int *pfOk );
int         Scl_LibertyItemBool( Scl_Tree_t *pTree, Scl_Item_t *pItem, int fDefault );
Scl_Item_t *Scl_LibertyFindChild( Scl_Tree_t *p, Scl_Item_t *pRoot, const char *pName );
Scl_Item_t *Scl_LibertyFindTop( Scl_Tree_t *p, const char *pName );

#endif /* MSTA_LIBERTY_LEX_H */
