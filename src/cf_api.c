/*
 * cf_api.c — Codeforces API 数据获取模块
 *
 * 使用 libcurl 发送 HTTP GET 请求，访问 CF 官方 API 端点并缓存
 * 返回的 JSON 数据到本地文件。所有 CF API 统一返回如下格式：
 *   {"status":"OK", "result": ...}
 * 或 {"status":"FAILED", "comment": "错误原因"}。
 *
 * 本模块每次请求前会调用 RateLimiter 等待放行，
 * 确保请求间隔 ≥ 2 秒，避免触发 API 频率限制被封 IP。
 */

#include "cf_api.h"
#include "json_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

/*
 * libcurl 写回调使用的动态缓冲区结构体。
 * libcurl 收到 HTTP 响应数据后会分多次调用 write_callback，
 * 每次传入一小块数据；本结构体用于将多块数据拼接成完整响应。
 */
struct MemoryBlock {
    char *data;     /* 指向动态分配的缓冲区（以 '\0' 结尾） */
    size_t size;    /* 当前缓冲区中有效数据的字节数（不含结尾 '\0'） */
};

/*
 * libcurl 写回调函数 —— 将服务器返回的数据逐块追加到 MemoryBlock。
 *
 * 参数：
 *   contents — 指向本次接收到的数据块
 *   size     — 每个数据单元的大小（始终为 1）
 *   nmemb    — 数据单元的数量
 *   userp    — 用户自定义指针，这里指向 MemoryBlock 结构体
 *
 * 返回：
 *   成功处理的字节数（必须等于 size * nmemb，否则 libcurl 认为出错）。
 */
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    /* 计算本次接收到的实际字节数 */
    size_t total = size * nmemb;

    /* 将 userp 转换为 MemoryBlock 指针 */
    struct MemoryBlock *mem = (struct MemoryBlock *)userp;

    /* 使用 realloc 扩展缓冲区，多分配 1 字节用于结尾 '\0' */
    char *ptr = realloc(mem->data, mem->size + total + 1);
    if (!ptr) return 0;  /* 内存分配失败，返回 0 通知 libcurl 出错 */

    /* 更新指针（realloc 可能移动内存位置） */
    mem->data = ptr;

    /* 将新数据复制到缓冲区末尾 */
    memcpy(&mem->data[mem->size], contents, total);
    mem->size += total;

    /* 始终保持以 '\0' 结尾，方便后续作为 C 字符串处理 */
    mem->data[mem->size] = '\0';

    return total;
}

/*
 * 发送 HTTP GET 请求，返回响应字符串。
 *
 * 调用者负责 free() 返回的字符串。
 * 失败返回 NULL。
 *
 * Windows 环境下禁用 SSL 证书验证，因为 MinGW 环境通常缺少
 * CA 证书包，启用验证会导致所有 HTTPS 请求失败。
 */
char *cf_http_get(const char *url) {
    /* 初始化 libcurl 会话句柄 */
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    /* 初始化动态缓冲区：分配 1 字节（只含 '\0'）表示空字符串 */
    struct MemoryBlock chunk;
    chunk.data = malloc(1);
    chunk.size = 0;

    /* 设置请求 URL */
    curl_easy_setopt(curl, CURLOPT_URL, url);
    /* 设置写回调函数和数据接收缓冲区 */
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    /* User-Agent 标识，部分 API 需要 */
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "CF-Crawler/1.0");
    /* 超时时间 30 秒，避免无限等待 */
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    /* 允许 HTTP 重定向跟随 */
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    /* 跳过 SSL 证书验证（Windows 环境缺少 CA 证书包） */
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    /* 执行 HTTP 请求（阻塞直到完成或超时） */
    CURLcode res = curl_easy_perform(curl);

    /* 清理 curl 句柄（不释放 chunk.data） */
    curl_easy_cleanup(curl);

    /* 检查请求是否成功 */
    if (res != CURLE_OK) {
        fprintf(stderr, "  [ERROR] curl failed: %s\n  URL: %s\n",
                curl_easy_strerror(res), url);
        free(chunk.data);
        return NULL;
    }

    /* 返回拼接好的完整响应字符串（调用者负责 free） */
    return chunk.data;
}

/*
 * 通用 API 请求流程：等待限速器 → 发送 HTTP GET → 校验 JSON → 写入缓存文件。
 *
 * 这是所有 CF API 端点的公共底层实现，封装了限速、请求、
 * 校验、缓存四个步骤。上层函数只需构造 URL 然后调用本函数即可。
 *
 * 参数：
 *   url  — CF API 端点 URL
 *   path — 本地缓存文件路径
 *   rl   — 限速器指针
 *
 * 返回：
 *   成功返回 0，失败返回 -1。
 */
int fetch_and_save(const char *url, const char *path, RateLimiter *rl) {
    /* 步骤 1：等待限速器放行，确保距上次请求 ≥ 2 秒 */
    rate_limiter_wait(rl);

    /* 步骤 2：发送 HTTP GET 请求，获取 JSON 响应字符串 */
    printf("  Fetching: %s\n", url);
    char *response = cf_http_get(url);
    if (!response) return -1;

    /* 步骤 3：记录本次请求时间，供下次 wait() 使用 */
    rate_limiter_tick(rl);

    /* 步骤 4：解析 JSON 并校验 status 字段是否为 "OK" */
    cJSON *root = cJSON_Parse(response);
    if (!root) {
        /* JSON 解析失败（格式错误或网络问题导致响应不完整） */
        fprintf(stderr, "  [ERROR] Invalid JSON response\n");
        free(response);
        return -1;
    }

    /* 检查顶层 status 字段是否存在 */
    cJSON *status = cJSON_GetObjectItem(root, "status");
    if (!status) {
        fprintf(stderr, "  [ERROR] Unexpected API response format\n");
        cJSON_Delete(root);
        free(response);
        return -1;
    }

    /* 检查 status 值是否为 "OK"。如果不是，打印 comment 字段
       的错误说明（如 "handle: Field should not be empty"） */
    if (strcmp(status->valuestring, "OK") != 0) {
        cJSON *comment = cJSON_GetObjectItem(root, "comment");
        fprintf(stderr, "  [ERROR] CF API: %s\n",
                comment ? comment->valuestring : status->valuestring);
        cJSON_Delete(root);
        free(response);
        return -1;
    }

    /* JSON 校验通过，释放解析树 */
    cJSON_Delete(root);

    /* 步骤 5：将原始 JSON 响应写入本地缓存文件 */
    int ret = write_text_file(path, response);
    free(response);

    if (ret != 0) {
        fprintf(stderr, "  [ERROR] Failed to write: %s\n", path);
    }
    return ret;
}

/*
 * 获取用户基本信息。
 * API 端点：user.info
 * URL:  https://codeforces.com/api/user.info?handles=<handle>
 * 缓存：data/<handle>_user.json
 *
 * 参数：
 *   handle — CF 用户名
 *   path   — 本地缓存文件路径
 *   rl     — 限速器指针
 *
 * 返回：成功 0，失败 -1。
 */
int cf_fetch_user_info(const char *handle, const char *path, RateLimiter *rl) {
    char url[512];
    snprintf(url, sizeof(url),
             "https://codeforces.com/api/user.info?handles=%s", handle);
    return fetch_and_save(url, path, rl);
}

/*
 * 获取用户 rating 变化历史。
 * API 端点：user.rating
 * URL:  https://codeforces.com/api/user.rating?handle=<handle>
 * 缓存：data/<handle>_rating.json
 *
 * 返回的 result 数组按比赛时间升序排列，每项包含：
 *   contestId, contestName, rank, oldRating, newRating, ratingUpdateTimeSeconds
 *
 * 参数：
 *   handle — CF 用户名
 *   path   — 本地缓存文件路径
 *   rl     — 限速器指针
 *
 * 返回：成功 0，失败 -1。
 */
int cf_fetch_user_rating(const char *handle, const char *path, RateLimiter *rl) {
    char url[512];
    snprintf(url, sizeof(url),
             "https://codeforces.com/api/user.rating?handle=%s", handle);
    return fetch_and_save(url, path, rl);
}

/*
 * 获取用户全部提交记录。
 * API 端点：user.status
 * URL:  https://codeforces.com/api/user.status?handle=<handle>
 * 缓存：data/<handle>_status.json
 *
 * 返回的 result 数组按提交时间降序（最新在前），每项包含：
 *   id, contestId, creationTimeSeconds, problem, verdict,
 *   programmingLanguage, timeConsumedMillis, memoryConsumedBytes 等
 *
 * 注意：该端点返回的数据量可能很大（用户可达数千条提交记录），
 * 因此失败时只发警告继续处理，不阻塞整个流程。
 *
 * 参数：
 *   handle — CF 用户名
 *   path   — 本地缓存文件路径
 *   rl     — 限速器指针
 *
 * 返回：成功 0，失败 -1。
 */
int cf_fetch_user_status(const char *handle, const char *path, RateLimiter *rl) {
    char url[512];
    snprintf(url, sizeof(url),
             "https://codeforces.com/api/user.status?handle=%s", handle);
    return fetch_and_save(url, path, rl);
}

/*
 * 获取全部比赛列表（排除训练赛 gym）。
 * API 端点：contest.list
 * URL:  https://codeforces.com/api/contest.list?gym=false
 * 缓存：data/contest_list.json
 *
 * 返回所有非 gym 比赛的元信息，按 id 降序（最新在前）。
 * 该数据全局共享，所有用户只请求一次。
 *
 * 参数：
 *   path — 本地缓存文件路径
 *   rl   — 限速器指针
 *
 * 返回：成功 0，失败 -1。
 */
int cf_fetch_contest_list(const char *path, RateLimiter *rl) {
    const char *url = "https://codeforces.com/api/contest.list?gym=false";
    return fetch_and_save(url, path, rl);
}

/*
 * 获取某场比赛的排行榜（备用端点，当前代码未使用）。
 * API 端点：contest.standings
 * URL:  https://codeforces.com/api/contest.standings?contestId=<id>&handles=<handle>&showUnofficial=true
 *
 * 返回指定比赛指定用户的排名详情。
 * 该端点目前未在 main.c 中调用，保留作为扩展接口。
 *
 * 参数：
 *   contest_id — 比赛 ID
 *   handle     — CF 用户名
 *   path       — 本地缓存文件路径
 *   rl         — 限速器指针
 *
 * 返回：成功 0，失败 -1。
 */
int cf_fetch_contest_standings(int contest_id, const char *handle,
                               const char *path, RateLimiter *rl) {
    char url[1024];
    snprintf(url, sizeof(url),
             "https://codeforces.com/api/contest.standings?contestId=%d"
             "&handles=%s&showUnofficial=true",
             contest_id, handle);
    return fetch_and_save(url, path, rl);
}
