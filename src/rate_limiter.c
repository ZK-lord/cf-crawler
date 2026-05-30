/*
 * rate_limiter.c — CF API 请求频率限制器
 *
 * Codeforces 官方 API 使用条款限制：每秒最多 1 次请求。
 * 本模块实现 2 秒间隔的保守限速策略，在每个请求之间
 * 强制等待至少 2 秒，避免因请求过于频繁被暂时封禁 IP。
 *
 * 跨平台高精度时间获取：
 *   Windows: GetSystemTimeAsFileTime（精度约 100 纳秒）
 *   POSIX:   clock_gettime(CLOCK_MONOTONIC)（不受系统时间调整影响）
 */

#include "rate_limiter.h"

#ifdef _WIN32

/*
 * Windows 平台：使用 GetSystemTimeAsFileTime 获取高精度时间。
 * 该 API 返回自 1601-01-01 以来的 100 纳秒间隔数，
 * 转换为以秒为单位的 double 浮点值返回。
 */
static double get_wall_time(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);

    /* 将高低 32 位组合成 64 位无符号整数 */
    unsigned long long t = ft.dwHighDateTime;
    t <<= 32;
    t |= ft.dwLowDateTime;

    /* 转换为秒：除以 10,000,000（100 纳秒 × 10^7 = 1 秒） */
    return (double)t / 10000000.0;
}

#else

/*
 * POSIX 平台：使用 clock_gettime(CLOCK_MONOTONIC) 获取单调时钟。
 * CLOCK_MONOTONIC 不受系统时间调整（如 NTP 校时）影响，
 * 适合用来计算时间间隔。
 */
static double get_wall_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    /* tv_sec 是秒数，tv_nsec 是纳秒数。
       将纳秒除以 10^9 加到秒上，返回以秒为单位的浮点值 */
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

#endif

/*
 * 初始化限速器状态。
 *
 * 设置最小请求间隔为 2.0 秒，上次请求时间初始化为 0.0
 * （0.0 是一个特殊标记值，表示"尚未发送过任何请求"，
 *  rate_limiter_wait 检测到此值会跳过等待）。
 */
void rate_limiter_init(RateLimiter *rl) {
    rl->last_request_time = 0.0;   /* 哨兵值：0.0 表示从未请求，无需等待 */
    rl->min_interval = 2.0;        /* 2 秒间隔，比 CF 的 1 秒限制更保守 */
}

/*
 * 等待直到允许发送下一个请求。
 *
 * 计算当前时间距离上次请求的间隔：
 *   - 如果是首次请求（last_request_time == 0.0），直接放行；
 *   - 如果间隔 ≥ min_interval（2 秒），直接放行；
 *   - 如果间隔不足 2 秒，计算差值并休眠补足。
 *
 * 休眠函数：
 *   Windows: Sleep(毫秒)
 *   POSIX:   nanosleep(秒+纳秒)
 */
void rate_limiter_wait(RateLimiter *rl) {
    /* 首次请求：last_request_time == 0.0 表示没有历史请求记录，
       无需等待，直接返回 */
    if (rl->last_request_time == 0.0) return;

    /* 获取当前时间并计算距上次请求已过的时间 */
    double now = get_wall_time();
    double elapsed = now - rl->last_request_time;

    /* 如果已过时间不足最小间隔，需要补足差额 */
    if (elapsed < rl->min_interval) {
        /* 计算还需要等待的秒数 */
        double sleep_sec = rl->min_interval - elapsed;

#ifdef _WIN32
        /* Windows：Sleep 接受毫秒参数。
           乘以 1000 将秒转为毫秒，强制转换为 DWORD 类型 */
        Sleep((DWORD)(sleep_sec * 1000));
#else
        /* POSIX：nanosleep 使用 timespec 结构体分别指定秒和纳秒 */
        struct timespec ts;
        ts.tv_sec = (time_t)sleep_sec;                          /* 整数秒 */
        ts.tv_nsec = (long)((sleep_sec - ts.tv_sec) * 1e9);     /* 剩余小数秒 → 纳秒 */
        nanosleep(&ts, NULL);
#endif
    }
}

/*
 * 记录本次请求的完成时间。
 *
 * 必须在实际 HTTP 请求完成后调用（而非请求开始前），
 * 因为 HTTP 请求本身的耗时不应计入等待间隔。
 * 下次 rate_limiter_wait 会以此时间点开始计算间隔。
 */
void rate_limiter_tick(RateLimiter *rl) {
    /* 以当前精确时间更新 last_request_time，
       作为下次 wait() 计算间隔的基准点 */
    rl->last_request_time = get_wall_time();
}
