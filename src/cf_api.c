/*
 * cf_api.c — Codeforces API 数据获取模块
 *
 * 使用 libcurl 发送 HTTP GET 请求，访问 CF API 并缓存 JSON 数据。
 * 所有 API 统一返回 {"status":"OK","result":...} 格式。
 * 本模块调用前会等待 RateLimiter 放行，避免请求过快被封。
 */

#include "cf_api.h"
#include "json_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

/* libcurl 写回调用的动态缓冲区 */
struct MemoryBlock {
    char *data;
    size_t size;
};

/* 将服务器返回的数据逐块追加到 MemoryBlock 末尾 */
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    struct MemoryBlock *mem = (struct MemoryBlock *)userp;

    char *ptr = realloc(mem->data, mem->size + total + 1);
    if (!ptr) return 0;

    mem->data = ptr;
    memcpy(&mem->data[mem->size], contents, total);
    mem->size += total;
    mem->data[mem->size] = '\0';
    return total;
}

/* 发送 HTTP GET 请求，返回响应字符串（调用者负责 free）。
   Windows 环境下跳过 SSL 证书验证，因为缺少 CA 证书包。 */
char *cf_http_get(const char *url) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    struct MemoryBlock chunk;
    chunk.data = malloc(1);
    chunk.size = 0;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "CF-Crawler/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

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

/* 通用流程：等待限速器 → 发请求 → 校验 JSON → 写入缓存文件 */
int fetch_and_save(const char *url, const char *path, RateLimiter *rl) {
    rate_limiter_wait(rl);

    printf("  Fetching: %s\n", url);
    char *response = cf_http_get(url);
    if (!response) return -1;
    rate_limiter_tick(rl);

    /* 校验 CF API 响应是否合法 (status == "OK") */
    cJSON *root = cJSON_Parse(response);
    if (!root) {
        fprintf(stderr, "  [ERROR] Invalid JSON response\n");
        free(response);
        return -1;
    }

    cJSON *status = cJSON_GetObjectItem(root, "status");
    if (!status) {
        fprintf(stderr, "  [ERROR] Unexpected API response format\n");
        cJSON_Delete(root);
        free(response);
        return -1;
    }
    if (strcmp(status->valuestring, "OK") != 0) {
        cJSON *comment = cJSON_GetObjectItem(root, "comment");
        fprintf(stderr, "  [ERROR] CF API: %s\n",
                comment ? comment->valuestring : status->valuestring);
        cJSON_Delete(root);
        free(response);
        return -1;
    }

    cJSON_Delete(root);

    /* 写入本地缓存 */
    int ret = write_text_file(path, response);
    free(response);
    if (ret != 0) {
        fprintf(stderr, "  [ERROR] Failed to write: %s\n", path);
    }
    return ret;
}

/* 获取用户基本信息 (user.info) → data/<handle>_user.json */
int cf_fetch_user_info(const char *handle, const char *path, RateLimiter *rl) {
    char url[512];
    snprintf(url, sizeof(url),
             "https://codeforces.com/api/user.info?handles=%s", handle);
    return fetch_and_save(url, path, rl);
}

/* 获取 rating 变化历史 (user.rating) → data/<handle>_rating.json */
int cf_fetch_user_rating(const char *handle, const char *path, RateLimiter *rl) {
    char url[512];
    snprintf(url, sizeof(url),
             "https://codeforces.com/api/user.rating?handle=%s", handle);
    return fetch_and_save(url, path, rl);
}

/* 获取全部提交记录 (user.status) → data/<handle>_status.json */
int cf_fetch_user_status(const char *handle, const char *path, RateLimiter *rl) {
    char url[512];
    snprintf(url, sizeof(url),
             "https://codeforces.com/api/user.status?handle=%s", handle);
    return fetch_and_save(url, path, rl);
}

/* 获取比赛列表 (contest.list)，排除训练赛(gym=false) → data/contest_list.json */
int cf_fetch_contest_list(const char *path, RateLimiter *rl) {
    const char *url = "https://codeforces.com/api/contest.list?gym=false";
    return fetch_and_save(url, path, rl);
}

/* 获取某场比赛排行榜，备用端点，当前未使用 */
int cf_fetch_contest_standings(int contest_id, const char *handle,
                               const char *path, RateLimiter *rl) {
    char url[1024];
    snprintf(url, sizeof(url),
             "https://codeforces.com/api/contest.standings?contestId=%d"
             "&handles=%s&showUnofficial=true",
             contest_id, handle);
    return fetch_and_save(url, path, rl);
}
