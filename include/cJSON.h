/*
  Copyright (c) 2009-2017 Dave Gamble and cJSON contributors

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
*/

#ifndef cJSON__h
#define cJSON__h

/* ================================================================
 *   cJSON.h — 超轻量级 JSON 解析/生成库（v1.7.19）
 *
 *   数据模型：整个 JSON 文档映射为一棵 cJSON 树。
 *     对象 {} 和数组 [] 是内部节点（child→子节点链表），
 *     字符串/数值/布尔/null 是叶子节点。
 *
 *   核心使用模式：
 *     读取：     cJSON_Parse(str) → cJSON_GetObjectItem() → 取值 → cJSON_Delete()
 *     构建：     cJSON_CreateObject() → cJSON_AddXxxToObject() → cJSON_Print() → cJSON_Delete()
 *
 *   内存规则：  cJSON_Parse 和 cJSON_Create* 返回的节点必须由调用者
 *              通过 cJSON_Delete() 释放；cJSON_Print 返回的字符串
 *              由调用者通过 free() 释放。
 * ================================================================ */

#ifdef __cplusplus
extern "C"
{
#endif

#if !defined(__WINDOWS__) && (defined(WIN32) || defined(WIN64) || defined(_MSC_VER) || defined(_WIN32))
#define __WINDOWS__
#endif

#ifdef __WINDOWS__

/* When compiling for windows, we specify a specific calling convention to avoid issues where we are being called from a project with a different default calling convention.  For windows you have 3 define options:

CJSON_HIDE_SYMBOLS - Define this in the case where you don't want to ever dllexport symbols
CJSON_EXPORT_SYMBOLS - Define this on library build when you want to dllexport symbols (default)
CJSON_IMPORT_SYMBOLS - Define this if you want to dllimport symbol

For *nix builds that support visibility attribute, you can define similar behavior by

setting default visibility to hidden by adding
-fvisibility=hidden (for gcc)
or
-xldscope=hidden (for sun cc)
to CFLAGS

then using the CJSON_API_VISIBILITY flag to "export" the same symbols the way CJSON_EXPORT_SYMBOLS does

*/

#define CJSON_CDECL __cdecl
#define CJSON_STDCALL __stdcall

/* export symbols by default, this is necessary for copy pasting the C and header file */
#if !defined(CJSON_HIDE_SYMBOLS) && !defined(CJSON_IMPORT_SYMBOLS) && !defined(CJSON_EXPORT_SYMBOLS)
#define CJSON_EXPORT_SYMBOLS
#endif

#if defined(CJSON_HIDE_SYMBOLS)
#define CJSON_PUBLIC(type)   type CJSON_STDCALL
#elif defined(CJSON_EXPORT_SYMBOLS)
#define CJSON_PUBLIC(type)   __declspec(dllexport) type CJSON_STDCALL
#elif defined(CJSON_IMPORT_SYMBOLS)
#define CJSON_PUBLIC(type)   __declspec(dllimport) type CJSON_STDCALL
#endif
#else /* !__WINDOWS__ */
#define CJSON_CDECL
#define CJSON_STDCALL

#if (defined(__GNUC__) || defined(__SUNPRO_CC) || defined (__SUNPRO_C)) && defined(CJSON_API_VISIBILITY)
#define CJSON_PUBLIC(type)   __attribute__((visibility("default"))) type
#else
#define CJSON_PUBLIC(type) type
#endif
#endif

/* 项目版本号：可通过 cJSON_Version() 运行时查询 */
#define CJSON_VERSION_MAJOR 1
#define CJSON_VERSION_MINOR 7
#define CJSON_VERSION_PATCH 19

/* size_t 定义，用于 ParseWithLength 等函数 */
#include <stddef.h>

/* cJSON 节点类型标志位（type 字段的可组合位掩码）—————————————
 *   低 8 位用于基本类型，用移位实现互斥的类型编码；
 *   高位（256/512）是附加标志位，可与基本类型组合使用。
 *   e.g. (cJSON_String | cJSON_IsReference) 表示一个"引用字符串" */
#define cJSON_Invalid (0)        /* 无效类型 / 未初始化 */
#define cJSON_False  (1 << 0)   /* false 布尔值 */
#define cJSON_True   (1 << 1)   /* true 布尔值 */
#define cJSON_NULL   (1 << 2)   /* null 值 */
#define cJSON_Number (1 << 3)   /* 数值类型（int / double） */
#define cJSON_String (1 << 4)   /* 字符串类型 */
#define cJSON_Array  (1 << 5)   /* 数组类型 */
#define cJSON_Object (1 << 6)   /* 对象类型（键值对集合） */
#define cJSON_Raw    (1 << 7)   /* 原始 JSON（不进行转义处理） */

#define cJSON_IsReference 256        /* 附加标志：节点为"引用"（不拥有内存） */
#define cJSON_StringIsConst 512      /* 附加标志：字符串为常量（不可写） */

/* cJSON 核心结构体 ———————————————————————————————————————————
 *   整个 JSON 树由 cJSON 节点通过链表 + 树形指针连接而成。
 *   数组/对象用"双向链表"组织子节点，child 指向链表头。
 *
 *   典型结构示意（一个对象包含两个键值对）：
 *     root (Object) —> child —> "name": "Alice" (String)
 *                              <-> "age": 20 (Number)
 *                              <-> NULL（链表尾）                     */
typedef struct cJSON
{
    /* 双向链表：兄弟节点之间水平连接（同一数组/对象的元素链） */
    struct cJSON *next;
    struct cJSON *prev;
    /* 树形指针：指向第一个子节点（对象的值 / 数组的第一个元素）。
     *    子节点之间再通过 next/prev 形成链表。 */
    struct cJSON *child;

    /* 节点类型，值为上述 cJSON_xxx 标志位的组合 */
    int type;

    /* 节点为 cJSON_String 或 cJSON_Raw 时，指向字符串内容 */
    char *valuestring;
    /* 【已弃用】请使用 cJSON_SetNumberValue 宏同时设置 valueint 和 valuedouble */
    int valueint;
    /* 节点为 cJSON_Number 时，存储数值（double 精度） */
    double valuedouble;

    /* 节点为对象的子节点时，存储"键"（key）字符串 */
    char *string;
} cJSON;

/* 自定义内存管理钩子：允许调用者注入自己的 malloc/free 实现 */
typedef struct cJSON_Hooks
{
      /* malloc/free 在 Windows 上使用 __cdecl 调用约定，确保兼容编译器默认设置 */
      void *(CJSON_CDECL *malloc_fn)(size_t sz);
      void (CJSON_CDECL *free_fn)(void *ptr);
} cJSON_Hooks;

/* cJSON 内部使用的布尔类型（int 别名，0 = false，非 0 = true） */
typedef int cJSON_bool;

/* Limits how deeply nested arrays/objects can be before cJSON rejects to parse them.
 * This is to prevent stack overflows. */
#ifndef CJSON_NESTING_LIMIT
#define CJSON_NESTING_LIMIT 1000
#endif

/* Limits the length of circular references can be before cJSON rejects to parse them.
 * This is to prevent stack overflows. */
#ifndef CJSON_CIRCULAR_LIMIT
#define CJSON_CIRCULAR_LIMIT 10000
#endif

/* ================================================================
 *   一、版本 & 初始化
 * ================================================================ */

/* 返回 cJSON 版本号字符串（e.g. "1.7.19"） */
CJSON_PUBLIC(const char*) cJSON_Version(void);

/* 注入自定义 malloc / free 函数，覆盖默认的内存管理 */
CJSON_PUBLIC(void) cJSON_InitHooks(cJSON_Hooks* hooks);

/* ================================================================
 *   二、JSON 解析（字符串 → cJSON 树）
 *   调用者负责用 cJSON_Delete() 释放返回的 cJSON 树。
 * ================================================================ */

/* 解析以 '\0' 结尾的 JSON 字符串，返回根节点 */
CJSON_PUBLIC(cJSON *) cJSON_Parse(const char *value);
/* 解析已知长度（不依赖 '\0'）的 JSON 字符串 */
CJSON_PUBLIC(cJSON *) cJSON_ParseWithLength(const char *value, size_t buffer_length);
/* 解析并可选检查 '\0' 结尾；return_parse_end 输出解析结束位置（失败时即错误位置） */
CJSON_PUBLIC(cJSON *) cJSON_ParseWithOpts(const char *value, const char **return_parse_end, cJSON_bool require_null_terminated);
CJSON_PUBLIC(cJSON *) cJSON_ParseWithLengthOpts(const char *value, size_t buffer_length, const char **return_parse_end, cJSON_bool require_null_terminated);

/* ================================================================
 *   三、JSON 序列化（cJSON 树 → 字符串）
 *   cJSON_Print 系列返回的字符串需调用者用 free() / cJSON_free() 释放。
 *   cJSON_PrintPreallocated 使用预分配缓冲区，不额外分配内存。
 * ================================================================ */

/* 格式化的 JSON 字符串（带缩进和换行，便于阅读） */
CJSON_PUBLIC(char *) cJSON_Print(const cJSON *item);
/* 紧凑 JSON 字符串（无多余空白，体积最小） */
CJSON_PUBLIC(char *) cJSON_PrintUnformatted(const cJSON *item);
/* 缓冲式序列化：prebuffer 为预估大小，fmt=1 格式化 / 0 紧凑 */
CJSON_PUBLIC(char *) cJSON_PrintBuffered(const cJSON *item, int prebuffer, cJSON_bool fmt);
/* 写入预分配的缓冲区 buffer（length 字节）。成功返回 1，失败返回 0。
 *   注意：预估不准时建议多分配 5 字节余量。 */
CJSON_PUBLIC(cJSON_bool) cJSON_PrintPreallocated(cJSON *item, char *buffer, const int length, const cJSON_bool format);
/* 递归删除 cJSON 节点及其所有子节点 */
CJSON_PUBLIC(void) cJSON_Delete(cJSON *item);

/* ================================================================
 *   四、读取节点数据
 * ================================================================ */

/* 返回数组（或对象）的子元素个数 */
CJSON_PUBLIC(int) cJSON_GetArraySize(const cJSON *array);
/* 按索引获取数组元素（0-based），越界返回 NULL */
CJSON_PUBLIC(cJSON *) cJSON_GetArrayItem(const cJSON *array, int index);
/* 按键名获取对象成员（大小写不敏感） */
CJSON_PUBLIC(cJSON *) cJSON_GetObjectItem(const cJSON * const object, const char * const string);
/* 按键名获取对象成员（大小写敏感） */
CJSON_PUBLIC(cJSON *) cJSON_GetObjectItemCaseSensitive(const cJSON * const object, const char * const string);
/* 检查对象是否包含指定键 */
CJSON_PUBLIC(cJSON_bool) cJSON_HasObjectItem(const cJSON *object, const char *string);
/* 返回上一次解析失败的错误位置指针（成功时返回 0/NULL） */
CJSON_PUBLIC(const char *) cJSON_GetErrorPtr(void);

/* 从节点中提取值：字符串值 */
CJSON_PUBLIC(char *) cJSON_GetStringValue(const cJSON * const item);
/* 从节点中提取值：数值（double） */
CJSON_PUBLIC(double) cJSON_GetNumberValue(const cJSON * const item);

/* ================================================================
 *   五、类型判断（返回 cJSON_bool：非 0 = 是）
 * ================================================================ */

CJSON_PUBLIC(cJSON_bool) cJSON_IsInvalid(const cJSON * const item);
CJSON_PUBLIC(cJSON_bool) cJSON_IsFalse(const cJSON * const item);
CJSON_PUBLIC(cJSON_bool) cJSON_IsTrue(const cJSON * const item);
CJSON_PUBLIC(cJSON_bool) cJSON_IsBool(const cJSON * const item);      /* False 或 True */
CJSON_PUBLIC(cJSON_bool) cJSON_IsNull(const cJSON * const item);
CJSON_PUBLIC(cJSON_bool) cJSON_IsNumber(const cJSON * const item);
CJSON_PUBLIC(cJSON_bool) cJSON_IsString(const cJSON * const item);
CJSON_PUBLIC(cJSON_bool) cJSON_IsArray(const cJSON * const item);
CJSON_PUBLIC(cJSON_bool) cJSON_IsObject(const cJSON * const item);
CJSON_PUBLIC(cJSON_bool) cJSON_IsRaw(const cJSON * const item);

/* ================================================================
 *   六、创建节点
 * ================================================================ */

/* 创建各类型的基本节点（CreateXxx 返回新分配的节点，需用 cJSON_Delete 释放） */
CJSON_PUBLIC(cJSON *) cJSON_CreateNull(void);
CJSON_PUBLIC(cJSON *) cJSON_CreateTrue(void);
CJSON_PUBLIC(cJSON *) cJSON_CreateFalse(void);
CJSON_PUBLIC(cJSON *) cJSON_CreateBool(cJSON_bool boolean);
CJSON_PUBLIC(cJSON *) cJSON_CreateNumber(double num);
CJSON_PUBLIC(cJSON *) cJSON_CreateString(const char *string);
/* 创建 Raw 节点（valuestring 内容不转义） */
CJSON_PUBLIC(cJSON *) cJSON_CreateRaw(const char *raw);
/* 创建空数组 [] */
CJSON_PUBLIC(cJSON *) cJSON_CreateArray(void);
/* 创建空对象 {} */
CJSON_PUBLIC(cJSON *) cJSON_CreateObject(void);

/* 创建"引用"节点：valuestring 指向外部字符串，不会被 cJSON_Delete 释放 */
CJSON_PUBLIC(cJSON *) cJSON_CreateStringReference(const char *string);
/* 创建"引用"数组/对象：不持有子节点所有权 */
CJSON_PUBLIC(cJSON *) cJSON_CreateObjectReference(const cJSON *child);
CJSON_PUBLIC(cJSON *) cJSON_CreateArrayReference(const cJSON *child);

/* 批量创建数组：从 C 数组构造 JSON 数组（count 不能超过数组实际元素数） */
CJSON_PUBLIC(cJSON *) cJSON_CreateIntArray(const int *numbers, int count);
CJSON_PUBLIC(cJSON *) cJSON_CreateFloatArray(const float *numbers, int count);
CJSON_PUBLIC(cJSON *) cJSON_CreateDoubleArray(const double *numbers, int count);
CJSON_PUBLIC(cJSON *) cJSON_CreateStringArray(const char *const *strings, int count);

/* ================================================================
 *   七、操作数组 / 对象（增删改查）
 * ================================================================ */

/* 向数组尾部追加元素；向对象添加键值对 */
CJSON_PUBLIC(cJSON_bool) cJSON_AddItemToArray(cJSON *array, cJSON *item);
CJSON_PUBLIC(cJSON_bool) cJSON_AddItemToObject(cJSON *object, const char *string, cJSON *item);
/* 添加键值对（string 必须为常量，不会被内部释放）。
 *   警告：写入 item->string 前务必检查 (item->type & cJSON_StringIsConst) == 0 */
CJSON_PUBLIC(cJSON_bool) cJSON_AddItemToObjectCS(cJSON *object, const char *string, cJSON *item);
/* 添加"引用"：共享已有节点，不产生副本 */
CJSON_PUBLIC(cJSON_bool) cJSON_AddItemReferenceToArray(cJSON *array, cJSON *item);
CJSON_PUBLIC(cJSON_bool) cJSON_AddItemReferenceToObject(cJSON *object, const char *string, cJSON *item);

/* 移除/分离节点：Detach 返回被移除节点（调用者负责释放），Delete 直接释放 */
CJSON_PUBLIC(cJSON *) cJSON_DetachItemViaPointer(cJSON *parent, cJSON * const item);
CJSON_PUBLIC(cJSON *) cJSON_DetachItemFromArray(cJSON *array, int which);
CJSON_PUBLIC(void) cJSON_DeleteItemFromArray(cJSON *array, int which);
CJSON_PUBLIC(cJSON *) cJSON_DetachItemFromObject(cJSON *object, const char *string);
CJSON_PUBLIC(cJSON *) cJSON_DetachItemFromObjectCaseSensitive(cJSON *object, const char *string);
CJSON_PUBLIC(void) cJSON_DeleteItemFromObject(cJSON *object, const char *string);
CJSON_PUBLIC(void) cJSON_DeleteItemFromObjectCaseSensitive(cJSON *object, const char *string);

/* 更新/插入：Insert 在指定位置插入（后续元素右移），Replace 替换 */
CJSON_PUBLIC(cJSON_bool) cJSON_InsertItemInArray(cJSON *array, int which, cJSON *newitem);
CJSON_PUBLIC(cJSON_bool) cJSON_ReplaceItemViaPointer(cJSON * const parent, cJSON * const item, cJSON * replacement);
CJSON_PUBLIC(cJSON_bool) cJSON_ReplaceItemInArray(cJSON *array, int which, cJSON *newitem);
CJSON_PUBLIC(cJSON_bool) cJSON_ReplaceItemInObject(cJSON *object,const char *string,cJSON *newitem);
CJSON_PUBLIC(cJSON_bool) cJSON_ReplaceItemInObjectCaseSensitive(cJSON *object,const char *string,cJSON *newitem);

/* ================================================================
 *   八、高级操作
 * ================================================================ */

/* 深/浅复制：recurse != 0 时递归复制所有子节点。
 *   复制后 next/prev 始终为 NULL。 */
CJSON_PUBLIC(cJSON *) cJSON_Duplicate(const cJSON *item, cJSON_bool recurse);
/* 递归比较两棵 cJSON 树：case_sensitive=1 键名敏感，=0 不敏感。
 *   任一方为 NULL 或 Invalid 时返回不相等。 */
CJSON_PUBLIC(cJSON_bool) cJSON_Compare(const cJSON * const a, const cJSON * const b, const cJSON_bool case_sensitive);

/* 压缩 JSON 字符串（原地移除空格、制表、回车、换行）。
 *   输入必须是可读写的内存地址，不能是字符串常量。 */
CJSON_PUBLIC(void) cJSON_Minify(char *json);

/* ================================================================
 *   九、快捷函数：创建并同时添加到对象
 *   成功返回新节点，失败返回 NULL。
 * ================================================================ */

CJSON_PUBLIC(cJSON*) cJSON_AddNullToObject(cJSON * const object, const char * const name);
CJSON_PUBLIC(cJSON*) cJSON_AddTrueToObject(cJSON * const object, const char * const name);
CJSON_PUBLIC(cJSON*) cJSON_AddFalseToObject(cJSON * const object, const char * const name);
CJSON_PUBLIC(cJSON*) cJSON_AddBoolToObject(cJSON * const object, const char * const name, const cJSON_bool boolean);
CJSON_PUBLIC(cJSON*) cJSON_AddNumberToObject(cJSON * const object, const char * const name, const double number);
CJSON_PUBLIC(cJSON*) cJSON_AddStringToObject(cJSON * const object, const char * const name, const char * const string);
CJSON_PUBLIC(cJSON*) cJSON_AddRawToObject(cJSON * const object, const char * const name, const char * const raw);
CJSON_PUBLIC(cJSON*) cJSON_AddObjectToObject(cJSON * const object, const char * const name);
CJSON_PUBLIC(cJSON*) cJSON_AddArrayToObject(cJSON * const object, const char * const name);

/* ================================================================
 *   十、常用宏
 * ================================================================ */

/* 安全设置整数值：同时写入 valueint 和 valuedouble，保证两个字段同步。
 *   用法：cJSON_SetIntValue(node, 42);   // 替代 node->valueint = 42;        */
#define cJSON_SetIntValue(object, number) ((object) ? (object)->valueint = (object)->valuedouble = (number) : (number))
/* cJSON_SetNumberValue 的辅助函数（内部使用） */
CJSON_PUBLIC(double) cJSON_SetNumberHelper(cJSON *object, double number);
/* 安全设置浮点数值：直接调用辅助函数，避免宏展开副作用 */
#define cJSON_SetNumberValue(object, number) ((object != NULL) ? cJSON_SetNumberHelper(object, (double)number) : (number))
/* 修改 cJSON_String 节点的 valuestring（仅在 type == cJSON_String 时生效） */
CJSON_PUBLIC(char*) cJSON_SetValuestring(cJSON *object, const char *valuestring);

/* 安全设置布尔值：仅在节点类型为 False/True 时生效，否则返回 cJSON_Invalid */
#define cJSON_SetBoolValue(object, boolValue) ( \
    (object != NULL && ((object)->type & (cJSON_False|cJSON_True))) ? \
    (object)->type=((object)->type &(~(cJSON_False|cJSON_True)))|((boolValue)?cJSON_True:cJSON_False) : \
    cJSON_Invalid\
)

/* 遍历数组或对象的所有子元素（头文件包含后即可使用的 for 循环宏）。
 *   用法：
 *     cJSON *elem;
 *     cJSON_ArrayForEach(elem, array) {
 *         printf("item: %s\n", elem->string);
 *     }                                                                     */
#define cJSON_ArrayForEach(element, array) for(element = (array != NULL) ? (array)->child : NULL; element != NULL; element = element->next)

/* 通过已注册的钩子函数分配/释放内存 */
CJSON_PUBLIC(void *) cJSON_malloc(size_t size);
CJSON_PUBLIC(void) cJSON_free(void *object);

#ifdef __cplusplus
}
#endif

#endif /* cJSON__h */
