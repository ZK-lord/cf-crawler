/*
 * htmlgen.c — HTML 报告生成模块
 *
 * 通过模板替换的方式生成 HTML 文件。
 * 读取 template.html，将其中的占位符替换为实际数据后输出。
 *
 * 两个占位符：
 *   {{TITLE}}     → 页面标题
 *   {{DATA_JSON}} → 序列化为 JSON 的分析数据，供前端 ECharts 使用
 */
#include "htmlgen.h"
#include "json_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 将模板字符串中所有出现的指定占位符替换为真实值。
 *
 * 采用两遍扫描策略：
 *   第一遍：扫描模板，统计占位符出现次数；
 *   第二遍：根据次数精确计算输出缓冲区大小，逐段复制并替换。
 *
 * 参数：
 *   tmpl        — 模板字符串
 *   placeholder — 要替换的占位符（如 "{{TITLE}}"）
 *   value       — 替换后的内容
 *
 * 返回：
 *   malloc 分配的新字符串（调用者负责 free），失败返回 NULL。
 *   如果占位符不存在，返回模板的副本（通过 strdup）。
 */
static char *replace_placeholder(const char *tmpl, const char *placeholder,
                                  const char *value) {
    /* 获取三个字符串的长度，用于后续缓冲区大小计算 */
    size_t tmpl_len = strlen(tmpl);
    size_t ph_len = strlen(placeholder);
    size_t val_len = strlen(value);

    /* —— 第一遍扫描：统计占位符出现次数 —— */
    int count = 0;
    const char *p = tmpl;
    while ((p = strstr(p, placeholder)) != NULL) {
        count++;
        p += ph_len;  /* 跳过本次匹配的占位符，继续向后搜索 */
    }

    /* 没有找到占位符，直接返回模板的副本 */
    if (count == 0) return strdup(tmpl);

    /* —— 计算输出缓冲区大小 ——
       out_len = 原模板长度 + 次数 × (新值长度 - 占位符长度) + '\0' */
    size_t out_len = tmpl_len + (size_t)count * (val_len - ph_len) + 1;
    char *out = (char *)malloc(out_len);
    if (!out) return NULL;

    /* —— 第二遍扫描：逐段复制并替换 —— */
    char *dst = out;         /* 写指针，指向当前输出位置 */
    const char *src = tmpl;  /* 读指针，指向模板中待处理的位置 */
    const char *next;        /* 下一次占位符出现的位置 */

    while ((next = strstr(src, placeholder)) != NULL) {
        /* 复制占位符之前的内容 */
        size_t seg_len = (size_t)(next - src);
        memcpy(dst, src, seg_len);
        dst += seg_len;

        /* 复制替换值（跳过占位符本身） */
        memcpy(dst, value, val_len);
        dst += val_len;

        /* 源指针越过本次匹配的占位符 */
        src = next + ph_len;
    }
    /* 复制最后一段（最后一个占位符之后的内容，含 '\0'） */
    strcpy(dst, src);

    return out;
}

/*
 * 生成单个 HTML 文件。
 *
 * 完整流程：
 *   1. 读取模板文件内容
 *   2. 将 cJSON 数据序列化为紧凑 JSON 字符串
 *   3. 替换 {{TITLE}} 占位符
 *   4. 替换 {{DATA_JSON}} 占位符
 *   5. 写入输出文件
 *
 * 参数：
 *   template_path — HTML 模板路径（如 "web/template.html"）
 *   output_path   — 输出 HTML 文件路径
 *   title         — 页面标题（替换 {{TITLE}}）
 *   data          — cJSON 数据（序列化后替换 {{DATA_JSON}}）
 *
 * 返回：
 *   成功 0，失败 -1。
 */
int generate_html(const char *template_path, const char *output_path,
                  const char *title, cJSON *data) {

    /* 步骤 1：读取模板文件 */
    char *tmpl = read_text_file(template_path);
    if (!tmpl) {
        fprintf(stderr, "  [ERROR] Cannot read template: %s\n", template_path);
        return -1;
    }

    /* 步骤 2：将 cJSON 数据序列化为紧凑 JSON 字符串。
       使用 cJSON_PrintUnformatted 而非 cJSON_Print，
       输出不带缩进和换行，减少嵌入 HTML 后的体积 */
    char *json_str = NULL;
    if (data) {
        json_str = cJSON_PrintUnformatted(data);
        if (!json_str) {
            fprintf(stderr, "  [ERROR] Cannot serialize JSON data\n");
            free(tmpl);
            return -1;
        }
    } else {
        /* 数据为空时写入 "null"，前端 JS 可正常检测 */
        json_str = strdup("null");
    }

    /* 步骤 3：替换 {{TITLE}} 占位符 */
    char *after_title = replace_placeholder(tmpl, "{{TITLE}}", title);
    free(tmpl);  /* 模板原始内容已不需要 */
    if (!after_title) {
        free(json_str);
        return -1;
    }

    /* 步骤 4：在已替换标题的结果上继续替换 {{DATA_JSON}} 占位符 */
    char *result = replace_placeholder(after_title, "{{DATA_JSON}}", json_str);
    free(after_title);
    free(json_str);
    if (!result) return -1;

    /* 步骤 5：写入输出文件 */
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
 * 生成所有 HTML 报告（多用户批量生成）。
 *
 * 调用 generate_html 两次：
 *   1. 用整个 users 数组生成 index.html（多用户总览页）
 *   2. 对每个用户单独生成 report_<handle>.html（个人详情页）
 *
 * 参数：
 *   users         — 所有用户的汇总数据（cJSON 数组）
 *   template_path — HTML 模板路径
 *   output_dir    — 输出目录
 *
 * 返回：
 *   成功 0，失败 -1。
 */
int generate_all_html(cJSON *users, const char *template_path,
                      const char *output_dir) {
    if (!users) return -1;

    /* —— 生成首页：CF_DATA 是整个 users 数组 ——
       前端检测到 CF_DATA 是数组时，进入"多用户汇总模式"，
       渲染用户列表和聚合图表 */
    char index_path[512];
    snprintf(index_path, sizeof(index_path), "%s/index.html", output_dir);
    generate_html(template_path, index_path,
                  "Codeforces Analytics - Users", users);

    /* —— 为每个用户生成独立详情页 ——
       前端检测到 CF_DATA 是对象时，进入"单用户详细模式"，
       渲染个人 rating 曲线、AC 分布、比赛记录等 */
    cJSON *user;
    cJSON_ArrayForEach(user, users) {
        /* 从用户数据中提取 handle，用作文件名的一部分 */
        cJSON *handle = cJSON_GetObjectItem(user, "handle");
        if (!handle || !handle->valuestring) continue;

        /* 构造输出路径：output/report_<handle>.html */
        char detail_path[512];
        snprintf(detail_path, sizeof(detail_path), "%s/report_%s.html",
                 output_dir, handle->valuestring);

        /* 构造页面标题：Codeforces - <handle> */
        char page_title[256];
        snprintf(page_title, sizeof(page_title),
                 "Codeforces - %s", handle->valuestring);

        /* 生成该用户的独立详情页 */
        generate_html(template_path, detail_path, page_title, user);
    }

    return 0;
}
