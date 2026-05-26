/*
 * cf_api.c — Codeforces API 数据获取模块
 *
 * ═══════════════════════════════════════════════════════════
 *   负责所有网络通信和数据缓存。
 *   使用 libcurl 发送 HTTP GET 请求，
 *   访问 Codeforces 官方 API 并缓存返回的 JSON 数据。
 * ═══════════════════════════════════════════════════════════
 *
 * 【设计模式】
 *   本模块采用「请求-校验-缓存」三层架构：
 *     1. 请求层 (cf_http_get)     — 纯 HTTP 通信，不关心里面是什么
 *     2. 校验层 (fetch_and_save)  — 验证 CF API 响应格式，写入缓存
 *     3. 端点层 (cf_fetch_xxx)    — 针对不同 API 端点构造 URL
 *
 *   三个层次相互独立，每一层只做一件事。
 *
 * 【CF API 返回格式】
 *   所有 CF API 端点统一返回：
 *     {"status":"OK", "result": <数据>}
 *   或
 *     {"status":"FAILED", "comment":"..."}
 *
 *   校验层的核心工作就是检查 status == "OK"。
 *
 * 【调用的 5 个 CF API 端点】
 *   ┌─────────────────────┬──────────────────┬──────────────────────┐
 *   │ 端点                │ URL 模式          │ 返回内容              │
 *   ├─────────────────────┼──────────────────┼──────────────────────┤
 *   │ user.info           │ ?handles=xxx     │ 用户基本信息和 rating │
 *   │ user.rating         │ ?handle=xxx      │ Rating 变化历史      │
 *   │ user.status         │ ?handle=xxx      │ 全部提交记录          │
 *   │ contest.list        │ ?gym=false       │ 所有非训练赛列表      │
 *   │ contest.standings   │ ?contestId=...   │ 比赛排行榜（备用）    │
 *   └─────────────────────┴──────────────────┴──────────────────────┘
 *
 * 【缓存策略】
 *   每个 API 调用结果存入 data/ 目录下的 JSON 文件。
 *   下次运行时先检查缓存是否存在和有效，避免重复请求。
 *   （当前版本：如果缓存文件存在则直接使用，不做过期检查）
 *
 * 【API 限速】
 *   调用方需传入 RateLimiter 指针，本模块调用前会等待限速器放行。
 *   CF 官方限制约每秒1次请求，我们采用保守的 2 秒间隔。
 *
 * 【Windows 兼容性】
 *   Windows 环境通常没有预装 SSL CA 证书包，
 *   因此关闭了 SSL 证书验证（CURLOPT_SSL_VERIFYPEER = 0）。
 *   在安全敏感的生产环境中不应这样做，但对于本地数据分析工具是可接受的。
 */

#include "cf_api.h"
#include "json_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

/*
 * 【libcurl 写回调数据结构】
 * MemoryBlock 是一个动态增长的缓冲区。
 * 使用 realloc 逐块扩展，避免了事先不知道响应大小的尴尬。
 *
 * 为什么不直接用固定大小的缓冲区？
 *   CF 的 user.status 响应可能非常大（用户有上万条提交记录时可达 10MB+），
 *   固定大小缓冲区要么不够（截断），要么太大（浪费内存）。
 *   realloc 按需扩展是最合理的选择。
 */
struct MemoryBlock {
    char *data;   /* 动态分配的字符数组，末尾有 '\0' */
    size_t size;  /* 当前已存储的字节数（不含终止符） */
};

/*
 * libcurl 写回调函数
 * ═══════════════
 *
 * 每次服务器返回一块数据，libcurl 就调用这个函数一次。
 * 函数把新数据追加到 MemoryBlock 的末尾。
 *
 * 参数说明（由 libcurl 调用时传入）：
 *   contents — 本次收到的数据块指针
 *   size     — 每个数据单元的大小（通常是 1 字节）
 *   nmemb    — 数据单元的个数（所以 size*nmemb = 本次收到的字节数）
 *   userp    — 我们通过 CURLOPT_WRITEDATA 传入的自定义指针
 *
 * 返回值必须是实际处理的字节数，如果不等于 size*nmemb，
 * libcurl 会认为发生错误并中止传输。
 */
static size_t write_callback(void *contents, size_t size, size_t nmemb,
                              void *userp) {
    size_t total = size * nmemb;                /* 本次实际数据量 */
    struct MemoryBlock *mem = (struct MemoryBlock *)userp;

    /* 扩容：多分配 total 个字节 + 1（留 '\0' 位置） */
    char *ptr = realloc(mem->data, mem->size + total + 1);
    if (!ptr) return 0;  /* 内存不足 → 返回 0 让 curl 终止 */

    mem->data = ptr;
    /* 把新数据追加到已有数据后面 */
    memcpy(&mem->data[mem->size], contents, total);
    mem->size += total;
    mem->data[mem->size] = '\0';  /* 保持字符串终止符 */

    return total;
}

/*
 * 发送 HTTP GET 请求
 * ════════════════
 *
 * 【用途】
 *   对给定的 URL 发起 GET 请求，返回完整的 HTTP 响应体字符串。
 *
 * 【返回值】
 *   成功 → malloc 分配的响应字符串（调用者负责 free）
 *   失败 → NULL（网络错误或超时）
 *
 * 【超时设置】
 *   CURLOPT_TIMEOUT = 30 秒。如果 CF 服务器 30 秒内无响应则放弃。
 *   CF 的 submit API（user.status）有时响应很慢，30秒是合理的。
 *
 * 【SSL 说明】
 *   在 Windows 上，libcurl 需要 CA 证书包来验证 HTTPS 证书。
 *   MSYS2 环境一般自带了证书包，但为兼容性关掉了验证。
 *   这不影响数据正确性，只是不验证服务器身份（对公开 API 来说可以接受）。
 */
char *cf_http_get(const char *url) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    /* 初始化内存缓冲区（至少 1 字节，方便 realloc 工作） */
    struct MemoryBlock chunk;
    chunk.data = malloc(1);
    chunk.size = 0;

    /* ┄┄┄┄┄ 配置 curl 选项 ┄┄┄┄┄ */
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "CF-Crawler/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);        /* 30秒超时 */
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);  /* 跟随重定向 */
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);  /* 跳过证书验证 */
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);  /* 跳过主机名验证 */

    /* 执行请求 */
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        fprintf(stderr, "  [ERROR] curl failed: %s\n  URL: %s\n",
                curl_easy_strerror(res), url);
        free(chunk.data);
        return NULL;
    }

    return chunk.data;
}

/*
 * 通用的「请求 → 校验 → 缓存」流程
 * ═══════════════════════════════
 *
 * 【流程】
 *   1. 等待限速器 → 避免请求过快被封
 *   2. HTTP GET 请求 → 获取原始 JSON 响应
 *   3. 记录请求时间 → 通知限速器
 *   4. 解析 JSON → 校验 status 字段是否为 "OK"
 *   5. 写入本地缓存文件 → 下次运行直接读缓存
 *
 * 【为什么需要校验层？】
 *   CF API 可能在以下情况下返回非 "OK" 状态：
 *     - 用户不存在：{"status":"FAILED","comment":"handles: User not found"}
 *     - 请求太频繁：{"status":"FAILED","comment":"Call limit exceeded"}
 *     - 服务器错误：{"status":"FAILED","comment":"..."}
 *   如果直接使用未经校验的数据，后续分析会产生难以排查的错误。
 *   在校验层就拦截掉异常响应，让调用者放心使用缓存文件。
 *
 * 参数：
 *   url  — CF API 完整 URL
 *   path — 本地缓存文件路径（如 "data/tourist_user.json"）
 *   rl   — 限速器指针
 * 返回：成功 0，失败 -1
 */
int fetch_and_save(const char *url, const char *path, RateLimiter *rl) {
    /* 步骤 1: 等待限速器放行 */
    rate_limiter_wait(rl);

    /* 步骤 2: 发送 HTTP 请求 */
    printf("  Fetching: %s\n", url);
    char *response = cf_http_get(url);
    if (!response) return -1;

    /* 步骤 3: 记录请求时间 */
    rate_limiter_tick(rl);

    /* 步骤 4: 解析并校验 JSON 响应 */
    cJSON *root = cJSON_Parse(response);
    if (!root) {
        /* 响应不是合法的 JSON 格式 */
        fprintf(stderr, "  [ERROR] Invalid JSON response\n");
        free(response);
        return -1;
    }

    cJSON *status = cJSON_GetObjectItem(root, "status");
    if (!status) {
        /* 响应中缺少 status 字段，格式不符合 CF API 规范 */
        fprintf(stderr, "  [ERROR] Unexpected API response format "
                "(missing 'status' field)\n");
        cJSON_Delete(root);
        free(response);
        return -1;
    }

    if (strcmp(status->valuestring, "OK") != 0) {
        /* CF API 返回了错误，打印具体原因 */
        cJSON *comment = cJSON_GetObjectItem(root, "comment");
        fprintf(stderr, "  [ERROR] CF API returned failure: %s\n",
                comment ? comment->valuestring : status->valuestring);
        cJSON_Delete(root);
        free(response);
        return -1;
    }

    /* status == "OK" — 响应正常 */

    cJSON_Delete(root);

    /* 步骤 5: 写入本地缓存文件 */
    int ret = write_text_file(path, response);
    free(response);

    if (ret != 0) {
        fprintf(stderr, "  [ERROR] Failed to write cache file: %s\n", path);
    }
    return ret;
}

/* ═══════════════════════════════════════════════════════════════
 * 以下 5 个函数是 CF API 各端点的便捷封装
 *
 * 每个函数的职责非常简单：
 *   1. 根据端点规则拼接 URL
 *   2. 调用 fetch_and_save() 完成请求和缓存
 *   3. 把错误码原样返回给调用者
 *
 * 这种设计的好处：
 *   - 调用者不需要知道 URL 格式
 *   - 所有端点共享同一套错误处理和缓存逻辑
 *   - 新增端点只需写一个新函数，不改动现有代码
 * ═══════════════════════════════════════════════════════════════ */

/*
 * 获取用户基本信息
 * API: https://codeforces.com/api/user.info?handles=<handle>
 * 缓存: data/<handle>_user.json
 *
 * 返回内容示例：
 *   {"status":"OK","result":[{"handle":"tourist","rating":3774,
 *     "rank":"legendary grandmaster","avatar":"...","titlePhoto":"..."}]}
 * 注意 result 是数组（可同时查多个用户），但我们每次只查一个。
 */
int cf_fetch_user_info(const char *handle, const char *path, RateLimiter *rl) {
    char url[512];
    snprintf(url, sizeof(url),
             "https://codeforces.com/api/user.info?handles=%s", handle);
    return fetch_and_save(url, path, rl);
}

/*
 * 获取用户 Rating 变化历史
 * API: https://codeforces.com/api/user.rating?handle=<handle>
 * 缓存: data/<handle>_rating.json
 *
 * 返回内容示例：
 *   {"status":"OK","result":[
 *     {"contestId":1234,"contestName":"...","oldRating":1500,
 *      "newRating":1620,"rank":42,"ratingUpdateTimeSeconds":1699123400},
 *     ...
 *   ]}
 * 按时间从旧到新排列，每条记录对应一场参加了的比赛。
 */
int cf_fetch_user_rating(const char *handle, const char *path, RateLimiter *rl) {
    char url[512];
    snprintf(url, sizeof(url),
             "https://codeforces.com/api/user.rating?handle=%s", handle);
    return fetch_and_save(url, path, rl);
}

/*
 * 获取用户全部提交记录
 * API: https://codeforces.com/api/user.status?handle=<handle>
 * 缓存: data/<handle>_status.json
 *
 * 返回内容示例：
 *   {"status":"OK","result":[
 *     {"id":375842134,"contestId":2229,"creationTimeSeconds":1779555867,
 *      "problem":{"contestId":2229,"index":"H","name":"...","rating":2900},
 *      "verdict":"OK","programmingLanguage":"C++23",...},
 *     ...
 *   ]}
 *
 * 注意：
 *   - 返回所有提交，可能非常大（万条级别）
 *   - 每条提交的 problem 对象可能含 rating 字段，也可能没有（新题尚未评级）
 *   - 提交按时间从新到旧排列（最新的在前）
 */
int cf_fetch_user_status(const char *handle, const char *path, RateLimiter *rl) {
    char url[512];
    snprintf(url, sizeof(url),
             "https://codeforces.com/api/user.status?handle=%s", handle);
    return fetch_and_save(url, path, rl);
}

/*
 * 获取 Codeforces 比赛列表
 * API: https://codeforces.com/api/contest.list?gym=false
 * 缓存: data/contest_list.json
 *
 * 参数 gym=false 排除训练赛（gym），只返回正式比赛。
 * 所有用户共享同一份比赛列表，所以只获取一次。
 *
 * 返回内容是一维数组，包含上千场比赛的元数据。
 */
int cf_fetch_contest_list(const char *path, RateLimiter *rl) {
    const char *url = "https://codeforces.com/api/contest.list?gym=false";
    return fetch_and_save(url, path, rl);
}

/*
 * 获取比赛排行榜数据（备用端点，当前未被 main.c 调用）
 * API: https://codeforces.com/api/contest.standings
 *      ?contestId=<id>&handles=<handle>&showUnofficial=true
 *
 * 可用于获取更详细的比赛内数据（每题得分、hack 记录等）。
 * 当前版本已实现了所有核心分析功能，此端点保留备用。
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
