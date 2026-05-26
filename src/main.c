/*
 * main.c — 程序入口与流程编排
 *
 * ═══════════════════════════════════════════════════════════
 *   本文件是程序的入口点（main 函数），负责：
 *   1. 解析命令行参数，读取用户列表文件
 *   2. 按顺序调用各模块，串联整个数据处理管线
 *   3. 处理错误和边界情况
 * ═══════════════════════════════════════════════════════════
 *
 * 【数据处理管线】
 *
 *   sample_users.txt             用户名单（每行一个 CF 用户名）
 *        │
 *        ▼
 *   [步骤 1] read_handles()      解析用户列表
 *        │
 *        ▼
 *   [步骤 2] cf_fetch_contest_list()  获取比赛元数据（全局共享，仅一次）
 *        │
 *        ▼
 *   [步骤 3] for each user:            逐个用户循环 ────────┐
 *     ├─ cf_fetch_user_info()         获取用户基本信息     │
 *     ├─ cf_fetch_user_rating()       获取 Rating 历史    │
 *     ├─ cf_fetch_user_status()       获取全部提交记录     │
 *     └─ build_user_summary()         分析数据 → cJSON 树 │
 *        │                                                  │
 *        ▼                                                  │
 *   [步骤 4] generate_all_html()      渲染 HTML 报告 ←─────┘
 *        │
 *        ▼
 *   output/index.html                浏览器打开即可查看
 *
 * 【命令行用法】
 *   cf_crawler.exe                         使用默认文件 sample_users.txt
 *   cf_crawler.exe my_handles.txt          指定自定义用户列表文件
 *
 * 【目录结构】
 *   data/     — API 数据缓存（JSON 文件）
 *   output/   — 生成的 HTML 报告
 *   bin/      — 编译产物（不纳入版本管理）
 *   web/      — HTML 模板文件
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

/* ════ 配置常量 ════ */
#define MAX_HANDLE_LEN 64       /* 用户名最大长度（CF 限制为 24 字符） */
#define MAX_HANDLES 256         /* 单次运行最多处理的用户数 */
#define DATA_DIR "data"         /* API 数据缓存目录 */
#define OUTPUT_DIR "output"     /* HTML 报告输出目录 */
#define TEMPLATE_PATH "web/template.html" /* HTML 模板路径 */

/* ════ 跨平台目录创建 ════ */
#ifdef _WIN32
#include <direct.h>
#define mkdir_p(path) _mkdir(path)   /* Windows: _mkdir 只需一个参数 */
#else
#define mkdir_p(path) mkdir(path, 0755)  /* POSIX: mkdir 需要权限参数 */
#endif

/*
 * 从文本文件中读取 CF 用户名列表
 * ═══════════════════════════════
 *
 * 【文件格式】
 *   每行一个用户名，支持空行（自动跳过）。
 *   示例：
 *     tourist
 *     Benq
 *     jiangly
 *
 * 【处理逻辑】
 *   1. 跳过行首空白字符
 *   2. 从行尾去除换行符、回车符和空白字符
 *   3. 忽略空行和超长用户名
 *
 * 【安全考虑】
 *   MAX_HANDLE_LEN = 64，大于 CF 实际限制（24字符），
 *   提供额外余量以应对未来变化。
 *
 * 参数：
 *   path     — 用户列表文件路径
 *   handles  — 输出：二维字符数组，每行存储一个用户名
 * 返回：成功读取的用户数；文件打开失败返回 -1
 */
static int read_handles(const char *path, char handles[][MAX_HANDLE_LEN]) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Cannot open handle list: %s\n", path);
        return -1;
    }

    int count = 0;
    char buf[256];  /* 单行缓冲区，远大于 MAX_HANDLE_LEN */

    while (fgets(buf, sizeof(buf), f) && count < MAX_HANDLES) {
        /* 去除行首空白 */
        char *s = buf;
        while (*s == ' ' || *s == '\t' || *s == '\r') s++;

        /* 去除行尾换行符和空白 */
        int len = (int)strlen(s);
        while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r'
                        || s[len-1] == ' '  || s[len-1] == '\t'))
            s[--len] = '\0';

        /* 跳过空行和超长用户名 */
        if (len > 0 && len < MAX_HANDLE_LEN) {
            /* memmove 而不是 strcpy，因为 s 和 handles[count]
               可能重叠（虽然这里实际上不会，但更安全） */
            memmove(handles[count], s, (size_t)len + 1);
            count++;
        }
    }
    fclose(f);
    return count;
}

/*
 * ╔══════════════════════════════════════════════════════════════╗
 * ║                      程序主入口                              ║
 * ║                                                            ║
 * ║   顺序执行 4 大步骤，每个步骤都有错误处理和进度提示。          ║
 * ╚══════════════════════════════════════════════════════════════╝
 */
int main(int argc, char *argv[]) {
    /* 命令行参数：可指定自定义的 handle 列表文件 */
    const char *handle_file = "sample_users.txt";
    if (argc > 1) handle_file = argv[1];

    printf("=== Codeforces Crawler 2025 ===\n");
    printf("Handle list: %s\n\n", handle_file);

    /* ═══════════════════════════════════════════
     * 步骤 1：读取用户列表
     * ═══════════════════════════════════════════ */
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

    /* 初始化限速器：设置 2 秒最小间隔（比 CF 官方 1 秒更保守） */
    RateLimiter rl;
    rate_limiter_init(&rl);

    /* ═══════════════════════════════════════════
     * 步骤 2：获取比赛列表（所有用户共享，只获取一次）
     *
     * 比赛列表用于构建"比赛ID→起止时间"查找表，
     * 是赛时/补题判定算法的数据基础。
     * ═══════════════════════════════════════════ */
    printf("[1/5] Fetching contest list...\n");
    if (cf_fetch_contest_list(DATA_DIR "/contest_list.json", &rl) != 0) {
        fprintf(stderr, "Failed to fetch contest list\n");
        /* 继续执行，缺少比赛列表时各函数会用默认值 */
    }

    /* 用于汇总所有用户分析结果的数组 */
    cJSON *users_arr = cJSON_CreateArray();
    if (!users_arr) {
        fprintf(stderr, "Memory error\n");
        return 1;
    }

    /* ═══════════════════════════════════════════
     * 步骤 3：逐个用户 — 获取数据 → 分析
     *
     * 对每个用户依次调用 3 个 API（info / rating / status），
     * 然后调用 build_user_summary 进行分析。
     * 如果某个 API 调用失败（用户不存在、网络错误等），
     * 跳过该用户并继续处理下一个。
     * ═══════════════════════════════════════════ */
    for (int i = 0; i < handle_count; i++) {
        const char *handle = handles[i];
        printf("\n--- Processing: %s (%d/%d) ---\n", handle, i + 1, handle_count);

        char path[1024];

        /* 3a: user.info — 获取用户基本信息（rating、rank、avatar 等）
         *     必需步骤，失败则跳过该用户 */
        printf("[1/3] Fetching user info...\n");
        snprintf(path, sizeof(path), "%s/%s_user.json", DATA_DIR, handle);
        if (cf_fetch_user_info(handle, path, &rl) != 0) {
            fprintf(stderr, "  Skipping %s (failed to fetch user info)\n", handle);
            continue;
        }

        /* 3b: user.rating — 获取 rating 变化历史（参赛记录）
         *     必需步骤，失败则跳过该用户 */
        printf("[2/3] Fetching rating history...\n");
        snprintf(path, sizeof(path), "%s/%s_rating.json", DATA_DIR, handle);
        if (cf_fetch_user_rating(handle, path, &rl) != 0) {
            fprintf(stderr, "  Skipping %s (failed to fetch rating history)\n", handle);
            continue;
        }

        /* 3c: user.status — 获取所有提交记录
         *     非必需步骤，失败只警告不跳过（仍可生成基本报告） */
        printf("[3/3] Fetching submissions...\n");
        snprintf(path, sizeof(path), "%s/%s_status.json", DATA_DIR, handle);
        if (cf_fetch_user_status(handle, path, &rl) != 0) {
            fprintf(stderr, "  Warning: failed to fetch submissions for %s\n", handle);
        }

        /* 3d: 分析数据 — 调用 analyzer 模块生成用户 JSON 树 */
        printf("  Analyzing data...\n");
        cJSON *summary = build_user_summary(handle, DATA_DIR, OUTPUT_DIR, 0);
        if (summary) {
            cJSON_AddItemToArray(users_arr, summary);
            printf("  Analysis complete.\n");
        } else {
            fprintf(stderr, "  Failed to analyze data for %s\n", handle);
        }
    }

    /* ═══════════════════════════════════════════
     * 步骤 4：生成 HTML 报告
     *
     * 将分析结果渲染为可视化页面：
     *   - output/index.html        — 多用户首页（含导航）
     *   - output/report_<handle>.html — 各用户详情页
     * ═══════════════════════════════════════════ */
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

    /* 清理 cJSON 内存 */
    cJSON_Delete(users_arr);
    return 0;
}
