/*
 * analyzer.h — 核心数据分析模块接口
 *
 * 提供 rating 颜色映射、头衔计算、
 * 以及用户数据综合分析功能。
 */

#pragma once
#include "cJSON.h"

/* 根据 rating 返回 CF 颜色十六进制字符串 */
const char *rating_color(int rating);

/* 根据 rating 返回头衔名称 */
const char *rating_title(int rating);

/*
 * 构建单个用户的完整分析数据树
 * 参数：
 *   handle        — CF 用户名
 *   data_dir      — 缓存数据目录
 *   output_dir    — 输出目录（预留）
 *   max_standings — 最大 standings 数量（预留）
 * 返回：cJSON 对象，含比赛记录、统计数据、直方图等
 */
cJSON *build_user_summary(const char *handle, const char *data_dir,
                          const char *output_dir, int max_standings);
