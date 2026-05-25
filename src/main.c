/*
 * main.c — 程序入口与流程编排
 *
 * 负责：
 *   1. 读取用户列表文件（sample_users.txt）
 *   2. 调用 CF API 获取每个用户的原始数据
 *   3. 调用 analyzer 模块进行数据分析
 *   4. 调用 htmlgen 模块生成可视化 HTML 报告
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "rate_limiter.h"
#include "json_utils.h"
#include "cf_api.h"
#include "analyzer.h"
#include "htmlgen.h"

#define MAX_HANDLE_LEN 64   /* Codeforces 用户名最大长度 */
#define MAX_HANDLES 256     /* 一次最多处理的用户数 */
#define DATA_DIR "data"     /* 原始 API 数据缓存目录 */
#define OUTPUT_DIR "output" /* HTML 报告输出目录 */
#define TEMPLATE_PATH "web/template.html" /* HTML 模板路径 */

#ifdef _WIN32
#include <direct.h>
#define mkdir_p(path) _mkdir(path)
#else
#define mkdir_p(path) mkdir(path, 0755)
#endif

/*
 * 从文本文件中读取 Codeforces 用户名列表
 * 每行一个用户名，自动去除首尾空白字符
 * 返回：成功读取的用户数，失败返回 -1
 */
static int read_handles(const char *path, char handles[][MAX_HANDLE_LEN]) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Cannot open handle list: %s\n", path);
        return -1;
    }

    int count = 0;
    char buf[256];
    while (fgets(buf, sizeof(buf), f) && count < MAX_HANDLES) {
        char *s = buf;
        while (*s == ' ' || *s == '\t' || *s == '\r') s++;
        int len = (int)strlen(s);
        while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r' || s[len-1] == ' ' || s[len-1] == '\t'))
            s[--len] = '\0';
        if (len > 0 && len < MAX_HANDLE_LEN) {
            memmove(handles[count], s, (size_t)len + 1);
            count++;
        }
    }
    fclose(f);
    return count;
}

int main(int argc, char *argv[]) {
    /* 默认读取 sample_users.txt，可通过命令行参数指定其他文件 */
    const char *handle_file = "sample_users.txt";
    if (argc > 1) handle_file = argv[1];

    printf("=== Codeforces Crawler 2025 ===\n");
    printf("Handle list: %s\n\n", handle_file);

    /* 步骤 1: 读取用户列表 */
    char handles[MAX_HANDLES][MAX_HANDLE_LEN] = {{0}};
    int handle_count = read_handles(handle_file, handles);
    if (handle_count <= 0) {
        fprintf(stderr, "No valid handles found in %s\n", handle_file);
        return 1;
    }
    printf("Found %d handle(s)\n\n", handle_count);

    /* 创建数据缓存和输出目录 */
    mkdir_p(DATA_DIR);
    mkdir_p(OUTPUT_DIR);

    /* 初始化 CF API 限速器（2 秒间隔） */
    RateLimiter rl;
    rate_limiter_init(&rl);

    /* 步骤 2: 获取比赛列表（所有用户共享，只获取一次） */
    printf("[1/5] Fetching contest list...\n");
    if (cf_fetch_contest_list(DATA_DIR "/contest_list.json", &rl) != 0) {
        fprintf(stderr, "Failed to fetch contest list\n");
    }

    /* 步骤 3: 逐个用户获取数据并分析 */
    cJSON *users_arr = cJSON_CreateArray();
    if (!users_arr) {
        fprintf(stderr, "Memory error\n");
        return 1;
    }

    for (int i = 0; i < handle_count; i++) {
        const char *handle = handles[i];
        printf("\n--- Processing: %s (%d/%d) ---\n", handle, i + 1, handle_count);

        char path[1024];

        /* 3a: 获取用户基本信息（rating、头像、头衔等） */
        printf("[1/3] Fetching user info...\n");
        snprintf(path, sizeof(path), "%s/%s_user.json", DATA_DIR, handle);
        if (cf_fetch_user_info(handle, path, &rl) != 0) {
            fprintf(stderr, "  Skipping %s (failed to fetch user info)\n", handle);
            continue;
        }

        /* 3b: 获取 rating 变化历史（参赛记录） */
        printf("[2/3] Fetching rating history...\n");
        snprintf(path, sizeof(path), "%s/%s_rating.json", DATA_DIR, handle);
        if (cf_fetch_user_rating(handle, path, &rl) != 0) {
            fprintf(stderr, "  Skipping %s (failed to fetch rating history)\n", handle);
            continue;
        }

        /* 3c: 获取所有提交记录（用于分析每题状态和补题情况） */
        printf("[3/3] Fetching submissions...\n");
        snprintf(path, sizeof(path), "%s/%s_status.json", DATA_DIR, handle);
        if (cf_fetch_user_status(handle, path, &rl) != 0) {
            fprintf(stderr, "  Warning: failed to fetch submissions for %s\n", handle);
        }

        /* 3d: 综合分析，生成该用户的统计数据 JSON */
        printf("  Analyzing data...\n");
        cJSON *summary = build_user_summary(handle, DATA_DIR, OUTPUT_DIR, 0);
        if (summary) {
            cJSON_AddItemToArray(users_arr, summary);
            printf("  Analysis complete.\n");
        } else {
            fprintf(stderr, "  Failed to analyze data for %s\n", handle);
        }
    }

    /* 步骤 4: 将所有数据渲染为 HTML 报告 */
    printf("\n=== Generating HTML reports ===\n");
    if (cJSON_GetArraySize(users_arr) > 0) {
        if (generate_all_html(users_arr, TEMPLATE_PATH, OUTPUT_DIR) == 0) {
            printf("\nDone! Open output/index.html in your browser.\n");
        } else {
            fprintf(stderr, "HTML generation failed.\n");
        }
    } else {
        fprintf(stderr, "No user data to generate.\n");
    }

    cJSON_Delete(users_arr);
    return 0;
}
