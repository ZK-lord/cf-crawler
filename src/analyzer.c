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

/*
 * 根据 rating 数值返回对应的十六进制颜色值。
 * 实现 CF 官方 10 级颜色体系，从 Newbie 灰到 Legendary Grandmaster 黑。
 * 返回值是指向静态字符串常量的指针，不需要调用者释放。
 */
const char *rating_color(int rating) {
    if (rating >= 3000) return "#000000";  /* Legendary Grandmaster — 黑色 */
    if (rating >= 2600) return "#CC0000";  /* International Grandmaster — 深红 */
    if (rating >= 2400) return "#FF0000";  /* Grandmaster — 红色 */
    if (rating >= 2300) return "#FF8C00";  /* International Master — 橙色 */
    if (rating >= 2100) return "#FF8C00";  /* Master — 橙色 */
    if (rating >= 1900) return "#AA00AA";  /* Candidate Master — 紫色 */
    if (rating >= 1600) return "#0000FF";  /* Expert — 蓝色 */
    if (rating >= 1400) return "#03A89E";  /* Specialist — 青色 */
    if (rating >= 1200) return "#008000";  /* Pupil — 绿色 */
    return "#808080";                      /* Newbie — 灰色 */
}

/*
 * 根据 rating 数值返回对应的英文头衔字符串。
 * 用于修正 CF API 偶尔返回的错误 rank 字段。
 */
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
 * 构建 比赛ID → {名称, 开始时间, 时长} 的查找表。
 *
 * contest.list 包含所有比赛的元数据，但 rating 历史记录里只给了
 * contestId 这一数字编号。用这个查找表可以在后续处理中以 O(1)
 * 取得比赛的名称和起止时间，用于赛时/补题判定。
 *
 * 参数：
 *   contest_list — cJSON 数组，contest.list API 的 result 字段
 * 返回：
 *   新建的 cJSON 对象（即使 contest_list 为空也返回空对象），
 *   调用者负责 cJSON_Delete()。
 */
static cJSON *build_contest_lookup(cJSON *contest_list) {
    /* 创建一个空对象作为查找表 */
    cJSON *lookup = cJSON_CreateObject();
    if (!contest_list || !lookup) return lookup;

    cJSON *item;
    cJSON_ArrayForEach(item, contest_list) {
        /* 提取比赛 id，作为查找表的 key */
        cJSON *id = cJSON_GetObjectItem(item, "id");
        if (!id || !cJSON_IsNumber(id)) continue;

        /* 将 id 转为字符串 key（cJSON 对象的 key 必须是字符串） */
        char key[32];
        snprintf(key, sizeof(key), "%d", id->valueint);

        /* 在当前查找表中创建一个该比赛对应的子对象 */
        cJSON *entry = cJSON_CreateObject();
        entry = cJSON_AddObjectToObject(lookup, key);

        /* 写入比赛名称（name 字段可能不存在，默认空串） */
        cJSON *name = cJSON_GetObjectItem(item, "name");
        cJSON_AddStringToObject(entry, "name",
            name && name->valuestring ? name->valuestring : "");

        /* 写入比赛开始时间戳（startTimeSeconds 字段） */
        cJSON *st = cJSON_GetObjectItem(item, "startTimeSeconds");
        cJSON_AddNumberToObject(entry, "startTimeSeconds",
            st && cJSON_IsNumber(st) ? st->valuedouble : 0);

        /* 写入比赛持续时长，默认为标准 2 小时（7200 秒） */
        cJSON *dur = cJSON_GetObjectItem(item, "durationSeconds");
        cJSON_AddNumberToObject(entry, "durationSeconds",
            dur && cJSON_IsNumber(dur) ? dur->valuedouble : 7200);
    }
    return lookup;
}

/* 一场比赛最多 26 题（A-Z），超出视为异常 */
#define MAX_PROBLEMS 26

/*
 * 单道题目的提交状态追踪结构体。
 * 每道题目在一次比赛中对应一个 ProblemState 实例，
 * 保存题号、赛中/赛后状态、错误次数、难度分等信息。
 */
typedef struct {
    char index[4];        /* 题号字符串，如 "A", "B1", "C" */
    int in_contest_ac;    /* 赛时是否已通过（verdict=="OK" 且时间在比赛内） */
    int post_ac;          /* 赛后是否补题通过（verdict=="OK" 但时间在比赛后） */
    int rejected;         /* 错误提交次数（verdict!="OK" 的次数） */
    int problem_rating;   /* 题目难度分（800-3500，0 表示未评级） */
    int seen;             /* 标记：该题是否已被处理（保留字段） */
} ProblemState;

/*
 * 按题号字母序排序的比较函数。
 * 用于 qsort，确保最终输出的题目按 A, B, C... 的顺序排列。
 */
static int prob_cmp(const void *a, const void *b) {
    const ProblemState *pa = (const ProblemState *)a;
    const ProblemState *pb = (const ProblemState *)b;
    return strcmp(pa->index, pb->index);
}

/*
 * 赛时/补题判定算法 —— 本模块核心功能。
 *
 * 原理：
 *   每条提交记录有一个 creationTimeSeconds（提交时间戳）。
 *   比赛结束时间 contest_end = 比赛开始时间 + 比赛持续时长。
 *   通过比较提交时间与比赛结束时间，判断提交发生在赛中还是赛后。
 *
 * 判定规则：
 *   sub_time <= contest_end  →  赛内提交
 *     → verdict == "OK"      →  赛时通过（前端显示为绿色块）
 *     → verdict != "OK"      →  记录错误次数（前端显示为红色块）
 *
 *   sub_time > contest_end   →  赛后提交
 *     → verdict == "OK"      →  补题通过（前端显示为黄色块）
 *
 * 参数：
 *   status      — cJSON 数组，user.status API 返回的全部提交记录
 *   contest_id  — 整数，目标比赛的 id
 *   contest_end — 整数，该比赛的结束时间戳（开始时间 + 持续时长）
 *   probs       — 输出参数，ProblemState 数组，存放各题目的判定结果
 *   max_probs   — probs 数组的最大容量（通常为 MAX_PROBLEMS = 26）
 *
 * 返回：
 *   涉及的题目数量（即 probs 中有效元素个数）
 */
static int process_contest_problems(cJSON *status, int contest_id,
                                    long long contest_end,
                                    ProblemState *probs, int max_probs) {
    /* 定义并初始化计数器，记录目前已存入 probs 数组的不同题目数量 */
    int prob_count = 0;

    /* 检查传入的 status 指针（应指向 cJSON 数组）是否为空。
       若为空，直接返回 0，表示没有处理任何题目 */
    if (!status) return 0;

    cJSON *sub;
    /* 遍历 status 数组中的每一个元素，每个元素代表一次提交记录 */
    cJSON_ArrayForEach(sub, status) {

        /* 从当前提交中获取 contestId 字段。
           如果该字段不存在或其整数值不等于目标 contest_id，
           则跳过这条提交（只处理属于本场比赛的提交） */
        cJSON *scid = cJSON_GetObjectItem(sub, "contestId");
        if (!scid || scid->valueint != contest_id) continue;

        /* 获取提交中的 problem 子对象，再从中取出 index 字段（题号）。
           如果 problem 或 index 不存在，跳过该条提交 */
        cJSON *sproblem = cJSON_GetObjectItem(sub, "problem");
        if (!sproblem) continue;
        cJSON *sindex = cJSON_GetObjectItem(sproblem, "index");
        if (!sindex) continue;

        /* 在已记录的题目数组 probs 中查找是否已有相同 index 的题目。
           probs[i].index 是之前存入的题目标号，
           sindex->valuestring 是当前提交的题目标号。
           若找到，记录该题在数组中的下标 idx */
        int idx = -1;
        for (int i = 0; i < prob_count; i++) {
            if (strcmp(probs[i].index, sindex->valuestring) == 0) {
                idx = i;
                break;
            }
        }

        /* 若未找到且数组还有空间，在 probs 末尾创建新条目。
           初始化题号为新题目的 index，所有状态置零 */
        if (idx < 0 && prob_count < max_probs) {
            idx = prob_count++;  /* 使用当前计数作为下标，然后自增 */
            strncpy(probs[idx].index, sindex->valuestring, sizeof(probs[idx].index) - 1);
            probs[idx].index[sizeof(probs[idx].index) - 1] = '\0';  /* 确保字符串终止 */
            probs[idx].in_contest_ac = 0;   /* 赛时未通过 */
            probs[idx].post_ac = 0;         /* 赛后未补题 */
            probs[idx].rejected = 0;        /* 无错误提交 */
            probs[idx].problem_rating = 0;  /* 难度分未知 */
            probs[idx].seen = 1;            /* 标记已处理 */

            /* 尝试获取题目的难度分（rating 字段可能不存在，
               如 Codeforces 上某些题目标题没有 rating 评分） */
            cJSON *prating = cJSON_GetObjectItem(sproblem, "rating");
            if (prating && cJSON_IsNumber(prating))
                probs[idx].problem_rating = prating->valueint;
        }

        /* 数组已满且未找到对应题目，跳过此提交 */
        if (idx < 0) continue;

        /* 获取提交的判定结果（verdict 字段）。
           如 "OK" 表示通过，"WRONG_ANSWER" 等表示失败 */
        cJSON *sverdict = cJSON_GetObjectItem(sub, "verdict");
        if (!sverdict) continue;

        /* 获取提交的发生时间戳（creationTimeSeconds 字段） */
        cJSON *sctime = cJSON_GetObjectItem(sub, "creationTimeSeconds");
        long long sub_time = sctime && cJSON_IsNumber(sctime) ?
            (long long)sctime->valuedouble : 0;

        /* ——— 核心判定逻辑 ———
           比较提交时间 sub_time 与比赛结束时间 contest_end：
           - 若 sub_time <= contest_end：提交发生在比赛期间
             - verdict == "OK"   → 赛中通过
             - verdict != "OK"   → 赛中失败（记录错误次数）
           - 若 sub_time > contest_end：提交发生在比赛结束后
             - verdict == "OK"   → 赛后补题通过 */
        if (strcmp(sverdict->valuestring, "OK") == 0) {
            if (sub_time > 0 && sub_time <= contest_end) {
                probs[idx].in_contest_ac = 1;  /* 标记：赛时通过了该题 */
            } else {
                probs[idx].post_ac = 1;         /* 标记：赛后补题通过了该题 */
            }
        } else {
            /* verdict 不是 OK，记录一次错误提交次数 */
            probs[idx].rejected++;
        }
    }

    /* 按题号字典序排序，确保输出时 A, B, C... 顺序一致 */
    qsort(probs, prob_count, sizeof(ProblemState), prob_cmp);
    return prob_count;
}

/*
 * 统计 AC 题目按难度分值的分布（直方图数据）。
 *
 * 遍历所有提交记录，筛选出 verdict=="OK" 的 AC 记录，
 * 以 problem.rating 为 key 在 cJSON 对象 hist 中累加计数。
 * 支持通过 window_sec 参数限制时间范围。
 *
 * 同一份 status 数据会被调用 4 次，每次传入不同的 window_sec，
 * 生成 4 个维度的直方图：
 *   window_sec = 0           → 全部历史
 *   window_sec = 365*86400   → 最近一年
 *   window_sec = 180*86400   → 最近 180 天
 *   window_sec = 30*86400    → 最近一个月
 *
 * 参数：
 *   status     — 用户全部提交记录（cJSON 数组）
 *   hist       — 输出参数，直方图数据（cJSON 对象，key=rating, value=AC数）
 *   now_ts     — 当前时间戳，用于计算窗口
 *   window_sec — 时间窗口长度（秒），0 表示不限时间
 *
 * 注意：
 *   cJSON 累加计数时，必须同时更新 valueint 和 valuedouble 两个字段，
 *   否则 cJSON_Print 序列化时两者不一致，会输出错误的旧值。
 */
static void count_ac_by_rating(cJSON *status, cJSON *hist,
                               long long now_ts, long long window_sec) {
    /* 如果提交记录为空，直方图保留为空对象 */
    if (!status) return;

    cJSON *sub;
    /* 遍历该用户的全部提交记录 */
    cJSON_ArrayForEach(sub, status) {
        /* 只统计 AC 的提交（verdict 字段值必须为 "OK"） */
        cJSON *verdict = cJSON_GetObjectItem(sub, "verdict");
        if (!verdict || strcmp(verdict->valuestring, "OK") != 0) continue;

        /* 获取提交时间，用于时间窗口过滤 */
        cJSON *ctime = cJSON_GetObjectItem(sub, "creationTimeSeconds");
        if (!ctime) continue;

        /* 如果指定了时间窗口，过滤掉超出窗口的旧提交 */
        long long ts = (long long)ctime->valuedouble;
        if (window_sec > 0 && now_ts - ts > window_sec) continue;

        /* 获取题目的 problem 子对象，再从中取 rating 难度分 */
        cJSON *problem = cJSON_GetObjectItem(sub, "problem");
        if (!problem) continue;
        cJSON *prating = cJSON_GetObjectItem(problem, "rating");
        int r = prating ? prating->valueint : 0;
        /* r = 0 表示该题无难度评级，也归入统计 */

        /* 以 rating 数值的字符串形式作为直方图 key，
           在 hist 对象中累加该难度段的 AC 计数 */
        char key[16];
        snprintf(key, sizeof(key), "%d", r);
        cJSON *existing = cJSON_GetObjectItem(hist, key);
        if (existing) {
            /* 该难度段已有记录，递增计数。
               关键：valueint 和 valuedouble 必须同时更新，
               否则 cJSON_Print 会输出过时的值 */
            existing->valueint++;
            existing->valuedouble = existing->valueint;
        } else {
            /* 首次遇到该难度段，创建新条目，初始计数为 1 */
            cJSON_AddNumberToObject(hist, key, 1);
        }
    }
}

/*
 * 修正 CF API 返回的 rank / maxRank 字段。
 *
 * CF API 偶尔返回错误的值（例如 tourist 的 maxRank 字段返回了
 * 他的用户名 "tourist" 而不是头衔 "Legendary Grandmaster"）。
 * 这里忽略 API 返回的原始 rank 值，根据 rating 数值重新计算
 * 正确的头衔字符串，确保前端展示正确。
 *
 * 参数：
 *   user — user.info API 返回的用户信息对象（原地修改）
 */
static void adjust_rating_rank(cJSON *user) {
    /* 提取当前 rating 和历史最高 rating */
    int rating = 0, maxRating = 0;
    cJSON *r = cJSON_GetObjectItem(user, "rating");
    if (r && cJSON_IsNumber(r)) rating = r->valueint;
    cJSON *mr = cJSON_GetObjectItem(user, "maxRating");
    if (mr && cJSON_IsNumber(mr)) maxRating = mr->valueint;

    /* 删除 API 返回的原始 rank（可能包含错误值），
       然后用 rating_title() 根据数值重新计算并写入正确的头衔 */
    cJSON_DeleteItemFromObject(user, "rank");
    cJSON_AddStringToObject(user, "rank", rating_title(rating));

    /* 同样处理历史最高分对应的头衔 */
    cJSON_DeleteItemFromObject(user, "maxRank");
    cJSON_AddStringToObject(user, "maxRank", rating_title(maxRating));
}

/*
 * 构建单个用户的完整分析数据。
 *
 * 这是 analyzer 模块的主入口函数，整合所有子功能：
 *   1. 读取 4 个缓存文件（user info, rating 历史, 提交记录, 比赛列表）
 *   2. 修正 rank 数据
 *   3. 遍历每场比赛，调用 process_contest_problems 进行赛时/补题判定
 *   4. 统计近 180 天活跃度
 *   5. 调用 count_ac_by_rating 生成 4 个维度的 AC 难度分布直方图
 *
 * 返回值是一个 cJSON 对象树，结构如下：
 *   {
 *     handle, rating, maxRating, rank, maxRank, avatar, titlePhoto,
 *     ratingColor,
 *     contestCount,          // 总参赛次数
 *     contestCount180,       // 近 180 天参赛次数
 *     maxRating180,          // 近 180 天最高 rating
 *     contests: [{           // 每场比赛的详细记录
 *       contestId, contestName, timestamp, startTime,
 *       oldRating, newRating, oldRatingColor, newRatingColor, rank,
 *       solvedCount,
 *       problems: [{index, rejectedAttempts, problemRating, verdict}, ...],  // 赛中题目
 *       upsolved: [{index, problemRating}, ...]                              // 赛后补题
 *     }, ...],
 *     histAll:   {"800": 5, "1200": 3, ...},   // 全部历史 AC 难度分布
 *     histYear:  {"800": 2, "1200": 1, ...},   // 近一年
 *     hist180:   {"800": 1, "1200": 0, ...},   // 近 180 天
 *     histMonth: {"800": 0, "1200": 0, ...}    // 近一月
 *   }
 *
 * 参数：
 *   handle        — CF 用户名
 *   data_dir      — 缓存数据目录（e.g. "data"）
 *   output_dir    — 输出目录（当前未使用，保留接口兼容性）
 *   max_standings — 最多处理的排行榜数量（当前未使用）
 *
 * 返回：
 *   成功返回 cJSON 对象（调用者负责 cJSON_Delete），失败返回 NULL。
 */
cJSON *build_user_summary(const char *handle, const char *data_dir,
                          const char *output_dir, int max_standings) {
    /* 保留参数的引用，消除编译警告（输出目录和排行榜数量参数暂未使用） */
    (void)output_dir;
    (void)max_standings;

    /* 创建最终返回的结果对象 */
    cJSON *result = cJSON_CreateObject();
    if (!result) return NULL;

    /* ——— 构建 4 个缓存文件的完整路径 ——— */
    char user_path[512], rating_path[512], status_path[512], clist_path[512];
    snprintf(user_path, sizeof(user_path), "%s/%s_user.json", data_dir, handle);
    snprintf(rating_path, sizeof(rating_path), "%s/%s_rating.json", data_dir, handle);
    snprintf(status_path, sizeof(status_path), "%s/%s_status.json", data_dir, handle);
    snprintf(clist_path, sizeof(clist_path), "%s/contest_list.json", data_dir);

    /* 加载用户基本信息（必需）。
       load_cf_result_first 从 CF API 标准响应 {"status":"OK","result":[{...}]}
       中提取 result 数组的第一个元素，返回其深拷贝 */
    cJSON *user_info = load_cf_result_first(user_path);
    if (!user_info) {
        fprintf(stderr, "  [ERROR] Cannot load user info for %s\n", handle);
        cJSON_Delete(result);
        return NULL;
    }

    /* 加载 rating 变化历史（必需）。
       load_cf_result_array 提取 result 数组的深拷贝 */
    cJSON *rating_history = load_cf_result_array(rating_path);
    if (!rating_history) {
        fprintf(stderr, "  [ERROR] Cannot load rating history for %s\n", handle);
        cJSON_Delete(user_info);
        cJSON_Delete(result);
        return NULL;
    }

    /* 加载提交记录和比赛列表（可选，不存在时继续处理但直方图和题目状态为空） */
    cJSON *status = load_cf_result_array(status_path);
    cJSON *contest_list = load_cf_result_array(clist_path);

    /* 定义时间窗口常量（秒数）。
       用 long long 避免 32 位 int 溢出（30*86400=2592000，安全；
       但 365*86400=31536000，用 64 位更稳妥） */
    time_t now = time(NULL);
    long long now_ts = (long long)now;
    long long year_sec = 365LL * 86400;
    long long halfyear_sec = 180LL * 86400;
    long long month_sec = 30LL * 86400;

    /* 修正 API 可能返回错误的 rank / maxRank 字段 */
    adjust_rating_rank(user_info);

    /* ================================================================
     *   第一部分：提取用户基本信息并写入 result
     * ================================================================ */

    /* 用户名（handle）。如果 API 返回中有该字段就用它，否则退回到传入参数 */
    cJSON *cf_handle = cJSON_GetObjectItem(user_info, "handle");
    cJSON_AddStringToObject(result, "handle",
        cf_handle && cf_handle->valuestring ? cf_handle->valuestring : handle);

    /* 用户头像 URL（avatar 字段） */
    cJSON *avatar = cJSON_GetObjectItem(user_info, "avatar");
    cJSON_AddStringToObject(result, "avatar",
        avatar && avatar->valuestring ? avatar->valuestring : "");

    /* 用户标题照片 URL（titlePhoto 字段，CF 上头像框等装饰图） */
    cJSON *titlePhoto = cJSON_GetObjectItem(user_info, "titlePhoto");
    cJSON_AddStringToObject(result, "titlePhoto",
        titlePhoto && titlePhoto->valuestring ? titlePhoto->valuestring : "");

    /* 当前 rating 和历史最高 rating（整数值） */
    int rating = 0, maxRating = 0;
    cJSON *r = cJSON_GetObjectItem(user_info, "rating");
    if (r && cJSON_IsNumber(r)) rating = r->valueint;
    cJSON *mr = cJSON_GetObjectItem(user_info, "maxRating");
    if (mr && cJSON_IsNumber(mr)) maxRating = mr->valueint;

    cJSON_AddNumberToObject(result, "rating", rating);
    cJSON_AddNumberToObject(result, "maxRating", maxRating);

    /* 当前 rating 对应的颜色（用于前端 CSS 着色） */
    cJSON_AddStringToObject(result, "ratingColor", rating_color(rating));

    /* 当前头衔。如果 API 返回了 rank 字段就用它，否则显示 "Unrated" */
    const char *rank_str = "Unrated";
    cJSON *rankj = cJSON_GetObjectItem(user_info, "rank");
    if (rankj && rankj->valuestring) rank_str = rankj->valuestring;
    cJSON_AddStringToObject(result, "rank", rank_str);

    /* 历史最高头衔，同理处理 */
    const char *maxRankStr = "Unrated";
    cJSON *maxRankj = cJSON_GetObjectItem(user_info, "maxRank");
    if (maxRankj && maxRankj->valuestring) maxRankStr = maxRankj->valuestring;
    cJSON_AddStringToObject(result, "maxRank", maxRankStr);

    /* ================================================================
     *   第二部分：遍历 rating 历史，逐场比赛分析
     *   每场比赛都会调用 process_contest_problems 进行赛时/补题判定
     * ================================================================ */

    /* 构建比赛 ID 查找表（即使 contest_list 为空也创建一个空对象，
       避免后续查找时出现空指针） */
    cJSON *contest_lookup = build_contest_lookup(contest_list);
    if (!contest_lookup) contest_lookup = cJSON_CreateObject();

    /* 初始化统计变量 */
    int contest_count = cJSON_GetArraySize(rating_history);  /* 历史总参赛次数 */
    int contest_count_180 = 0;    /* 近 180 天参赛次数 */
    int max_rating_180 = 0;      /* 近 180 天最高 rating */

    /* 在 result 中创建 contests 数组，用于存放每场比赛的分析结果 */
    cJSON *contests_arr = cJSON_AddArrayToObject(result, "contests");

    cJSON *item;
    /* 遍历 rating 历史中的每一场比赛记录 */
    cJSON_ArrayForEach(item, rating_history) {
        /* 为当前比赛创建一个分析结果对象 */
        cJSON *contest_obj = cJSON_CreateObject();

        /* 提取比赛 ID（整数） */
        int contest_id = 0;
        cJSON *cid = cJSON_GetObjectItem(item, "contestId");
        if (cid && cJSON_IsNumber(cid)) contest_id = cid->valueint;

        /* 提取 rating 更新时间戳（ratingUpdateTimeSeconds 字段）。
           用 long long 存储，避免时间戳超出 32 位 int 范围 */
        long long update_time = 0;
        cJSON *ctime = cJSON_GetObjectItem(item, "ratingUpdateTimeSeconds");
        if (ctime && cJSON_IsNumber(ctime)) update_time = (long long)ctime->valuedouble;

        /* 写入比赛基本元数据 */
        cJSON_AddNumberToObject(contest_obj, "contestId", contest_id);

        /* 比赛名称 */
        cJSON *cname = cJSON_GetObjectItem(item, "contestName");
        cJSON_AddStringToObject(contest_obj, "contestName",
            cname && cname->valuestring ? cname->valuestring : "");

        /* rating 更新时间（作为前端显示的时间参考） */
        cJSON_AddNumberToObject(contest_obj, "timestamp", (double)update_time);

        /* 从查找表中获取该比赛的开始时间和持续时长 */
        char key[32];
        snprintf(key, sizeof(key), "%d", contest_id);
        cJSON *cl_entry = cJSON_GetObjectItem(contest_lookup, key);

        long long start_time = 0;     /* 比赛开始时间戳 */
        long long duration = 7200;    /* 默认 7200 秒 = 2 小时 */
        if (cl_entry) {
            cJSON *st = cJSON_GetObjectItem(cl_entry, "startTimeSeconds");
            if (st && cJSON_IsNumber(st)) start_time = (long long)st->valuedouble;

            cJSON *dur = cJSON_GetObjectItem(cl_entry, "durationSeconds");
            if (dur && cJSON_IsNumber(dur)) duration = (long long)dur->valuedouble;
        }

        /* 写入比赛开始时间。如果查找表没有数据，
           退而求其次使用 rating 更新时间作为近似值 */
        cJSON_AddNumberToObject(contest_obj, "startTime",
            (double)(start_time ? start_time : update_time));

        /* 提取赛前/赛后 rating 值，计算本场变化 */
        cJSON *oldr = cJSON_GetObjectItem(item, "oldRating");
        cJSON *newr = cJSON_GetObjectItem(item, "newRating");
        int old_rating = oldr && cJSON_IsNumber(oldr) ? oldr->valueint : 0;
        int new_rating = newr && cJSON_IsNumber(newr) ? newr->valueint : 0;

        /* 写入 rating 变化数据及对应颜色 */
        cJSON_AddNumberToObject(contest_obj, "oldRating", old_rating);
        cJSON_AddNumberToObject(contest_obj, "newRating", new_rating);
        cJSON_AddStringToObject(contest_obj, "oldRatingColor", rating_color(old_rating));
        cJSON_AddStringToObject(contest_obj, "newRatingColor", rating_color(new_rating));

        /* 比赛排名。rank 是比赛结束时的名次 */
        cJSON *rankj2 = cJSON_GetObjectItem(item, "rank");
        int rank_val = rankj2 && cJSON_IsNumber(rankj2) ? rankj2->valueint : 0;
        cJSON_AddNumberToObject(contest_obj, "rank", rank_val);

        /* ——— 近 180 天活跃度统计 ———
           如果该比赛的更新时间在最近 180 天内，累加参赛次数，
           并更新这个窗口内的最高 rating */
        if (now_ts - update_time <= halfyear_sec) {
            contest_count_180++;
            if (new_rating > max_rating_180) max_rating_180 = new_rating;
        }

        /* ——— 调用赛时/补题判定算法 ———
           计算比赛结束时间：开始时间 + 持续时长 */
        long long contest_end = start_time + duration;

        /* 在栈上分配 ProblemState 数组并初始化全零。
           {{0}} 语法确保所有字段初始值为 0 */
        ProblemState probs[MAX_PROBLEMS] = {{0}};

        /* 调用核心判定函数，返回本场比赛涉及的题目数量 */
        int prob_cnt = process_contest_problems(status, contest_id,
            contest_end, probs, MAX_PROBLEMS);

        /* 将判定结果分类写入 JSON：
           - problems 数组  →  赛中涉及的题目（通过或失败，前端显示在比赛区）
           - upsolved 数组 →  赛后补题通过的题目（前端显示在补题区） */
        int solved_count = 0;  /* 本场赛时 AC 数量 */
        cJSON *problems_arr = cJSON_AddArrayToObject(contest_obj, "problems");
        cJSON *upsolved_arr = cJSON_AddArrayToObject(contest_obj, "upsolved");

        /* 遍历本场比赛的每道题目，按状态分类写入 */
        for (int i = 0; i < prob_cnt; i++) {
            if (probs[i].in_contest_ac) {
                /* 赛时 AC → 前端渲染为绿色块，表示本题在比赛中通过 */
                solved_count++;
                cJSON *prob = cJSON_CreateObject();
                cJSON_AddStringToObject(prob, "index", probs[i].index);
                cJSON_AddNumberToObject(prob, "rejectedAttempts", probs[i].rejected);
                cJSON_AddNumberToObject(prob, "problemRating", probs[i].problem_rating);
                cJSON_AddStringToObject(prob, "verdict", "OK");
                cJSON_AddItemToArray(problems_arr, prob);
            } else if (probs[i].rejected > 0) {
                /* 有提交但全部失败（WA / TLE 等）→ 前端渲染为红色块 */
                cJSON *prob = cJSON_CreateObject();
                cJSON_AddStringToObject(prob, "index", probs[i].index);
                cJSON_AddNumberToObject(prob, "rejectedAttempts", probs[i].rejected);
                cJSON_AddNumberToObject(prob, "problemRating", probs[i].problem_rating);
                cJSON_AddStringToObject(prob, "verdict", "FAIL");
                cJSON_AddItemToArray(problems_arr, prob);
            }

            /* 赛后补题：赛时未通过，但赛后补做了且通过 → 前端渲染为黄色块 */
            if (probs[i].post_ac && !probs[i].in_contest_ac) {
                cJSON *us = cJSON_CreateObject();
                cJSON_AddStringToObject(us, "index", probs[i].index);
                cJSON_AddNumberToObject(us, "problemRating", probs[i].problem_rating);
                cJSON_AddItemToArray(upsolved_arr, us);
            }
        }

        /* 写入本场比赛的赛时 AC 数量 */
        cJSON_AddNumberToObject(contest_obj, "solvedCount", solved_count);

        /* 将本场比赛的分析结果添加到 contests 数组 */
        cJSON_AddItemToArray(contests_arr, contest_obj);
    }

    /* ================================================================
     *   第三部分：写入统计摘要 + 生成 4 个时间维度的 AC 难度直方图
     * ================================================================ */

    /* 写入汇总统计数据 */
    cJSON_AddNumberToObject(result, "contestCount", contest_count);
    cJSON_AddNumberToObject(result, "contestCount180", contest_count_180);
    cJSON_AddNumberToObject(result, "maxRating180", max_rating_180);

    /* 在 result 中创建 4 个直方图对象（key 为 histAll/histYear/hist180/histMonth）。
       AddObjectToObject 会在 result 中创建子对象并返回其指针 */
    cJSON *hist_all = cJSON_AddObjectToObject(result, "histAll");
    cJSON *hist_year = cJSON_AddObjectToObject(result, "histYear");
    cJSON *hist_180 = cJSON_AddObjectToObject(result, "hist180");
    cJSON *hist_month = cJSON_AddObjectToObject(result, "histMonth");

    /* 如果有提交记录，对 4 个时间窗口分别调用统计函数 */
    if (status) {
        count_ac_by_rating(status, hist_all,   now_ts, 0);             /* 不限时 → 全部历史 */
        count_ac_by_rating(status, hist_year,  now_ts, year_sec);      /* 365 天 → 近一年 */
        count_ac_by_rating(status, hist_180,   now_ts, halfyear_sec);  /* 180 天 → 近半年 */
        count_ac_by_rating(status, hist_month, now_ts, month_sec);     /* 30 天 → 近一月 */
    }

    /* 释放所有临时加载的数据。
       这些数据已通过 cJSON_Duplicate 深拷贝到 result 中，
       原始数据不再需要 */
    cJSON_Delete(user_info);
    cJSON_Delete(rating_history);
    if (status) cJSON_Delete(status);
    if (contest_list) cJSON_Delete(contest_list);
    cJSON_Delete(contest_lookup);

    return result;
}
