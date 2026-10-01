/**CHeader*************************************************************

  FileName    [msta_types.h]

  SystemName  [miniSTA]

  Synopsis    [全工程公用的类型、单位约定和动态数组模板。]

  ---------------------------------------------------------------------
  单位约定（重要，所有模块统一遵守）
  ---------------------------------------------------------------------
    时间  : ps   (皮秒)     —— Liberty 里通常是 ns，读入时换算
    负载  : fF   (飞法)     —— Liberty 里通常是 ff 或 pf，读入时换算
    摆率  : ps   (皮秒)     —— 与时间同一单位，表示 0->1 的翻转耗时
    报告  : 输出时再换算回 ns，便于和手册对照

  内部一律用 double 存放上述单位的数值（见 MstaTime / MstaCap）。

  ---------------------------------------------------------------------
  ID 约定
  ---------------------------------------------------------------------
  网表/时序图里的对象一律用 "int id + 数组下标" 表示，不用裸指针传来传去。
  ID 可用于数组索引，越界访问由断言捕获。
  MSTA_NO_ID 表示"没有"，任何 id 都 >= 0。

***********************************************************************/

#ifndef MSTA_TYPES_H
#define MSTA_TYPES_H

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#define MSTA_NO_ID      (-1)

typedef int             MstaId; /* 数组下标式的对象编号 */
typedef double          MstaTime;
typedef double          MstaCap;

/* ---------------------------------------------------------------------
   MstaArray(T, Name) —— 通用动态数组：一段连续内存 + size/capacity。
   展开后提供初始化、追加、索引、释放和查找函数。
   约定：元素永远值拷贝存放，销毁数组即销毁元素本身；
   元素里指向的堆内存（如 char * 字符串）要由调用方先释放。
   --------------------------------------------------------------------- */
/* 一个 .c 文件通常只用到展开出的部分函数，标成 unused 免得编译器对其余函数告警。 */
#define MSTA_UNUSED     __attribute__((unused))

#define MstaArrayDefine(Type, Name)                                           \
typedef struct {                                                              \
    Type *pData;                                                              \
    int   nSize;                                                              \
    int   nCapacity;                                                          \
} Name;                                                                       \
MSTA_UNUSED static inline void Name##Init( Name *p )                          \
{   p->pData = NULL; p->nSize = 0; p->nCapacity = 0;  }                       \
MSTA_UNUSED static inline Type *Name##Append( Name *p )                       \
{                                                                             \
    if ( p->nSize == p->nCapacity )                                           \
    {                                                                         \
        int nNew = p->nCapacity ? p->nCapacity * 2 : 8;                       \
        p->pData = (Type *)realloc( p->pData, (size_t)nNew * sizeof(Type) );  \
        assert( p->pData );                                                   \
        memset( p->pData + p->nCapacity, 0, (size_t)(nNew - p->nCapacity) * sizeof(Type) ); \
        p->nCapacity = nNew;                                                  \
    }                                                                         \
    memset( &p->pData[p->nSize], 0, sizeof(Type) );                           \
    return &p->pData[ p->nSize++ ];                                           \
}                                                                             \
MSTA_UNUSED static inline Type *Name##At( Name *p, int i )                    \
{   assert( 0 <= i && i < p->nSize ); return &p->pData[i];  }                 \
MSTA_UNUSED static inline void Name##Free( Name *p )                          \
{   free( p->pData ); p->pData = NULL; p->nSize = p->nCapacity = 0;  }        \
MSTA_UNUSED static inline int Name##Find( Name *p, Type *pElem )              \
{   return (pElem < p->pData || pElem >= p->pData + p->nSize)                 \
        ? MSTA_NO_ID : (int)(pElem - p->pData);  }

/* 容器使用下标循环和 XxxAt() 遍历。 */

/* ---------------------------------------------------------------------
   全局日志：一律写到 stderr；警告和错误分别带 "** Warning: " /
   "** Error: " 前缀，普通信息不带前缀。
   --------------------------------------------------------------------- */
typedef enum {
    MSTA_LOG_INFO,
    MSTA_LOG_WARN,
    MSTA_LOG_ERROR
} MstaLogLevel;

void Msta_Log( MstaLogLevel Level, const char *pFormat, ... );

#define Msta_Info(...)   Msta_Log( MSTA_LOG_INFO,  __VA_ARGS__ )
#define Msta_Warn(...)   Msta_Log( MSTA_LOG_WARN,  __VA_ARGS__ )
#define Msta_Error(...)  Msta_Log( MSTA_LOG_ERROR, __VA_ARGS__ )

/* 分析过程中发现的可解释问题（比如未约束的端点）：Msta_WarnOnce 立即打印，
   同一句只打一次；g_vMstaWarnings 记录已打过的句子。 */
MstaArrayDefine( char *, MstaMsgArray )
extern MstaMsgArray g_vMstaWarnings;
void Msta_WarnOnce( const char *pFormat, ... );

#endif /* MSTA_TYPES_H */
