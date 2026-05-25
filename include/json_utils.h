/*
 * json_utils.h — JSON/文件工具模块接口
 *
 * 提供文本文件读写、JSON 解析、
 * 以及 CF API 响应数据提取等基础功能。
 */

#pragma once
#include "cJSON.h"

/* 读取文本文件全部内容（调用者 free） */
char *read_text_file(const char *path);

/* 将字符串写入文本文件 */
int write_text_file(const char *path, const char *content);

/* 读取并解析 JSON 文件（调用者 cJSON_Delete） */
cJSON *read_json_file(const char *path);

/* 将 cJSON 序列化为 JSON 文件 */
int write_json_file(const char *path, const cJSON *json);

/* 从 CF API 响应文件中提取 result 数组 */
cJSON *load_cf_result_array(const char *path);

/* 从 CF API 响应文件中提取 result 第一个元素 */
cJSON *load_cf_result_first(const char *path);

/* 创建目录（跨平台封装） */
int ensure_directory(const char *path);
