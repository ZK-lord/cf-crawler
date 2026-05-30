/*
 * json_utils.c — JSON 文件读写与 CF API 数据提取工具
 *
 * 提供以下基础能力：
 *   1. 文本文件读写（read_text_file / write_text_file）
 *   2. JSON 文件解析与序列化（read_json_file / write_json_file）
 *   3. 从 CF API 标准响应中提取数据（load_cf_result_array / load_cf_result_first）
 *   4. 跨平台目录创建（ensure_directory）
 *
 * CF API 标准响应格式：
 *   {"status":"OK", "result": [...]}       — 大部分 API
 *   {"status":"OK", "result": [{...}]}     — user.info（单元素数组）
 */

#include "json_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#endif

/*
 * 读取文本文件的全部内容到字符串。
 *
 * 以二进制模式 ("rb") 打开文件，先获取文件大小，一次性
 * 分配对应大小的缓冲区并全部读入，最后补 '\0' 结尾。
 *
 * 参数：
 *   path — 文件路径
 *
 * 返回：
 *   malloc 分配的字符串（调用者负责 free），失败返回 NULL。
 */
char *read_text_file(const char *path) {
    /* 以二进制读模式打开文件 */
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    /* 获取文件大小：移到末尾 → ftell 获取位置 → 回到开头 */
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);

    /* 分配文件大小 + 1 字节的内存（+1 给结尾 '\0'） */
    char *content = (char *)malloc((size_t)len + 1);
    if (!content) {
        fclose(f);
        return NULL;
    }

    /* 一次性读取全部内容，fread 返回实际读取的字节数 */
    size_t read_len = fread(content, 1, (size_t)len, f);
    content[read_len] = '\0';  /* 确保 C 字符串以 '\0' 结尾 */
    fclose(f);
    return content;
}

/*
 * 将字符串内容写入文本文件。
 *
 * 以二进制写模式 ("wb") 打开文件，使用 fwrite 一次性写入，
 * 并校验实际写入的字节数是否与预期一致。
 *
 * 参数：
 *   path    — 文件路径
 *   content — 要写入的字符串
 *
 * 返回：
 *   成功 0，写入不完整或无法打开文件返回 -1。
 */
int write_text_file(const char *path, const char *content) {
    /* 以二进制写模式打开（覆盖已存在的文件） */
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    size_t len = strlen(content);
    size_t written = fwrite(content, 1, len, f);
    fclose(f);

    /* 校验：实际写入字节数必须等于字符串长度 */
    return (written == len) ? 0 : -1;
}

/*
 * 读取并解析 JSON 文件。
 *
 * 组合 read_text_file 和 cJSON_Parse 两步操作。
 *
 * 参数：
 *   path — JSON 文件路径
 *
 * 返回：
 *   cJSON 对象指针（调用者负责 cJSON_Delete），失败返回 NULL。
 */
cJSON *read_json_file(const char *path) {
    /* 先读文本内容 */
    char *text = read_text_file(path);
    if (!text) return NULL;

    /* 解析 JSON 文本，构建 cJSON 树 */
    cJSON *json = cJSON_Parse(text);
    free(text);  /* 文本内容已解析为树结构，释放原始字符串 */
    return json;
}

/*
 * 将 cJSON 对象序列化为格式化 JSON 字符串并写入文件。
 *
 * 组合 cJSON_Print 和 write_text_file 两步操作。
 * cJSON_Print 输出带缩进和换行的人类可读 JSON。
 *
 * 参数：
 *   path — 输出文件路径
 *   json — 要序列化的 cJSON 对象
 *
 * 返回：
 *   成功 0，失败 -1。
 */
int write_json_file(const char *path, const cJSON *json) {
    /* 序列化为格式化的 JSON 字符串 */
    char *text = cJSON_Print(json);
    if (!text) return -1;

    /* 写入文件 */
    int ret = write_text_file(path, text);
    free(text);  /* cJSON_Print 分配的字符串 */
    return ret;
}

/*
 * 从 CF API 响应文件中提取 result 数组。
 *
 * CF API 统一响应格式：
 *   {"status":"OK", "result": [...]}
 *
 * 本函数解析文件 → 校验 status=="OK" → 检查 result 为数组 →
 * 返回 result 的深拷贝。
 *
 * 参数：
 *   path — 缓存 JSON 文件路径
 *
 * 返回：
 *   result 数组的深拷贝（调用者负责 cJSON_Delete），
 *   status 不为 "OK"、result 非数组或文件不存在时返回 NULL。
 */
cJSON *load_cf_result_array(const char *path) {
    /* 读取并解析 JSON 文件 */
    cJSON *root = read_json_file(path);
    if (!root) return NULL;

    /* 检查顶层 status 字段，必须为 "OK" */
    cJSON *status = cJSON_GetObjectItem(root, "status");
    if (!status || strcmp(status->valuestring, "OK") != 0) {
        cJSON_Delete(root);
        return NULL;
    }

    /* 检查 result 字段，必须为数组类型 */
    cJSON *result = cJSON_GetObjectItem(root, "result");
    if (!result || !cJSON_IsArray(result)) {
        cJSON_Delete(root);
        return NULL;
    }

    /* 深拷贝 result 数组（recurse=1 表示递归复制所有子节点）。
       深拷贝后释放原始 root，调用者拥有返回值的所有权 */
    cJSON *ret = cJSON_Duplicate(result, 1);
    cJSON_Delete(root);
    return ret;
}

/*
 * 从 CF API 响应文件中提取 result 数组的第一个元素。
 *
 * 用于 user.info 等返回单元素数组的 API 端点。
 * CF user.info 响应格式：
 *   {"status":"OK", "result": [{handle:..., rating:..., ...}]}
 *
 * 参数：
 *   path — 缓存 JSON 文件路径
 *
 * 返回：
 *   result[0] 的深拷贝（调用者负责 cJSON_Delete），失败返回 NULL。
 */
cJSON *load_cf_result_first(const char *path) {
    /* 先提取整个 result 数组 */
    cJSON *arr = load_cf_result_array(path);
    if (!arr) return NULL;

    /* 取数组的第一个元素（索引 0） */
    cJSON *first = cJSON_GetArrayItem(arr, 0);

    /* 深拷贝第一个元素 */
    cJSON *ret = NULL;
    if (first) ret = cJSON_Duplicate(first, 1);

    /* 释放临时数组，返回深拷贝结果 */
    cJSON_Delete(arr);
    return ret;
}

/*
 * 创建目录（跨平台封装）。
 *
 * Windows 使用 _mkdir（单参数），POSIX 使用 mkdir（双参数）。
 *
 * 参数：
 *   path — 目录路径
 *
 * 返回：
 *   成功 0，失败 -1。
 */
int ensure_directory(const char *path) {
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}
