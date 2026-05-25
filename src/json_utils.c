/*
 * json_utils.c — JSON 文件读写与 CF API 数据提取工具
 *
 * 提供文件 I/O 和 JSON 解析的辅助函数，
 * 以及从 CF API 标准响应格式（{"status":"OK","result":[...]}）
 * 中提取数据的便捷方法。
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
 * 读取文本文件全部内容
 * 返回：malloc 分配的字符串，调用者负责 free()
 * 失败返回 NULL
 */
char *read_text_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    char *content = (char *)malloc((size_t)len + 1);
    if (!content) {
        fclose(f);
        return NULL;
    }
    size_t read_len = fread(content, 1, (size_t)len, f);
    content[read_len] = '\0';
    fclose(f);
    return content;
}

/*
 * 写入文本文件
 * 返回：成功 0，失败 -1
 */
int write_text_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t len = strlen(content);
    size_t written = fwrite(content, 1, len, f);
    fclose(f);
    return (written == len) ? 0 : -1;
}

/*
 * 读取并解析 JSON 文件
 * 返回：cJSON 对象指针，调用者负责 cJSON_Delete()
 * 失败返回 NULL
 */
cJSON *read_json_file(const char *path) {
    char *text = read_text_file(path);
    if (!text) return NULL;
    cJSON *json = cJSON_Parse(text);
    free(text);
    return json;
}

/*
 * 将 cJSON 对象序列化为格式化的 JSON 字符串并写入文件
 * 返回：成功 0，失败 -1
 */
int write_json_file(const char *path, const cJSON *json) {
    char *text = cJSON_Print(json);
    if (!text) return -1;
    int ret = write_text_file(path, text);
    free(text);
    return ret;
}

/*
 * 从 CF API 响应文件中提取 result 数组
 *
 * CF API 标准响应格式：
 *   {"status":"OK","result":[...]}
 *
 * 返回提取的数组的深拷贝，调用者负责 cJSON_Delete()
 * 若 status 不为 "OK" 或 result 不是数组，返回 NULL
 */
cJSON *load_cf_result_array(const char *path) {
    cJSON *root = read_json_file(path);
    if (!root) return NULL;
    cJSON *status = cJSON_GetObjectItem(root, "status");
    if (!status || strcmp(status->valuestring, "OK") != 0) {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON *result = cJSON_GetObjectItem(root, "result");
    if (!result || !cJSON_IsArray(result)) {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON *ret = cJSON_Duplicate(result, 1);
    cJSON_Delete(root);
    return ret;
}

/*
 * 从 CF API 响应文件中提取 result 数组的第一个元素
 *
 * 用于 user.info 等返回单元素数组的端点。
 * 返回第一个元素的深拷贝，调用者负责 cJSON_Delete()
 */
cJSON *load_cf_result_first(const char *path) {
    cJSON *arr = load_cf_result_array(path);
    if (!arr) return NULL;
    cJSON *first = cJSON_GetArrayItem(arr, 0);
    cJSON *ret = NULL;
    if (first) ret = cJSON_Duplicate(first, 1);
    cJSON_Delete(arr);
    return ret;
}

/*
 * 创建目录（跨平台封装）
 * 返回：成功 0，失败 -1
 */
int ensure_directory(const char *path) {
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}
