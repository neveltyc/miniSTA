/**CHeader*************************************************************

  FileName    [msta_util.h]

  Synopsis    [字符串、名字表和文件读入这三件小事。]

  这里只提供三样东西，每样都不依赖工程里其它模块：
    1. Msta_StrDup         —— 复制字符串
    2. MstaNameTable       —— 字符串 <-> 整数 id 的双向映射（内部驻留）
    3. Msta_FileReadAll    —— 一次把整个文件读进内存
  名字表是网表/库/约束之间互相引用的黏合剂：把"CLK"这种字符串换成一个 int，
  后续查表使用整数 ID。

***********************************************************************/

#ifndef MSTA_UTIL_H
#define MSTA_UTIL_H

#include "msta_types.h"

char *Msta_StrDup( const char *pStr );

/* ---------------- 文件 ---------------- */

/* 把整个文件读进一块新分配的内存，*pnSize 返回字节数。失败返回 NULL。 */
char *Msta_FileReadAll( const char *pFileName, size_t *pnSize );

/* ---------------- 名字表 ---------------- */

typedef struct MstaNameEntry {
    char                *pName;   /* 驻留下来的字符串 */
    MstaId               Id;      /* 它在 pNames 里的下标 */
    struct MstaNameEntry *pNext;  /* 同一桶里的下一个 */
} MstaNameEntry;

typedef struct {
    /* 每个桶是一条单向链表；桶数在建表时确定，不做 rehash。 */
    MstaNameEntry **ppBuckets;
    int             nBuckets;
    const char    **pNames;   /* id -> 字符串，顺序就是第一次出现的顺序 */
    int             nNames;
    int             nNamesCap;
} MstaNameTable;

MstaNameTable *Msta_NameTableStart( void );
/* 拿到字符串对应的 id；没见过就新建。字符串会被复制进表里。 */
MstaId         Msta_NameTableId( MstaNameTable *p, const char *pName );
/* id 对应的字符串；越界时返回 "?"，保证打印路径时不会崩。 */
const char    *Msta_NameTableName( MstaNameTable *p, MstaId id );

/* ---------------- 整数哈希表：key(int) -> value(int) ----------------
   网表和时序图里到处需要 "名字id -> 数组下标" 的映射（模块名、网络名、
   单元名……）。这里放唯一的一份实现：键和值都是 int，空槽用 -1 表示。 */
typedef struct {
    int *pKeys;
    int *pValues;
    int  nSize;
    int  nCap;              /* 桶数，始终是 2 的幂 */
} MstaIntMap;

void Msta_IntMapInit( MstaIntMap *p, int nCapWanted );
void Msta_IntMapFree( MstaIntMap *p );
int  Msta_IntMapGet( MstaIntMap *p, int Key, int DefaultValue );
void Msta_IntMapSet( MstaIntMap *p, int Key, int Value );

/* ---------------- 参数切分 ----------------
   dofile 和 sdc 都用同一套规则：空白分词、双引号包住带空格的参数、
   # 之后是注释。就地改写输入缓冲区，argv 指向缓冲区内部。
   返回参数个数。 */
int Msta_SplitArgs( char *pLine, char **argv, int nMaxArgs );

/* 供库、网表和约束共用的名字表。 */
MstaNameTable *Msta_Names( void );
/* 常用到的两个转发，省掉满屏的 Msta_Names()。 */
#define Msta_NameId(pStr)      Msta_NameTableId( Msta_Names(), (pStr) )
#define Msta_NameStr(id)       Msta_NameTableName( Msta_Names(), (id) )

#endif /* MSTA_UTIL_H */
