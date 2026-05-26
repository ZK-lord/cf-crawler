/*
 * htmlgen.c — HTML 报告生成模块
 *
 * 读取 HTML 模板文件，将 {{TITLE}} 和 {{DATA_JSON}} 替换为实际数据，
 * 生成最终的 HTML 报告。
 */

#include "htmlgen.h"
#include "json_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 替换字符串中所有出现的占位符 */
static char *replace_placeholder(const char *tmpl, const char *placeholder,
                                  const char *value) {
    size_t tmpl_len = strlen(tmpl);
    size_t ph_len = strlen(placeholder);
    size_t val_len = strlen(value);

    /* 先扫一遍统计出现次数 */
    int count = 0;
    const char *p = tmpl;
    while ((p = strstr(p, placeholder)) != NULL) {
        count++;
        p += ph_len;
    }

    if (count == 0) return strdup(tmpl);

    /* 根据次数计算输出缓冲区大小 */
    size_t out_len = tmpl_len + (size_t)count * (val_len - ph_len) + 1;
    char *out = (char *)malloc(out_len);
    if (!out) return NULL;

    /* 逐段复制，遇到占位符时插入新值 */
    char *dst = out;
    const char *src = tmpl;
    const char *next;

    while ((next = strstr(src, placeholder)) != NULL) {
        size_t seg_len = (size_t)(next - src);
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
 * 流程：读模板 → 序列化 JSON → 替换 {{TITLE}} → 替换 {{DATA_JSON}} → 写文件
 *
 * 参数：
 *   template_path — 模板路径
 *   output_path   — 输出路径
 *   title         — 页面标题
 *   data          — cJSON 数据
 */
int generate_html(const char *template_path, const char *output_path,
                  const char *title, cJSON *data) {

    char *tmpl = read_text_file(template_path);
    if (!tmpl) {
        fprintf(stderr, "  [ERROR] Cannot read template: %s\n", template_path);
        return -1;
    }

    /* 将 cJSON 数据序列化为紧凑 JSON 字符串 */
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

    /* 依次替换两个占位符 */
    char *after_title = replace_placeholder(tmpl, "{{TITLE}}", title);
    free(tmpl);
    if (!after_title) {
        free(json_str);
        return -1;
    }

    char *result = replace_placeholder(after_title, "{{DATA_JSON}}", json_str);
    free(after_title);
    free(json_str);
    if (!result) return -1;

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
 * 输出：
 *   output/index.html           — 多用户首页
 *   output/report_<handle>.html — 每个用户的详情页
 */
int generate_all_html(cJSON *users, const char *template_path,
                      const char *output_dir) {
    if (!users) return -1;

    /* 首页：CF_DATA 是整个 users 数组 */
    char index_path[512];
    snprintf(index_path, sizeof(index_path), "%s/index.html", output_dir);
    generate_html(template_path, index_path,
                  "Codeforces Analytics - Users", users);

    /* 每人一个详情页：CF_DATA 是单个 user 对象 */
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
