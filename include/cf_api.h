/*
 * cf_api.h — Codeforces API 数据获取模块接口
 *
 * 封装了 libcurl 的 HTTP GET 请求和 5 个 CF API 端点调用。
 */

#pragma once
#include "rate_limiter.h"

/* 发送 HTTP GET 请求，返回 malloc 分配的响应字符串 */
char *cf_http_get(const char *url);

/* 通用：请求 URL → 校验 JSON → 写入文件 */
int fetch_and_save(const char *url, const char *path, RateLimiter *rl);

/* 获取用户基本信息（rating、rank、avatar 等） */
int cf_fetch_user_info(const char *handle, const char *path, RateLimiter *rl);

/* 获取用户 rating 变化历史 */
int cf_fetch_user_rating(const char *handle, const char *path, RateLimiter *rl);

/* 获取用户全部提交记录 */
int cf_fetch_user_status(const char *handle, const char *path, RateLimiter *rl);

/* 获取 CF 比赛列表（排除训练赛） */
int cf_fetch_contest_list(const char *path, RateLimiter *rl);

/* 获取某场比赛的排行榜数据（备用） */
int cf_fetch_contest_standings(int contest_id, const char *handle,
                               const char *path, RateLimiter *rl);
