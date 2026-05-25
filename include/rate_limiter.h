/*
 * rate_limiter.h — CF API 请求频率限制器接口
 *
 * 确保每两次 API 请求之间至少间隔 min_interval 秒，
 * 遵守 Codeforces API 频率限制。
 */

#pragma once

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#include <time.h>

/*
 * RateLimiter 结构体
 * last_request_time — 上次请求的时间戳（从系统启动以来的秒数）
 * min_interval      — 最小请求间隔（默认 2.0 秒）
 */
typedef struct {
    double last_request_time;
    double min_interval;
} RateLimiter;

/* 初始化限速器 */
void rate_limiter_init(RateLimiter *rl);

/* 等待直到可以发送下一个请求 */
void rate_limiter_wait(RateLimiter *rl);

/* 记录本次请求时间戳 */
void rate_limiter_tick(RateLimiter *rl);
