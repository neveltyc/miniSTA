/**CHeader*************************************************************

  FileName    [msta_json.h]

  Synopsis    [极简 JSON 读取器 —— 读 Yosys 网表与 SDC Tcl 前端的中间格式。]

  Verilog 的语法细节交给 yosys，msta 只读它 write_json 出来的中间格式：
  形状固定、字段少，所以解析器可以很小。

  JSON 形状（yosys 0.33 write_json）：
    { "creator": "...",
      "modules": {
        "<模块名>": {
          "ports": { "<端口名>": {"direction":"input|output|inout", "bits":[2,3]} },
          "cells": { "<实例名>": {"type":"<单元名>",
                                  "parameters":{...},
                                  "connections": {"<脚名>":[6], "<多位脚>":[2,3]}} },
          "netnames": { "<线名>": {"bits":[6]} }
        } } }
  约定：bits 里的整数是网络编号；字符串 "0"/"1" 是常量 1'b0 / 1'b1（"x"/"z" 按 0 处理）；
        每个模块自己给网络编号，所以跨模块时要用 (模块路径, 编号) 二元组。

  这个解析器只做 "读"，不做 "写"，且刻意保守：不认识的输入一律报错。

***********************************************************************/

#ifndef MSTA_JSON_H
#define MSTA_JSON_H

#include "msta_types.h"

typedef enum {
    MJSON_NULL = 0,
    MJSON_TRUE,
    MJSON_FALSE,
    MJSON_NUMBER,
    MJSON_STRING,
    MJSON_ARRAY,
    MJSON_OBJECT
} MJsonKind;

typedef struct MJsonValue MJsonValue;
struct MJsonValue {
    MJsonKind  Kind;
    double     Num;          /* NUMBER */
    char      *pStr;         /* STRING */
    char     **pKeys;        /* OBJECT: 每个孩子的名字（ARRAY 时为 NULL） */
    MJsonValue **ppItems;    /* OBJECT/ARRAY 的孩子 */
    int        nItems;
};

/* 解析一段 JSON 文本。失败返回 NULL 并把原因写进 pErrBuf。 */
MJsonValue *Msta_JsonParse( const char *pText, size_t nLength, char *pErrBuf, int nErrBuf );
/* 直接读文件。 */
MJsonValue *Msta_JsonParseFile( const char *pFileName, char *pErrBuf, int nErrBuf );
void        Msta_JsonFree( MJsonValue *p );

/* ---- 查询助手：取不到就返回默认值，调用方不必层层判空 ---- */
MJsonValue *Msta_JsonGet( MJsonValue *pObj, const char *pKey );
const char *Msta_JsonStr( MJsonValue *pObj, const char *pKey, const char *pDefault );
int         Msta_JsonIsArray( MJsonValue *pValue );
const char *Msta_JsonKeyAt( MJsonValue *pObj, int i );
MJsonValue *Msta_JsonAt( MJsonValue *pValue, int i );
int         Msta_JsonCount( MJsonValue *pValue );

#endif /* MSTA_JSON_H */
