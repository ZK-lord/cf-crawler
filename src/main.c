/*
 * main.c — 程序入口与流程编排
 *
 * 数据处理管线：
 *   sample_users.txt → CF API → data/ 缓存 → analyzer → htmlgen → output/ 报告
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

#define MAX_HANDLE_LEN 64
#define MAX_HANDLES 256
#define DATA_DIR "data"
#define OUTPUT_DIR "output"
#define TEMPLATE_PATH "web/template.html"

#ifdef _WIN32
#include <direct.h>
#define mkdir_p(path) _mkdir(path)
#else
#define mkdir_p(path) mkdir(path, 0755)
#endif

/* 从文本文件读取 CF 用户名，每行一个，自动去首尾空白，返回读取数量 */
static int read_handles(const char *path, char handles[][MAX_HANDLE_LEN]) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Cannot open handle list: %s\n", path);
        return -1;
    }

    int count = 0;
    char buf[256];
    while (fgets(buf, sizeof(buf), f) && count < MAX_HANDLES) {
        /* 去行首空白 */
        char *s = buf;
        while (*s == ' ' || *s == '\t' || *s == '\r') s++;

        /* 去行尾换行符和空白 */
        int len = (int)strlen(s);
        while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r'
                        || s[len-1] == ' '  || s[len-1] == '\t'))
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
    /* 命令行参数可指定自定义用户列表文件 */
    const char *handle_file = "sample_users.txt";
    if (argc > 1) handle_file = argv[1];

    printf("=== Codeforces Crawler 2025 ===\n");
    printf("Handle list: %s\n\n", handle_file);

    /* 步骤1: 读取用户列表 */
    char handles[MAX_HANDLES][MAX_HANDLE_LEN] = {{0}};
    int handle_count = read_handles(handle_file, handles);
    if (handle_count <= 0) {
        fprintf(stderr, "No valid handles found in %s\n", handle_file);
        return 1;
    }
    printf("Found %d handle(s)\n\n", handle_count);

    /* 创建输出目录 */
    mkdir_p(DATA_DIR);
    mkdir_p(OUTPUT_DIR);

    /* 初始化 API 限速器，2秒间隔防止被封 IP */
    RateLimiter rl;
    rate_limiter_init(&rl);

    /* 步骤2: 获取比赛列表，所有用户共享只需获取一次 */
    printf("[1/5] Fetching contest list...\n");
    if (cf_fetch_contest_list(DATA_DIR "/contest_list.json", &rl) != 0) {
        fprintf(stderr, "Failed to fetch contest list\n");
    }

    /* 所有用户的汇总数据数组 */
    cJSON *users_arr = cJSON_CreateArray();
    if (!users_arr) {
        fprintf(stderr, "Memory error\n");
        return 1;
    }

    /* 步骤3: 逐个用户获取数据并分析 */
    for (int i = 0; i < handle_count; i++) {
        const char *handle = handles[i];
        printf("\n--- Processing: %s (%d/%d) ---\n", handle, i + 1, handle_count);

        char path[1024];

        /* 3a: 获取用户基本信息，失败则跳过该用户 */
        printf("[1/3] Fetching user info...\n");
        snprintf(path, sizeof(path), "%s/%s_user.json", DATA_DIR, handle);
        if (cf_fetch_user_info(handle, path, &rl) != 0) {
            fprintf(stderr, "  Skipping %s (failed to fetch user info)\n", handle);
            continue;
        }

        /* 3b: 获取 rating 变化历史 */
        printf("[2/3] Fetching rating history...\n");
        snprintf(path, sizeof(path), "%s/%s_rating.json", DATA_DIR, handle);
        if (cf_fetch_user_rating(handle, path, &rl) != 0) {
            fprintf(stderr, "  Skipping %s (failed to fetch rating history)\n", handle);
            continue;
        }

        /* 3c: 获取提交记录，失败不阻塞，继续处理 */
        printf("[3/3] Fetching submissions...\n");
        snprintf(path, sizeof(path), "%s/%s_status.json", DATA_DIR, handle);
        if (cf_fetch_user_status(handle, path, &rl) != 0) {
            fprintf(stderr, "  Warning: failed to fetch submissions for %s\n", handle);
        }

        /* 3d: 分析数据，生成该用户的统计 JSON */
        printf("  Analyzing data...\n");
        cJSON *summary = build_user_summary(handle, DATA_DIR, OUTPUT_DIR, 0);
        if (summary) {
            cJSON_AddItemToArray(users_arr, summary);
            printf("  Analysis complete.\n");
        } else {
            fprintf(stderr, "  Failed to analyze data for %s\n", handle);
        }
    }

    /* 步骤4: 将所有数据渲染为 HTML 报告
       - index.html           多用户首页
       - report_<handle>.html 每个用户的详情页 */
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
