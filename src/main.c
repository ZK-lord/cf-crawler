/*
 * main.c — 程序入口与流程编排
 *
 * 整体数据处理管线：
 *   sample_users.txt → CF API → data/ 缓存 → analyzer → htmlgen → output/ 报告
 *
 * 调用方式：
 *   ./cf_crawler [用户列表文件]
 *   不传参数默认读取 sample_users.txt
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

/* ——— 全局常量定义 ——— */
#define MAX_HANDLE_LEN 64     /* 单个用户名的最大字符数 */
#define MAX_HANDLES 256       /* 最多支持的用户数量 */
#define DATA_DIR "data"       /* 缓存数据目录 */
#define OUTPUT_DIR "output"   /* HTML 输出目录 */
#define TEMPLATE_PATH "web/template.html"  /* HTML 模板位置 */

/* ——— 跨平台目录创建宏 ———
   Windows 使用 _mkdir（单参数），POSIX 使用 mkdir（双参数，含权限） */
#ifdef _WIN32
#include <direct.h>
#define mkdir_p(path) _mkdir(path)
#else
#define mkdir_p(path) mkdir(path, 0755)
#endif

/*
 * 从文本文件按行读取 CF 用户名列表。
 *
 * 每行一个用户名，自动处理：
 *   - 去除行首空白字符（空格、制表、回车）
 *   - 去除行尾换行符和空白字符
 *   - 跳过空行
 *   - 超过 MAX_HANDLE_LEN 的超长行被截断
 *
 * 参数：
 *   path    — 用户名列表文件路径
 *   handles — 输出参数，二维字符数组，每行为一个用户名
 *
 * 返回：
 *   成功读取的用户数量，文件无法打开时返回 -1。
 */
static int read_handles(const char *path, char handles[][MAX_HANDLE_LEN]) {
    /* 以文本模式打开用户列表文件 */
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Cannot open handle list: %s\n", path);
        return -1;
    }

    int count = 0;
    char buf[256];
    /* 逐行读取，直到文件尾或达到最大用户数上限 */
    while (fgets(buf, sizeof(buf), f) && count < MAX_HANDLES) {
        /* 跳过行首空白字符（空格、制表、回车） */
        char *s = buf;
        while (*s == ' ' || *s == '\t' || *s == '\r') s++;

        /* 从行尾向前删除换行符和空白字符 */
        int len = (int)strlen(s);
        while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r'
                        || s[len-1] == ' '  || s[len-1] == '\t'))
            s[--len] = '\0';

        /* 非空行且长度合法则存入 handles 数组 */
        if (len > 0 && len < MAX_HANDLE_LEN) {
            memmove(handles[count], s, (size_t)len + 1);
            count++;
        }
    }
    fclose(f);
    return count;
}

/*
 * 程序主入口。
 *
 * 执行流程（6 步）：
 *   1. 读取用户列表文件
 *   2. 创建输出目录
 *   3. 初始化限速器并拉取全局比赛列表
 *   4. 逐个用户：拉取数据 → 分析 → 汇总
 *   5. 生成 HTML 报告
 *   6. 清理资源并退出
 */
int main(int argc, char *argv[]) {
    /* 命令行参数：第一个参数可指定自定义的用户列表文件路径。
       不传则默认使用 sample_users.txt */
    const char *handle_file = "sample_users.txt";
    if (argc > 1) handle_file = argv[1];

    printf("=== Codeforces Crawler 2025 ===\n");
    printf("Handle list: %s\n\n", handle_file);

    /* ================================================================
     *   步骤 1：读取用户列表
     * ================================================================ */
    /* 在栈上分配 handles 二维数组并初始化为全零。
       {{0}} 确保所有字节初始化为 0 */
    char handles[MAX_HANDLES][MAX_HANDLE_LEN] = {{0}};
    int handle_count = read_handles(handle_file, handles);
    if (handle_count <= 0) {
        fprintf(stderr, "No valid handles found in %s\n", handle_file);
        return 1;
    }
    printf("Found %d handle(s)\n\n", handle_count);

    /* 创建 data/ 和 output/ 两个必要目录。
       data/ 用于缓存 API 响应，output/ 用于存放生成的 HTML */
    mkdir_p(DATA_DIR);
    mkdir_p(OUTPUT_DIR);

    /* ================================================================
     *   步骤 2：初始化限速器并获取比赛列表
     *   比赛列表是所有用户共享的全局数据，只需获取一次。
     * ================================================================ */
    /* 在栈上创建 RateLimiter 实例并初始化 */
    RateLimiter rl;
    rate_limiter_init(&rl);

    /* 获取全部非 gym 比赛列表，缓存到 data/contest_list.json。
       即使获取失败也不阻塞后续处理（仅影响赛时/补题判定的精度） */
    printf("[1/5] Fetching contest list...\n");
    if (cf_fetch_contest_list(DATA_DIR "/contest_list.json", &rl) != 0) {
        fprintf(stderr, "Failed to fetch contest list\n");
    }

    /* ================================================================
     *   步骤 3：创建汇总数组，后续每个用户的分析结果都存入此数组
     * ================================================================ */
    /* 创建一个空的 cJSON 数组，用于存放所有用户的 summary */
    cJSON *users_arr = cJSON_CreateArray();
    if (!users_arr) {
        fprintf(stderr, "Memory error\n");
        return 1;
    }

    /* ================================================================
     *   步骤 4：循环处理每个用户
     *   对每个用户依次：拉取信息 → 拉取 rating → 拉取提交 → 分析
     * ================================================================ */
    for (int i = 0; i < handle_count; i++) {
        const char *handle = handles[i];
        printf("\n--- Processing: %s (%d/%d) ---\n", handle, i + 1, handle_count);

        char path[1024];  /* 复用此缓冲区构建文件路径 */

        /* 4a：获取用户基本信息（必需）。
           调用 cf_fetch_user_info → fetch_and_save → cf_http_get
           → 校验 status=="OK" → 写入 data/<handle>_user.json */
        printf("[1/3] Fetching user info...\n");
        snprintf(path, sizeof(path), "%s/%s_user.json", DATA_DIR, handle);
        if (cf_fetch_user_info(handle, path, &rl) != 0) {
            /* 无法获取用户信息（不存在或被封禁），跳过该用户 */
            fprintf(stderr, "  Skipping %s (failed to fetch user info)\n", handle);
            continue;
        }

        /* 4b：获取 rating 变化历史（必需）。
           写入 data/<handle>_rating.json */
        printf("[2/3] Fetching rating history...\n");
        snprintf(path, sizeof(path), "%s/%s_rating.json", DATA_DIR, handle);
        if (cf_fetch_user_rating(handle, path, &rl) != 0) {
            /* rating 历史无法获取，跳过该用户 */
            fprintf(stderr, "  Skipping %s (failed to fetch rating history)\n", handle);
            continue;
        }

        /* 4c：获取全部提交记录（可选）。
           该端点返回数据量可能很大（数千条），失败时仅警告不阻塞。
           写入 data/<handle>_status.json */
        printf("[3/3] Fetching submissions...\n");
        snprintf(path, sizeof(path), "%s/%s_status.json", DATA_DIR, handle);
        if (cf_fetch_user_status(handle, path, &rl) != 0) {
            fprintf(stderr, "  Warning: failed to fetch submissions for %s\n", handle);
            /* 继续处理，analyzer 会在没有 status 数据时跳过直方图统计 */
        }

        /* 4d：调用 analyzer 模块对已缓存的数据进行综合分析。
          build_user_summary 内部会执行：
            - 读取 4 个缓存文件
            - 修正 rank 字段
            - 遍历每场比赛进行赛时/补题判定
            - 统计 AC 难度分布直方图（4 个时间窗口） */
        printf("  Analyzing data...\n");
        cJSON *summary = build_user_summary(handle, DATA_DIR, OUTPUT_DIR, 0);
        if (summary) {
            /* 将分析结果添加到 users_arr 数组 */
            cJSON_AddItemToArray(users_arr, summary);
            printf("  Analysis complete.\n");
        } else {
            fprintf(stderr, "  Failed to analyze data for %s\n", handle);
        }
    }

    /* ================================================================
     *   步骤 5：生成 HTML 报告
     *   generate_all_html 会为每个用户生成独立页面 + 一个总索引页
     * ================================================================ */
    printf("\n=== Generating HTML reports ===\n");
    if (cJSON_GetArraySize(users_arr) > 0) {
        /* 至少有一个用户分析成功，开始生成 HTML */
        if (generate_all_html(users_arr, TEMPLATE_PATH, OUTPUT_DIR) == 0) {
            printf("\nDone! Open output/index.html in your browser.\n");
        } else {
            fprintf(stderr, "HTML generation failed.\n");
        }
    } else {
        fprintf(stderr, "No user data to generate.\n");
    }

    /* ================================================================
     *   步骤 6：清理资源
     *   users_arr 包含所有用户的 summary（cJSON 树），统一释放
     * ================================================================ */
    cJSON_Delete(users_arr);
    return 0;
}
