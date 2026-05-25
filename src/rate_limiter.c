/*
 * rate_limiter.c — CF API 请求频率限制器
 *
 * Codeforces 官方 API 限制：每秒最多 1 次请求
 * 本模块实现 2 秒间隔的保守限速策略，
 * 避免因请求过快被封 IP。
 *
 * 跨平台时间获取：
 *   Windows: GetSystemTimeAsFileTime
 *   POSIX:   clock_gettime(CLOCK_MONOTONIC)
 */

#include "rate_limiter.h"

#ifdef _WIN32
/*
 * Windows 高精度时间获取（100 纳秒单位 → 秒）
 * GetSystemTimeAsFileTime 精度约 100 纳秒
 */
static double get_wall_time(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    unsigned long long t = ft.dwHighDateTime;
    t <<= 32;
    t |= ft.dwLowDateTime;
    return (double)t / 10000000.0;
}
#else
/*
 * POSIX 单调时钟，不受系统时间调整影响
 */
static double get_wall_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
#endif

/*
 * 初始化限速器，设置最小请求间隔为 2 秒
 */
void rate_limiter_init(RateLimiter *rl) {
    rl->last_request_time = 0.0;
    rl->min_interval = 2.0;
}

/*
 * 等待直到可以发送下一个请求
 * 如果距上次请求不足 min_interval 秒，则休眠补齐间隔
 */
void rate_limiter_wait(RateLimiter *rl) {
    if (rl->last_request_time == 0.0) return;
    double now = get_wall_time();
    double elapsed = now - rl->last_request_time;
    if (elapsed < rl->min_interval) {
        double sleep_sec = rl->min_interval - elapsed;
#ifdef _WIN32
        Sleep((DWORD)(sleep_sec * 1000));
#else
        struct timespec ts;
        ts.tv_sec = (time_t)sleep_sec;
        ts.tv_nsec = (long)((sleep_sec - ts.tv_sec) * 1e9);
        nanosleep(&ts, NULL);
#endif
    }
}

/*
 * 记录本次请求时间，供下一次 wait() 使用
 */
void rate_limiter_tick(RateLimiter *rl) {
    rl->last_request_time = get_wall_time();
}
