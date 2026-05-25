/*
 * htmlgen.c — HTML 报告生成模块
 *
 * 读取 HTML 模板文件，将 {{TITLE}} 和 {{DATA_JSON}}
 * 占位符替换为实际数据，生成最终的 HTML 报告。
 *
 * 生成两种页面：
 *   - index.html: 多用户列表页面（CF_DATA 为数组）
 *   - report_<handle>.html: 单个用户详情页面（CF_DATA 为对象）
 */

#include "htmlgen.h"
#include "json_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 替换字符串中的所有占位符
 *
 * 参数：
 *   tmpl        — 模板字符串
 *   placeholder — 要替换的占位符（如 "{{TITLE}}"）
 *   value       — 替换后的值
 * 返回：新分配的字符串，调用者负责 free()
 */
static char *replace_placeholder(const char *tmpl, const char *placeholder,
                                  const char *value) {
    size_t tmpl_len = strlen(tmpl);
    size_t ph_len = strlen(placeholder);
    size_t val_len = strlen(value);

    /* 统计占位符出现次数，确定输出缓冲区大小 */
    int count = 0;
    const char *p = tmpl;
    while ((p = strstr(p, placeholder)) != NULL) {
        count++;
        p += ph_len;
    }

    if (count == 0) return strdup(tmpl);

    /* 分配足够大的缓冲区 */
    size_t out_len = tmpl_len + count * (val_len - ph_len) + 1;
    char *out = (char *)malloc(out_len);
    if (!out) return NULL;

    /* 逐段复制并替换 */
    char *dst = out;
    const char *src = tmpl;
    const char *next;
    while ((next = strstr(src, placeholder)) != NULL) {
        size_t seg_len = next - src;
        memcpy(dst, src, seg_len);
        dst += seg_len;
        memcpy(dst, value, val_len);
        dst += val_len;
        src = next + ph_len;
    }
    strcpy(dst, src);

    return out;
}

/*
 * 生成单个 HTML 文件
 *
 * 流程：
 *   1. 读取模板文件
 *   2. 将数据序列化为 JSON 字符串
 *   3. 替换 {{TITLE}} 和 {{DATA_JSON}} 占位符
 *   4. 写入输出文件
 *
 * 参数：
 *   template_path — 模板文件路径
 *   output_path   — 输出文件路径
 *   title         — 页面标题（替换 {{TITLE}}）
 *   data          — cJSON 数据（替换 {{DATA_JSON}}）
 * 返回：成功返回 0，失败返回 -1
 */
int generate_html(const char *template_path, const char *output_path,
                  const char *title, cJSON *data) {
    /* 读取模板 */
    char *tmpl = read_text_file(template_path);
    if (!tmpl) {
        fprintf(stderr, "  [ERROR] Cannot read template: %s\n", template_path);
        return -1;
    }

    /* 将 cJSON 数据序列化为 JSON 字符串 */
    char *json_str = NULL;
    if (data) {
        json_str = cJSON_PrintUnformatted(data);
        if (!json_str) {
            fprintf(stderr, "  [ERROR] Cannot serialize JSON data\n");
            free(tmpl);
            return -1;
        }
    } else {
        json_str = strdup("null");
    }

    /* 替换标题占位符 */
    char *after_title = replace_placeholder(tmpl, "{{TITLE}}", title);
    free(tmpl);
    if (!after_title) {
        free(json_str);
        return -1;
    }

    /* 替换数据占位符 */
    char *result = replace_placeholder(after_title, "{{DATA_JSON}}", json_str);
    free(after_title);
    free(json_str);

    if (!result) return -1;

    /* 写入输出文件 */
    int ret = write_text_file(output_path, result);
    free(result);

    if (ret == 0) {
        printf("  Generated: %s\n", output_path);
    } else {
        fprintf(stderr, "  [ERROR] Failed to write: %s\n", output_path);
    }

    return ret;
}

/*
 * 生成所有 HTML 报告
 *
 * 生成一个 index.html（多用户列表页），
 * 以及每个用户独立的 report_<handle>.html（详情页）。
 *
 * 参数：
 *   users         — 用户数据数组（cJSON 数组）
 *   template_path — 模板文件路径
 *   output_dir    — 输出目录
 * 返回：成功返回 0，失败返回 -1
 */
int generate_all_html(cJSON *users, const char *template_path,
                      const char *output_dir) {
    if (!users) return -1;

    /* 生成多用户首页 index.html */
    char index_path[512];
    snprintf(index_path, sizeof(index_path), "%s/index.html", output_dir);
    generate_html(template_path, index_path,
                  "Codeforces Analytics - Users", users);

    /* 为每个用户生成独立的详情页 */
    cJSON *user;
    cJSON_ArrayForEach(user, users) {
        cJSON *handle = cJSON_GetObjectItem(user, "handle");
        if (!handle || !handle->valuestring) continue;

        char detail_path[512];
        snprintf(detail_path, sizeof(detail_path), "%s/report_%s.html",
                 output_dir, handle->valuestring);

        char page_title[256];
        snprintf(page_title, sizeof(page_title),
                 "Codeforces - %s", handle->valuestring);

        generate_html(template_path, detail_path, page_title, user);
    }

    return 0;
}
