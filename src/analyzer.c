/*
 * analyzer.c — 核心数据分析模块
 *
 * 读取 data/ 目录下的缓存 JSON 文件，分析用户比赛表现。
 *
 * 三个核心功能：
 *   1. 赛时/补题判定 — 比较提交时间与比赛结束时间
 *   2. AC 难度分布直方图 — 按题目 rating 分桶，支持多时间窗口
 *   3. 活跃度统计 — 近 180 天参赛次数和最高分
 */

#include "analyzer.h"
#include "json_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* CF 官方 10 级颜色体系，用于图表着色 */
const char *rating_color(int rating) {
    if (rating >= 3000) return "#000000";  /* LGM  黑 */
    if (rating >= 2600) return "#CC0000";  /* IGM  深红 */
    if (rating >= 2400) return "#FF0000";  /* GM   红 */
    if (rating >= 2300) return "#FF8C00";  /* IM   橙 */
    if (rating >= 2100) return "#FF8C00";  /* M    橙 */
    if (rating >= 1900) return "#AA00AA";  /* CM   紫 */
    if (rating >= 1600) return "#0000FF";  /* Expert 蓝 */
    if (rating >= 1400) return "#03A89E";  /* Specialist 青 */
    if (rating >= 1200) return "#008000";  /* Pupil 绿 */
    return "#808080";                      /* Newbie 灰 */
}

/* 对应颜色的文字头衔 */
const char *rating_title(int rating) {
    if (rating >= 3000) return "Legendary Grandmaster";
    if (rating >= 2600) return "International Grandmaster";
    if (rating >= 2400) return "Grandmaster";
    if (rating >= 2300) return "International Master";
    if (rating >= 2100) return "Master";
    if (rating >= 1900) return "Candidate Master";
    if (rating >= 1600) return "Expert";
    if (rating >= 1400) return "Specialist";
    if (rating >= 1200) return "Pupil";
    return "Newbie";
}

/*
 * 构建 比赛ID → {名称, 开始时间, 时长} 的查找表
 *
 * contest.list 包含所有比赛的元数据，但 rating 历史只给了 contestId。
 * 用这个查找表可以 O(1) 取到比赛的起止时间，用于后面的赛时/补题判定。
 */
static cJSON *build_contest_lookup(cJSON *contest_list) {
    cJSON *lookup = cJSON_CreateObject();
    if (!contest_list || !lookup) return lookup;

    cJSON *item;
    cJSON_ArrayForEach(item, contest_list) {
        cJSON *id = cJSON_GetObjectItem(item, "id");
        if (!id || !cJSON_IsNumber(id)) continue;

        char key[32];
        snprintf(key, sizeof(key), "%d", id->valueint);
        cJSON *entry = cJSON_CreateObject();
        entry = cJSON_AddObjectToObject(lookup, key);

        cJSON *name = cJSON_GetObjectItem(item, "name");
        cJSON_AddStringToObject(entry, "name",
            name && name->valuestring ? name->valuestring : "");

        cJSON *st = cJSON_GetObjectItem(item, "startTimeSeconds");
        cJSON_AddNumberToObject(entry, "startTimeSeconds",
            st && cJSON_IsNumber(st) ? st->valuedouble : 0);

        cJSON *dur = cJSON_GetObjectItem(item, "durationSeconds");
        cJSON_AddNumberToObject(entry, "durationSeconds",
            dur && cJSON_IsNumber(dur) ? dur->valuedouble : 7200);
    }
    return lookup;
}

/* 一场比赛最多 26 题（A-Z） */
#define MAX_PROBLEMS 26

/* 单道题的提交状态 */
typedef struct {
    char index[4];        /* 题号: A, B, C, ... */
    int in_contest_ac;    /* 赛时是否通过 */
    int post_ac;          /* 赛后是否补题通过 */
    int rejected;         /* 错误提交次数 */
    int problem_rating;   /* 题目难度分（800-3500，可能为0表示未评级） */
    int seen;
} ProblemState;

/* 按题号排序 */
static int prob_cmp(const void *a, const void *b) {
    const ProblemState *pa = (const ProblemState *)a;
    const ProblemState *pb = (const ProblemState *)b;
    return strcmp(pa->index, pb->index);
}

/*
 * 赛时/补题判定算法
 *
 * 原理：每条提交有 creationTimeSeconds（提交时间戳）。
 * 比赛结束时间 contest_end = 开始时间 + 持续时间。
 * 比较两者即可区分：
 *
 *   sub_time <= contest_end → 赛内提交
 *     → verdict == OK → 赛时通过，标记绿色
 *     → verdict != OK → 记录错误次数，标记红色
 *
 *   sub_time > contest_end → 赛后提交
 *     → verdict == OK → 补题通过，标记黄色
 *
 * 参数：
 *   status      — 全部提交记录
 *   contest_id  — 目标比赛 ID
 *   contest_end — 比赛结束时间戳
 *   probs       — 输出：各题目的状态数组
 *   max_probs   — 数组容量
 * 返回：涉及的题目数
 */
static int process_contest_problems(cJSON *status, int contest_id,
                                    long long contest_end,
                                    ProblemState *probs, int max_probs) {
    int prob_count = 0;
    if (!status) return 0;

    cJSON *sub;
    cJSON_ArrayForEach(sub, status) {

        /* 只处理属于本场比赛的提交 */
        cJSON *scid = cJSON_GetObjectItem(sub, "contestId");
        if (!scid || scid->valueint != contest_id) continue;

        cJSON *sproblem = cJSON_GetObjectItem(sub, "problem");
        if (!sproblem) continue;
        cJSON *sindex = cJSON_GetObjectItem(sproblem, "index");
        if (!sindex) continue;

        /* 在 probs 数组中查找或创建该题的记录 */
        int idx = -1;
        for (int i = 0; i < prob_count; i++) {
            if (strcmp(probs[i].index, sindex->valuestring) == 0) {
                idx = i;
                break;
            }
        }
        if (idx < 0 && prob_count < max_probs) {
            idx = prob_count++;
            strncpy(probs[idx].index, sindex->valuestring, sizeof(probs[idx].index) - 1);
            probs[idx].index[sizeof(probs[idx].index) - 1] = '\0';
            probs[idx].in_contest_ac = 0;
            probs[idx].post_ac = 0;
            probs[idx].rejected = 0;
            probs[idx].problem_rating = 0;
            probs[idx].seen = 1;

            cJSON *prating = cJSON_GetObjectItem(sproblem, "rating");
            if (prating && cJSON_IsNumber(prating))
                probs[idx].problem_rating = prating->valueint;
        }
        if (idx < 0) continue;

        cJSON *sverdict = cJSON_GetObjectItem(sub, "verdict");
        if (!sverdict) continue;

        /* 获取提交时间 */
        cJSON *sctime = cJSON_GetObjectItem(sub, "creationTimeSeconds");
        long long sub_time = sctime && cJSON_IsNumber(sctime) ?
            (long long)sctime->valuedouble : 0;

        /* 核心判定：比较提交时间与比赛结束时间 */
        if (strcmp(sverdict->valuestring, "OK") == 0) {
            if (sub_time > 0 && sub_time <= contest_end) {
                probs[idx].in_contest_ac = 1;  /* 赛时 AC */
            } else {
                probs[idx].post_ac = 1;         /* 赛后补题 AC */
            }
        } else {
            probs[idx].rejected++;
        }
    }

    qsort(probs, prob_count, sizeof(ProblemState), prob_cmp);
    return prob_count;
}

/*
 * 统计 AC 题目难度分布直方图
 *
 * 遍历所有提交，筛选 verdict=="OK" 的 AC 记录，
 * 按 problem.rating 分桶，支持时间窗口过滤。
 *
 * 同一份 status 数据会调用 4 次，生成 4 个直方图：
 *   window_sec=0           → 全部历史
 *   window_sec=365*86400   → 近一年
 *   window_sec=180*86400   → 近 180 天
 *   window_sec=30*86400    → 近一个月
 *
 * 注意：cJSON 累加时必须同时更新 valueint 和 valuedouble，
 * 否则序列化时两者不等会输出错误的原始值。
 */
static void count_ac_by_rating(cJSON *status, cJSON *hist,
                               long long now_ts, long long window_sec) {
    if (!status) return;

    cJSON *sub;
    cJSON_ArrayForEach(sub, status) {
        cJSON *verdict = cJSON_GetObjectItem(sub, "verdict");
        if (!verdict || strcmp(verdict->valuestring, "OK") != 0) continue;

        cJSON *ctime = cJSON_GetObjectItem(sub, "creationTimeSeconds");
        if (!ctime) continue;

        /* 时间窗口过滤 */
        long long ts = (long long)ctime->valuedouble;
        if (window_sec > 0 && now_ts - ts > window_sec) continue;

        cJSON *problem = cJSON_GetObjectItem(sub, "problem");
        if (!problem) continue;
        cJSON *prating = cJSON_GetObjectItem(problem, "rating");
        int r = prating ? prating->valueint : 0;

        /* 以 rating 为 key 累加计数 */
        char key[16];
        snprintf(key, sizeof(key), "%d", r);
        cJSON *existing = cJSON_GetObjectItem(hist, key);
        if (existing) {
            existing->valueint++;
            existing->valuedouble = existing->valueint;
        } else {
            cJSON_AddNumberToObject(hist, key, 1);
        }
    }
}

/*
 * 修正 CF API 返回的 rank 字段。
 * API 偶尔返回错误数据（如 tourist 的 maxRank 返回了他的用户名），
 * 这里根据 rating 值重新计算确保正确。
 */
static void adjust_rating_rank(cJSON *user) {
    int rating = 0, maxRating = 0;
    cJSON *r = cJSON_GetObjectItem(user, "rating");
    if (r && cJSON_IsNumber(r)) rating = r->valueint;
    cJSON *mr = cJSON_GetObjectItem(user, "maxRating");
    if (mr && cJSON_IsNumber(mr)) maxRating = mr->valueint;

    cJSON_DeleteItemFromObject(user, "rank");
    cJSON_AddStringToObject(user, "rank", rating_title(rating));

    cJSON_DeleteItemFromObject(user, "maxRank");
    cJSON_AddStringToObject(user, "maxRank", rating_title(maxRating));
}

/*
 * 构建单个用户的完整分析数据
 *
 * 读取 4 个缓存文件，分析后返回一个 cJSON 对象树，包含：
 *   - 基本信息: handle, rating, rank, avatar 等
 *   - 统计: contestCount, contestCount180, maxRating180
 *   - 比赛记录数组: 每场比赛每题的状态和补题
 *   - 直方图: histAll, histYear, hist180, histMonth
 */
cJSON *build_user_summary(const char *handle, const char *data_dir,
                          const char *output_dir, int max_standings) {
    (void)output_dir;
    (void)max_standings;

    cJSON *result = cJSON_CreateObject();
    if (!result) return NULL;

    /* 构建缓存文件路径 */
    char user_path[512], rating_path[512], status_path[512], clist_path[512];
    snprintf(user_path, sizeof(user_path), "%s/%s_user.json", data_dir, handle);
    snprintf(rating_path, sizeof(rating_path), "%s/%s_rating.json", data_dir, handle);
    snprintf(status_path, sizeof(status_path), "%s/%s_status.json", data_dir, handle);
    snprintf(clist_path, sizeof(clist_path), "%s/contest_list.json", data_dir);

    /* 加载数据：user info 和 rating 是必需的，status 和 contest_list 可选 */
    cJSON *user_info = load_cf_result_first(user_path);
    if (!user_info) {
        fprintf(stderr, "  [ERROR] Cannot load user info for %s\n", handle);
        cJSON_Delete(result);
        return NULL;
    }

    cJSON *rating_history = load_cf_result_array(rating_path);
    if (!rating_history) {
        fprintf(stderr, "  [ERROR] Cannot load rating history for %s\n", handle);
        cJSON_Delete(user_info);
        cJSON_Delete(result);
        return NULL;
    }

    cJSON *status = load_cf_result_array(status_path);
    cJSON *contest_list = load_cf_result_array(clist_path);

    /* 时间窗口常量 */
    time_t now = time(NULL);
    long long now_ts = (long long)now;
    long long year_sec = 365LL * 86400;
    long long halfyear_sec = 180LL * 86400;
    long long month_sec = 30LL * 86400;

    /* 修正 API 头衔数据 */
    adjust_rating_rank(user_info);

    /* ═══════════════════════════════════════════
     * 第一部分：基本信息
     * ═══════════════════════════════════════════ */

    cJSON *cf_handle = cJSON_GetObjectItem(user_info, "handle");
    cJSON_AddStringToObject(result, "handle",
        cf_handle && cf_handle->valuestring ? cf_handle->valuestring : handle);

    cJSON *avatar = cJSON_GetObjectItem(user_info, "avatar");
    cJSON_AddStringToObject(result, "avatar",
        avatar && avatar->valuestring ? avatar->valuestring : "");

    cJSON *titlePhoto = cJSON_GetObjectItem(user_info, "titlePhoto");
    cJSON_AddStringToObject(result, "titlePhoto",
        titlePhoto && titlePhoto->valuestring ? titlePhoto->valuestring : "");

    int rating = 0, maxRating = 0;
    cJSON *r = cJSON_GetObjectItem(user_info, "rating");
    if (r && cJSON_IsNumber(r)) rating = r->valueint;
    cJSON *mr = cJSON_GetObjectItem(user_info, "maxRating");
    if (mr && cJSON_IsNumber(mr)) maxRating = mr->valueint;

    cJSON_AddNumberToObject(result, "rating", rating);
    cJSON_AddNumberToObject(result, "maxRating", maxRating);
    cJSON_AddStringToObject(result, "ratingColor", rating_color(rating));

    const char *rank_str = "Unrated";
    cJSON *rankj = cJSON_GetObjectItem(user_info, "rank");
    if (rankj && rankj->valuestring) rank_str = rankj->valuestring;
    cJSON_AddStringToObject(result, "rank", rank_str);

    const char *maxRankStr = "Unrated";
    cJSON *maxRankj = cJSON_GetObjectItem(user_info, "maxRank");
    if (maxRankj && maxRankj->valuestring) maxRankStr = maxRankj->valuestring;
    cJSON_AddStringToObject(result, "maxRank", maxRankStr);

    /* ═══════════════════════════════════════════
     * 第二部分：遍历每场比赛，生成详细记录
     * ═══════════════════════════════════════════ */

    cJSON *contest_lookup = build_contest_lookup(contest_list);
    if (!contest_lookup) contest_lookup = cJSON_CreateObject();

    int contest_count = cJSON_GetArraySize(rating_history);
    int contest_count_180 = 0;
    int max_rating_180 = 0;
    cJSON *contests_arr = cJSON_AddArrayToObject(result, "contests");

    cJSON *item;
    cJSON_ArrayForEach(item, rating_history) {
        cJSON *contest_obj = cJSON_CreateObject();

        int contest_id = 0;
        long long update_time = 0;
        cJSON *cid = cJSON_GetObjectItem(item, "contestId");
        if (cid && cJSON_IsNumber(cid)) contest_id = cid->valueint;
        cJSON *ctime = cJSON_GetObjectItem(item, "ratingUpdateTimeSeconds");
        if (ctime && cJSON_IsNumber(ctime)) update_time = (long long)ctime->valuedouble;

        cJSON_AddNumberToObject(contest_obj, "contestId", contest_id);

        cJSON *cname = cJSON_GetObjectItem(item, "contestName");
        cJSON_AddStringToObject(contest_obj, "contestName",
            cname && cname->valuestring ? cname->valuestring : "");

        cJSON_AddNumberToObject(contest_obj, "timestamp", (double)update_time);

        /* 从查找表获取比赛起止时间 */
        char key[32];
        snprintf(key, sizeof(key), "%d", contest_id);
        cJSON *cl_entry = cJSON_GetObjectItem(contest_lookup, key);
        long long start_time = 0;
        long long duration = 7200;
        if (cl_entry) {
            cJSON *st = cJSON_GetObjectItem(cl_entry, "startTimeSeconds");
            if (st && cJSON_IsNumber(st)) start_time = (long long)st->valuedouble;
            cJSON *dur = cJSON_GetObjectItem(cl_entry, "durationSeconds");
            if (dur && cJSON_IsNumber(dur)) duration = (long long)dur->valuedouble;
        }

        cJSON_AddNumberToObject(contest_obj, "startTime",
            (double)(start_time ? start_time : update_time));

        /* 赛前/赛后 rating */
        cJSON *oldr = cJSON_GetObjectItem(item, "oldRating");
        cJSON *newr = cJSON_GetObjectItem(item, "newRating");
        int old_rating = oldr && cJSON_IsNumber(oldr) ? oldr->valueint : 0;
        int new_rating = newr && cJSON_IsNumber(newr) ? newr->valueint : 0;

        cJSON_AddNumberToObject(contest_obj, "oldRating", old_rating);
        cJSON_AddNumberToObject(contest_obj, "newRating", new_rating);
        cJSON_AddStringToObject(contest_obj, "oldRatingColor", rating_color(old_rating));
        cJSON_AddStringToObject(contest_obj, "newRatingColor", rating_color(new_rating));

        cJSON *rankj2 = cJSON_GetObjectItem(item, "rank");
        int rank_val = rankj2 && cJSON_IsNumber(rankj2) ? rankj2->valueint : 0;
        cJSON_AddNumberToObject(contest_obj, "rank", rank_val);

        /* 近 180 天活跃度 */
        if (now_ts - update_time <= halfyear_sec) {
            contest_count_180++;
            if (new_rating > max_rating_180) max_rating_180 = new_rating;
        }

        /* 赛时/补题判定 */
        long long contest_end = start_time + duration;
        ProblemState probs[MAX_PROBLEMS] = {{0}};
        int prob_cnt = process_contest_problems(status, contest_id,
            contest_end, probs, MAX_PROBLEMS);

        /* 构建题目状态数组 */
        int solved_count = 0;
        cJSON *problems_arr = cJSON_AddArrayToObject(contest_obj, "problems");
        cJSON *upsolved_arr = cJSON_AddArrayToObject(contest_obj, "upsolved");

        for (int i = 0; i < prob_cnt; i++) {
            if (probs[i].in_contest_ac) {
                /* 赛时 AC → 绿色 */
                solved_count++;
                cJSON *prob = cJSON_CreateObject();
                cJSON_AddStringToObject(prob, "index", probs[i].index);
                cJSON_AddNumberToObject(prob, "rejectedAttempts", probs[i].rejected);
                cJSON_AddNumberToObject(prob, "problemRating", probs[i].problem_rating);
                cJSON_AddStringToObject(prob, "verdict", "OK");
                cJSON_AddItemToArray(problems_arr, prob);
            } else if (probs[i].rejected > 0) {
                /* 有提交但未 AC → 红色 */
                cJSON *prob = cJSON_CreateObject();
                cJSON_AddStringToObject(prob, "index", probs[i].index);
                cJSON_AddNumberToObject(prob, "rejectedAttempts", probs[i].rejected);
                cJSON_AddNumberToObject(prob, "problemRating", probs[i].problem_rating);
                cJSON_AddStringToObject(prob, "verdict", "FAIL");
                cJSON_AddItemToArray(problems_arr, prob);
            }

            /* 赛后补题 AC → 黄色 */
            if (probs[i].post_ac && !probs[i].in_contest_ac) {
                cJSON *us = cJSON_CreateObject();
                cJSON_AddStringToObject(us, "index", probs[i].index);
                cJSON_AddNumberToObject(us, "problemRating", probs[i].problem_rating);
                cJSON_AddItemToArray(upsolved_arr, us);
            }
        }

        cJSON_AddNumberToObject(contest_obj, "solvedCount", solved_count);
        cJSON_AddItemToArray(contests_arr, contest_obj);
    }

    /* ═══════════════════════════════════════════
     * 第三部分：统计数据 + AC 难度直方图
     * ═══════════════════════════════════════════ */

    cJSON_AddNumberToObject(result, "contestCount", contest_count);
    cJSON_AddNumberToObject(result, "contestCount180", contest_count_180);
    cJSON_AddNumberToObject(result, "maxRating180", max_rating_180);

    cJSON *hist_all = cJSON_AddObjectToObject(result, "histAll");
    cJSON *hist_year = cJSON_AddObjectToObject(result, "histYear");
    cJSON *hist_180 = cJSON_AddObjectToObject(result, "hist180");
    cJSON *hist_month = cJSON_AddObjectToObject(result, "histMonth");

    if (status) {
        count_ac_by_rating(status, hist_all,   now_ts, 0);
        count_ac_by_rating(status, hist_year,  now_ts, year_sec);
        count_ac_by_rating(status, hist_180,   now_ts, halfyear_sec);
        count_ac_by_rating(status, hist_month, now_ts, month_sec);
    }

    /* 释放临时数据 */
    cJSON_Delete(user_info);
    cJSON_Delete(rating_history);
    if (status) cJSON_Delete(status);
    if (contest_list) cJSON_Delete(contest_list);
    cJSON_Delete(contest_lookup);

    return result;
}
