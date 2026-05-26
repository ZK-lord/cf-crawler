/*
 * htmlgen.c — HTML 报告生成模块
 *
 * ═══════════════════════════════════════════════════════════
 *   负责将分析好的 JSON 数据填入 HTML 模板，生成最终报告。
 * ═══════════════════════════════════════════════════════════
 *
 * 【工作原理】
 *   采用模板替换策略：HTML 模板中有两个占位符，
 *   本模块读取模板文件，把占位符替换为实际内容。
 *
 *   占位符和替换内容：
 *     {{TITLE}}     → 页面标题字符串
 *     {{DATA_JSON}} → 分析数据序列化后的 JSON 字符串
 *
 * 【模板示例】
 *   <html>
 *   <head><title>{{TITLE}}</title></head>
 *   <body>
 *     <script>
 *       var CF_DATA = {{DATA_JSON}};  ← 这里被替换为 JSON 数据
 *     </script>
 *   </body>
 *   </html>
 *
 * 【为什么用模板替换而不是直接拼字符串？】
 *   1. HTML 结构和数据分离，修改样式不需要改 C 代码
 *   2. ECharts 配置写在模板里，方便前端调试
 *   3. 模板可以独立在浏览器中预览（填入 mock 数据）
 *
 * 【生成的两种页面】
 *   1. index.html:
 *      - CF_DATA 是一个数组，包含所有用户的摘要数据
 *      - 前端用这个数组渲染用户列表和对比图表
 *
 *   2. report_<handle>.html:
 *      - CF_DATA 是一个对象，包含单个用户的完整数据
 *      - 前端用这个对象渲染 Rating 曲线、题目状态、直方图等
 */

#include "htmlgen.h"
#include "json_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 替换字符串中的占位符
 * ═══════════════════
 *
 * 【算法】
 *   在一个字符串中查找所有出现的占位符，替换为指定值。
 *   支持多次出现（一个模板中同一占位符可能出现在多个位置）。
 *
 * 【实现细节】
 *   1. 先扫描原文，统计占位符出现次数
 *   2. 根据次数计算输出缓冲区大小
 *      out_len = 原文长度 + 出现次数 × (新值长度 - 占位符长度) + 1
 *   3. 逐段复制原文，遇到占位符时插入新值
 *
 * 【为什么不直接用 str_replace？】
 *   标准 C 库没有 str_replace 函数。
 *   这里手动实现了一个高效的逐段复制算法，只需一次遍历。
 *
 * 参数：
 *   tmpl        — 模板字符串
 *   placeholder — 要查找的占位符（如 "{{TITLE}}"）
 *   value       — 替换后的内容
 * 返回：新分配的字符串（调用者负责 free），
 *       如果没有找到占位符则返回原字符串的拷贝。
 */
static char *replace_placeholder(const char *tmpl, const char *placeholder,
                                  const char *value) {
    size_t tmpl_len = strlen(tmpl);
    size_t ph_len = strlen(placeholder);
    size_t val_len = strlen(value);

    /* 第一遍扫描：统计占位符出现次数 */
    int count = 0;
    const char *p = tmpl;
    while ((p = strstr(p, placeholder)) != NULL) {
        count++;
        p += ph_len;  /* 跳过当前占位符，继续搜索下一个 */
    }

    /* 没有占位符 → 直接返回原字符串拷贝 */
    if (count == 0) return strdup(tmpl);

    /* 计算输出缓冲区大小并分配 */
    /* 公理：替换后长度 = 原长 + 次数 × (替换值长度 - 占位符长度)
       因为每个占位符被删掉（-ph_len）再插入新值（+val_len） */
    size_t out_len = tmpl_len + (size_t)count * (val_len - ph_len) + 1;
    char *out = (char *)malloc(out_len);
    if (!out) return NULL;

    /* 第二遍：逐段复制并替换 */
    char *dst = out;
    const char *src = tmpl;
    const char *next;

    while ((next = strstr(src, placeholder)) != NULL) {
        /* 复制从 src 到占位符之前的内容 */
        size_t seg_len = (size_t)(next - src);
        memcpy(dst, src, seg_len);
        dst += seg_len;

        /* 插入替换值 */
        memcpy(dst, value, val_len);
        dst += val_len;

        /* 跳过占位符 */
        src = next + ph_len;
    }

    /* 复制最后一段（最后一个占位符之后的内容） */
    strcpy(dst, src);

    return out;
}

/*
 * 生成单个 HTML 文件
 * ════════════════
 *
 * 【流程】
 *   template.html  ──读取──→ 字符串
 *                                │
 *   cJSON 数据 ──序列化──→ JSON 字符串
 *                                │
 *                  ┌─ 替换 {{TITLE}} ──┐
 *                  └─ 替换 {{DATA_JSON}} ──┘
 *                                │
 *                                ▼
 *                           写入 output 目录下的 HTML 文件
 *
 * 【数据流】
 *   输入: template_path + title + data
 *   输出: output_path 指向的 HTML 文件
 *
 * 参数：
 *   template_path — HTML 模板文件路径
 *   output_path   — 生成的 HTML 文件路径
 *   title         — 页面标题（显示在浏览器标签页上）
 *   data          — cJSON 数据对象或数组
 * 返回：成功 0，失败 -1
 */
int generate_html(const char *template_path, const char *output_path,
                  const char *title, cJSON *data) {

    /* ---- 第1步：读取模板文件 ---- */
    char *tmpl = read_text_file(template_path);
    if (!tmpl) {
        fprintf(stderr, "  [ERROR] Cannot read template: %s\n", template_path);
        return -1;
    }

    /* ---- 第2步：将 cJSON 数据序列化为紧凑 JSON 字符串 ----
     * 使用 cJSON_PrintUnformatted（不带缩进）以减小文件大小。
     * data 为 NULL 时输出 null。 */
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

    /* ---- 第3步：替换标题占位符 {{TITLE}} ---- */
    char *after_title = replace_placeholder(tmpl, "{{TITLE}}", title);
    free(tmpl);  /* 模板字符串已无用，释放 */
    if (!after_title) {
        free(json_str);
        return -1;
    }

    /* ---- 第4步：替换数据占位符 {{DATA_JSON}} ---- */
    char *result = replace_placeholder(after_title, "{{DATA_JSON}}", json_str);
    free(after_title);
    free(json_str);

    if (!result) return -1;

    /* ---- 第5步：写入输出文件 ---- */
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
 * ════════════════
 *
 * 【生成的文件】
 *   1. output/index.html
 *      — 多用户首页，展示所有被分析用户的概览
 *      — CF_DATA 为数组，包含每个用户的基本信息和统计摘要
 *
 *   2. output/report_<handle>.html （每个用户一份）
 *      — 用户详情页，展示：
 *        · Rating 变化曲线（ECharts 折线图）
 *        · 每场比赛的题目状态（绿/红/黄 色块）
 *        · AC 难度分布直方图（4 个时间窗口可切换）
 *        · 近 180 天活跃度数据
 *
 * 【为什么首页和详情页分开？】
 *   如果只有一个页面，加载多用户数据时页面会很重。
 *   首页只展示摘要，点击用户后再加载详情，响应更快。
 *
 * 参数：
 *   users         — 用户数据 cJSON 数组
 *   template_path — HTML 模板文件路径
 *   output_dir    — 输出目录
 * 返回：成功 0，失败 -1
 */
int generate_all_html(cJSON *users, const char *template_path,
                      const char *output_dir) {
    if (!users) return -1;

    /* 生成多用户首页：CF_DATA 是整个 users 数组 */
    char index_path[512];
    snprintf(index_path, sizeof(index_path), "%s/index.html", output_dir);
    generate_html(template_path, index_path,
                  "Codeforces Analytics - Users", users);

    /* 为每个用户生成独立详情页：CF_DATA 是单个 user 对象 */
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
