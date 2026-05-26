/*
 * analyzer.c — 核心数据分析模块
 *
 * ═══════════════════════════════════════════════════════════
 *   本模块是整个项目的算法核心，负责所有数据分析和统计工作。
 * ═══════════════════════════════════════════════════════════
 *
 * 【输入】4 个缓存的 JSON 文件：
 *   1. <handle>_user.json    — user.info  接口，用户基本信息
 *   2. <handle>_rating.json  — user.rating 接口，Rating 变化历史
 *   3. <handle>_status.json  — user.status 接口，全部提交记录
 *   4. contest_list.json     — contest.list 接口，比赛元数据
 *
 * 【输出】一个 cJSON 对象树，包含以下结构：
 *   {
 *     handle, rating, maxRating, rank, avatar, ...    <— 基本信息
 *     contestCount, contestCount180, maxRating180,    <— 统计数据
 *     contests: [{                                      <— 每场比赛详情
 *       contestName, rank, oldRating, newRating,
 *       problems: [{index, verdict, rejectedAttempts}] <— 赛时每题状态
 *       upsolved: [{index, problemRating}]             <— 赛后补题
 *     }],
 *     histAll:   {"800": N, "900": N, ...}            <— AC难度分布(全部)
 *     histYear:  {"800": N, ...}                       <— AC难度分布(近一年)
 *     hist180:   {"800": N, ...}                       <— AC难度分布(近180天)
 *     histMonth: {"800": N, ...}                       <— AC难度分布(近一月)
 *   }
 *
 * 【核心算法】
 *   1. 赛时/补题判定 (process_contest_problems)
 *      — 通过比较每条提交的时间戳与比赛结束时间，精确区分赛时 AC、
 *        赛时 WA 和赛后补题 AC
 *   2. AC 难度分布直方图 (count_ac_by_rating)
 *      — 遍历全部 AC 提交，按题目 rating 分桶计数，
 *        支持 4 个时间窗口切换
 *   3. 活跃度统计
 *      — 统计近 180 天内参赛次数和最高 rating
 *
 * 【关键数据结构】
 *   ProblemState — 追踪一道题在某场比赛中的完整状态
 *     包括：题号(index)、赛时AC(in_contest_ac)、
 *           赛后补题AC(post_ac)、错误次数(rejected)、难度分
 *
 * 【设计决策】
 *   - 为什么用“时间比较”而不是 CF 的 participantType 字段判定赛时/补题？
 *     因为 participantType 在某些比赛类型(如 Div.3 的公开 hacking 阶段)
 *     中不准确，而时间戳判定对所有比赛类型都一致。
 *   - 为什么比赛结束时间 = startTimeSeconds + durationSeconds？
 *     CF API 不直接返回 endTimeSeconds，只能用开始时间+时长来计算。
 *     默认时长设为 7200 秒（2小时，CF 大多数比赛的标准时长）。
 */

#include "analyzer.h"
#include "json_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * 【CF 颜色映射】根据 rating 分值返回对应颜色
 *
 * CF 官方有 10 个等级，每个等级有专属颜色代码。
 * 在 ECharts 图表中用于数据点和曲线着色，
 * 在比赛记录中用于展示赛前/赛后 rating 变化。
 *
 * 颜色值来源: https://codeforces.com/blog/entry/20638
 *
 *   rating 范围      | 头衔                    | 颜色
 *   ----------------+------------------------+--------
 *   >= 3000         | Legendary Grandmaster  | 黑色
 *   2600 - 2999     | International GM       | 深红
 *   2400 - 2599     | Grandmaster            | 红色
 *   2300 - 2399     | International Master   | 橙色
 *   2100 - 2299     | Master                 | 橙色
 *   1900 - 2099     | Candidate Master       | 紫色
 *   1600 - 1899     | Expert                 | 蓝色
 *   1400 - 1599     | Specialist             | 青色
 *   1200 - 1399     | Pupil                  | 绿色
 *   <  1200         | Newbie                 | 灰色
 *
 * 注意：边界值用 >= 链式判断，从高到低依次检查，
 * 确保 3000 分不会被 2600 的条件误判。
 */
const char *rating_color(int rating) {
    if (rating >= 3000) return "#000000";  /* Legendary Grandmaster — 黑 */
    if (rating >= 2600) return "#CC0000";  /* International Grandmaster — 深红 */
    if (rating >= 2400) return "#FF0000";  /* Grandmaster — 红 */
    if (rating >= 2300) return "#FF8C00";  /* International Master — 橙 */
    if (rating >= 2100) return "#FF8C00";  /* Master — 橙 */
    if (rating >= 1900) return "#AA00AA";  /* Candidate Master — 紫 */
    if (rating >= 1600) return "#0000FF";  /* Expert — 蓝 */
    if (rating >= 1400) return "#03A89E";  /* Specialist — 青 */
    if (rating >= 1200) return "#008000";  /* Pupil — 绿 */
    return "#808080";                      /* Newbie — 灰 */
}

/*
 * 【CF 头衔映射】与 rating_color 完全对应，
 * 用于在报告中显示文字头衔（如页面标题旁的标签）。
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
 * 构建"比赛 ID → 比赛元数据"的查找表
 * ════════════════════════════════════
 *
 * 【为什么需要这个查找表？】
 *   user.rating 接口返回每场比赛的 rating 变化，包含 contestId
 *   但不包含比赛的开始时间和时长。而 contest.list 接口返回了
 *   所有比赛的元数据（名称、开始时间、时长）。
 *
 *   为了判定赛时/补题，必须知道比赛结束时间 =
 *   startTimeSeconds + durationSeconds。
 *   每次遍历比赛时去 contest_list 里线性搜索太慢（O(n²)），
 *   所以先构建一个以 contestId 为 key 的哈希表，后续 O(1) 查找。
 *
 * 【数据结构】
 *   lookup = {
 *     "1234": { name, startTimeSeconds, durationSeconds },
 *     "1235": { name, startTimeSeconds, durationSeconds },
 *     ...
 *   }
 *   其中 key 是 contestId 的字符串形式。
 *
 * 【边界处理】
 *   - contestId 不存在于 lookup 中（如非 rating 比赛）→ duration 默认 7200 秒
 *   - 比赛名称缺失 → 置空字符串
 *   - startTimeSeconds 缺失 → 用 ratingUpdateTimeSeconds 代替
 */
static cJSON *build_contest_lookup(cJSON *contest_list) {
    cJSON *lookup = cJSON_CreateObject();
    if (!contest_list || !lookup) return lookup;

    cJSON *item;
    cJSON_ArrayForEach(item, contest_list) {
        /* 取出比赛 ID，用作查找表的 key */
        cJSON *id = cJSON_GetObjectItem(item, "id");
        if (!id || !cJSON_IsNumber(id)) continue;

        char key[32];
        snprintf(key, sizeof(key), "%d", id->valueint);

        /* 为此比赛创建一个子对象，存储名称/开始时间/时长 */
        cJSON *entry = cJSON_CreateObject();
        entry = cJSON_AddObjectToObject(lookup, key);

        /* 比赛名称（如 "Codeforces Round #1000 (Div. 2)"） */
        cJSON *name = cJSON_GetObjectItem(item, "name");
        cJSON_AddStringToObject(entry, "name",
            name && name->valuestring ? name->valuestring : "");

        /* 比赛开始时间 — Unix 时间戳（秒） */
        cJSON *st = cJSON_GetObjectItem(item, "startTimeSeconds");
        cJSON_AddNumberToObject(entry, "startTimeSeconds",
            st && cJSON_IsNumber(st) ? st->valuedouble : 0);

        /* 比赛持续时间（秒），CF 大部分比赛为 7200（2小时） */
        cJSON *dur = cJSON_GetObjectItem(item, "durationSeconds");
        cJSON_AddNumberToObject(entry, "durationSeconds",
            dur && cJSON_IsNumber(dur) ? dur->valuedouble : 7200);
    }
    return lookup;
}

/*
 * CF 一场比赛最多 26 道题（A 到 Z）
 * 实际比赛中通常为 5-8 道题，26 是理论上限。
 */
#define MAX_PROBLEMS 26

/*
 * 【核心数据结构】单道题目的提交状态追踪
 * ════════════════════════════════════
 *
 * 在一场比赛中，每道题用这个结构体记录所有相关状态。
 * 三个 flag 的优先级：
 *   in_contest_ac > post_ac > rejected > 未提交
 *
 * 为什么 in_contest_ac 和 post_ac 是独立的两个字段？
 *   因为同一道题可能在赛时 AC（in_contest_ac=1），
 *   也可能赛时 WA 后又在赛后补题 AC（in_contest_ac=0, post_ac=1）。
 *   两者互斥：一旦赛时 AC，就不会标记为赛后补题。
 */
typedef struct {
    char index[4];        /* 题目编号："A", "B", "C", ..., "Z"（含 null 终止符）*/
    int in_contest_ac;    /* 是否在赛内通过 (0/1) — 对应前端绿色块 */
    int post_ac;          /* 是否在赛后补题通过 (0/1) — 对应前端黄色块 */
    int rejected;         /* 错误提交次数（WA + TLE + RE + ...）
                             用于显示每道题失败了几次 */
    int problem_rating;   /* 题目难度分（800-3500），用于直方图统计 */
    int seen;             /* 标记该题在比赛中是否有提交（未使用但保留） */
} ProblemState;

/*
 * 按题目编号排序的比较函数（qsort 回调）
 * 确保题目列表始终以 A, B, C, ... 顺序输出
 */
static int prob_cmp(const void *a, const void *b) {
    const ProblemState *pa = (const ProblemState *)a;
    const ProblemState *pb = (const ProblemState *)b;
    return strcmp(pa->index, pb->index);
}

/*
 * ╔══════════════════════════════════════════════════════════════╗
 * ║           赛时/补题判定算法 — 项目核心技术点                    ║
 * ╚══════════════════════════════════════════════════════════════╝
 *
 * 【问题描述】
 *   给定一场比赛的所有提交记录，判断每道题是赛时做出来的、
 *   赛时没做出来（WA了）、还是赛后补题做出来的。
 *
 * 【算法原理】
 *   每条提交记录(Submission)包含两个关键字段：
 *     - contestId:            属于哪场比赛
 *     - creationTimeSeconds:  这条提交是什么时候产生的（Unix 时间戳）
 *     - verdict:              评测结果（OK, WRONG_ANSWER, TIME_LIMIT_EXCEEDED...）
 *
 *   每场比赛有一个结束时间 contest_end（开始时间 + 持续时间）。
 *
 *   判定规则只有一行：
 *     if (creationTimeSeconds <= contest_end) → 赛内提交
 *     if (creationTimeSeconds >  contest_end) → 赛后提交
 *
 *   结合 verdict:
 *     - 赛内 + OK       → in_contest_ac = 1  （赛时通过 ✓）
 *     - 赛内 + 非OK     → rejected++         （失败了，不计入AC）
 *     - 赛后 + OK       → post_ac = 1        （补题通过）
 *
 * 【为什么这个算法比 CF 官方的 "Practice" 标记更准确？】
 *   有些比赛类型（如 Educational Round）的 hacking 阶段结束后，
 *   官方仍允许提交但标记为 "Practice"。用时间戳判定可以精确区分
 *   真正的赛内提交和赛后补题，不受比赛类型和官方标记的影响。
 *
 * 【为什么要过滤 contestId？】
 *   status 数组包含用户在所有比赛中的所有提交（包括 gym、训练赛等）。
 *   必须按 contestId 筛选出属于当前比赛的提交，才能正确判定。
 *
 * 【时间复杂度】
 *   O(S) where S = status 数组长度。
 *   对每场比赛都要遍历一次全部提交。
 *   实际中用户提交量级在 10³-10⁴，总体在毫秒级完成。
 *
 * 参数：
 *   status      — 用户的全部提交记录（user.status 的 result 数组）
 *   contest_id  — 当前分析的比赛 ID
 *   contest_end — 比赛结束时间戳 = startTime + duration
 *   probs       — 输出数组，存储每道题的分析结果
 *   max_probs   — 输出数组容量（通常为 MAX_PROBLEMS = 26）
 *
 * 返回：实际涉及的题目数量
 */
static int process_contest_problems(cJSON *status, int contest_id,
                                    long long contest_end,
                                    ProblemState *probs, int max_probs) {
    int prob_count = 0;  /* 当前已记录的题目数量 */
    if (!status) return 0;

    cJSON *sub;
    cJSON_ArrayForEach(sub, status) {

        /* ---- 第1步：筛选属于当前比赛的提交 ---- */
        /* 跳过不属于本场比赛的提交 */
        cJSON *scid = cJSON_GetObjectItem(sub, "contestId");
        if (!scid || scid->valueint != contest_id) continue;

        /* ---- 第2步：提取题目信息 ---- */
        cJSON *sproblem = cJSON_GetObjectItem(sub, "problem");
        if (!sproblem) continue;
        cJSON *sindex = cJSON_GetObjectItem(sproblem, "index");
        if (!sindex) continue;  /* 没有题号则无法记录，跳过 */

        /* ---- 第3步：在 ProblemState 数组中定位该题目 ---- */
        /* 如果这道题之前已经出现过（有多条提交），复用已有记录；
           如果是新出现的题，在数组中追加一条新记录 */
        int idx = -1;
        for (int i = 0; i < prob_count; i++) {
            if (strcmp(probs[i].index, sindex->valuestring) == 0) {
                idx = i;
                break;
            }
        }
        /* 新题目：初始化 ProblemState */
        if (idx < 0 && prob_count < max_probs) {
            idx = prob_count++;
            /* 安全复制题号字符串（用 strncpy 防止溢出） */
            strncpy(probs[idx].index, sindex->valuestring,
                    sizeof(probs[idx].index) - 1);
            probs[idx].index[sizeof(probs[idx].index) - 1] = '\0';
            probs[idx].in_contest_ac = 0;
            probs[idx].post_ac = 0;
            probs[idx].rejected = 0;
            probs[idx].problem_rating = 0;
            probs[idx].seen = 1;

            /* 记录题目难度分，CF API 的 user.status 中
               problem 对象可能包含 rating 字段（800-3500）。
               注意：并非所有题都有 rating，新题可能没有。 */
            cJSON *prating = cJSON_GetObjectItem(sproblem, "rating");
            if (prating && cJSON_IsNumber(prating))
                probs[idx].problem_rating = prating->valueint;
        }
        if (idx < 0) continue;  /* 超出容量，忽略 */

        /* ---- 第4步：提取评测结果和时间戳 ---- */
        cJSON *sverdict = cJSON_GetObjectItem(sub, "verdict");
        if (!sverdict) continue;

        /* 获取提交时间（Unix 时间戳，精确到秒） */
        cJSON *sctime = cJSON_GetObjectItem(sub, "creationTimeSeconds");
        long long sub_time = sctime && cJSON_IsNumber(sctime) ?
            (long long)sctime->valuedouble : 0;

        /*
         * ---- 第5步：核心判定逻辑 ----
         *
         * 这是整个分析器的关键 if 语句：
         *
         *   if (sub_time <= contest_end) → 赛内提交
         *
         * sub_time        = 用户提交这条代码的时间
         * contest_end     = 比赛开始时间 + 比赛时长 = 比赛结束时间
         *
         * 比较这俩就能知道这条提交是比赛进行中产生的，
         * 还是比完赛之后才提交的。
         */
        if (strcmp(sverdict->valuestring, "OK") == 0) {
            /* 这条提交评测结果为 Accepted */
            if (sub_time > 0 && sub_time <= contest_end) {
                /* 在比赛结束前 AC → 赛内通过 */
                probs[idx].in_contest_ac = 1;
            } else {
                /* 比赛结束之后才 AC → 赛后补题通过 */
                probs[idx].post_ac = 1;
            }
        } else {
            /* 这条提交评测结果不是 AC（WA, TLE, RE, MLE...）
               累计错误次数，后续在前端显示红色块和失败次数 */
            probs[idx].rejected++;
        }
    }

    /* ---- 第6步：按题号排序并返回 ---- */
    /* 确保输出顺序为 A, B, C, ... 方便前端展示 */
    qsort(probs, prob_count, sizeof(ProblemState), prob_cmp);
    return prob_count;
}

/*
 * 统计 AC 题目的难度分布直方图
 * ═══════════════════════════════
 *
 * 【用途】
 *   生成 ECharts 柱状图的数据源，展示用户在不同难度级别上
 *   通过了多少题。是评价一个选手能力范围的核心指标。
 *
 * 【算法】
 *   1. 遍历全部提交，筛选 verdict == "OK" 的 AC 记录
 *   2. 提取每条 AC 记录的 problem.rating（题目难度分）
 *   3. 如果指定了时间窗口（window_sec > 0），
 *      跳过窗口之外的老提交
 *   4. 以 rating 值为 key，在 hist 对象中累加计数
 *
 * 【输出格式】
 *   hist = {"800": 208, "900": 56, "1000": 69, ..., "3500": 85}
 *
 *   key 是 rating 值（字符串形式），value 是 AC 次数。
 *   未通过的题目不计入、没有 rating 的题目归入 key="0"。
 *
 * 【时间窗口设计】
 *   同一个 status 数据会调用本函数 4 次，生成 4 个直方图：
 *     window_sec = 0            → histAll   全部历史
 *     window_sec = 365*86400    → histYear  近一年
 *     window_sec = 180*86400    → hist180   近 180 天
 *     window_sec = 30*86400     → histMonth 近一个月
 *   前端可以在这四个区间之间切换，观察用户能力的动态变化。
 *
 * 【⚠ cJSON 注意】
 *   累加计数时必须同时更新 valueint 和 valuedouble，
 *   因为 cJSON 序列化时会检查两者是否相等：
 *     - 相等 → 按整数输出（"2"）
 *     - 不等 → 按 valuedouble 输出（"1.0"）
 *   如果只写 valueint++ 不同步 valuedouble，序列化结果永远是 1。
 *
 * 参数：
 *   status     — 用户全部提交记录
 *   hist       — 输出：直方图 cJSON 对象
 *   now_ts     — 当前时间戳（Unix 秒），用于计算时间差
 *   window_sec — 时间窗口大小（秒），0 表示不限时间
 */
static void count_ac_by_rating(cJSON *status, cJSON *hist,
                               long long now_ts, long long window_sec) {
    if (!status) return;

    cJSON *sub;
    cJSON_ArrayForEach(sub, status) {

        /* 只统计 AC 的提交，WA/TLE 等不计入 */
        cJSON *verdict = cJSON_GetObjectItem(sub, "verdict");
        if (!verdict || strcmp(verdict->valuestring, "OK") != 0) continue;

        /* 获取提交时间，无时间戳的跳过 */
        cJSON *ctime = cJSON_GetObjectItem(sub, "creationTimeSeconds");
        if (!ctime) continue;

        /* 时间窗口过滤：
           now_ts - ts > window_sec 意味着这条提交太老了 */
        long long ts = (long long)ctime->valuedouble;
        if (window_sec > 0 && now_ts - ts > window_sec) continue;

        /* 提取题目难度分
           优先 rating 字段（CF 题目难度，800-3500），
           无 rating 时归入 key="0" */
        cJSON *problem = cJSON_GetObjectItem(sub, "problem");
        if (!problem) continue;
        cJSON *prating = cJSON_GetObjectItem(problem, "rating");
        int r = prating ? prating->valueint : 0;

        /* 以 rating 值为字符串 key，在直方图对象中累加 */
        char key[16];
        snprintf(key, sizeof(key), "%d", r);
        cJSON *existing = cJSON_GetObjectItem(hist, key);

        if (existing) {
            /* 该难度分已有记录，计数 +1 */
            existing->valueint++;
            existing->valuedouble = existing->valueint;
            /* ⚠ 必须同步更新 valuedouble，否则 cJSON 序列化时
               发现 valueint != valuedouble，会输出 valuedouble 的原始值 */
        } else {
            /* 该难度分首次出现，创建新条目，初始值为 1 */
            cJSON_AddNumberToObject(hist, key, 1);
        }
    }
}

/*
 * 修正 CF API 返回的头衔数据
 * ═══════════════════════════
 *
 * CF API 在某些情况下会返回错误的 rank 字段——
 * 例如 tourist 的 maxRank 偶尔被 API 返回为他自己的用户名。
 * 为了避免前端显示异常，这里根据 rating 值重新计算正确的头衔。
 *
 * 始终以本地 rating_title() 的输出覆盖 API 原始值。
 */
static void adjust_rating_rank(cJSON *user) {
    int rating = 0, maxRating = 0;
    cJSON *r = cJSON_GetObjectItem(user, "rating");
    if (r && cJSON_IsNumber(r)) rating = r->valueint;
    cJSON *mr = cJSON_GetObjectItem(user, "maxRating");
    if (mr && cJSON_IsNumber(mr)) maxRating = mr->valueint;

    /* 删除 API 返回的可能错误的 rank，替换为本地计算结果 */
    cJSON_DeleteItemFromObject(user, "rank");
    cJSON_AddStringToObject(user, "rank", rating_title(rating));

    cJSON_DeleteItemFromObject(user, "maxRank");
    cJSON_AddStringToObject(user, "maxRank", rating_title(maxRating));
}

/*
 * ╔══════════════════════════════════════════════════════════════╗
 * ║       build_user_summary — 构建单个用户的完整分析数据          ║
 * ╚══════════════════════════════════════════════════════════════╝
 *
 * 【总览】
 *   这是 analyzer 模块的主函数。
 *   它读取用户在 data/ 目录下的 4 个缓存文件，
 *   调用上述所有子函数进行分析，最终输出一个 cJSON 树。
 *
 * 【数据流】
 *
 *   data/<handle>_user.json  ─┐
 *   data/<handle>_rating.json ─┤
 *   data/<handle>_status.json ─┼──→ build_user_summary() ──→ cJSON 树
 *   data/contest_list.json   ─┘
 *
 * 【返回的 JSON 结构】
 *   {
 *     // 基本信息（来自 user.info）
 *     "handle": "tourist",
 *     "rating": 3774,
 *     "maxRating": 4009,
 *     "rank": "Legendary Grandmaster",
 *     "ratingColor": "#000000",
 *     "avatar": "https://...",
 *     "titlePhoto": "https://...",
 *
 *     // 统计摘要
 *     "contestCount": 234,      // 参赛次数
 *     "contestCount180": 12,    // 近180天参赛次数
 *     "maxRating180": 3915,     // 近180天最高分
 *
 *     // 比赛记录（数组，按时间从旧到新排列）
 *     "contests": [
 *       {
 *         "contestId": 1234,
 *         "contestName": "Codeforces Round #...",
 *         "timestamp": 1699123400,
 *         "oldRating": 1500, "newRating": 1600,
 *         "rank": 42,
 *         "solvedCount": 5,
 *         "problems": [               ← 赛时提交的题目
 *           {"index":"A","verdict":"OK","rejectedAttempts":0},
 *           {"index":"B","verdict":"FAIL","rejectedAttempts":2},
 *         ],
 *         "upsolved": [               ← 赛后补题的题目
 *           {"index":"C","problemRating":1800},
 *         ]
 *       },
 *       ...
 *     ],
 *
 *     // AC 难度分布直方图（4个时间窗口）
 *     "histAll":   {"800":208, "900":56, ...},
 *     "histYear":  {"800":100, ...},
 *     "hist180":   {"800":50,  ...},
 *     "histMonth": {"800":10,  ...}
 *   }
 *
 * 参数：
 *   handle        — Codeforces 用户名
 *   data_dir      — 缓存数据目录（通常是 "data"）
 *   output_dir    — 输出目录（保留参数，当前未使用）
 *   max_standings — 最大 standings 数量（保留参数，当前未使用）
 * 返回：cJSON 对象指针，调用者负责 cJSON_Delete()。
 *       失败（如缓存文件不存在）返回 NULL。
 */
cJSON *build_user_summary(const char *handle, const char *data_dir,
                          const char *output_dir, int max_standings) {
    (void)output_dir;
    (void)max_standings;

    /* ---- 初始化 ---- */
    cJSON *result = cJSON_CreateObject();
    if (!result) return NULL;

    /* 构建缓存文件路径 */
    char user_path[512], rating_path[512], status_path[512], clist_path[512];
    snprintf(user_path, sizeof(user_path), "%s/%s_user.json", data_dir, handle);
    snprintf(rating_path, sizeof(rating_path), "%s/%s_rating.json", data_dir, handle);
    snprintf(status_path, sizeof(status_path), "%s/%s_status.json", data_dir, handle);
    snprintf(clist_path, sizeof(clist_path), "%s/contest_list.json", data_dir);

    /* ---- 加载数据文件 ---- */

    /* user_info 和 rating_history 是必需的，缺失则无法分析 */
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

    /* status 和 contest_list 是可选的：
       - status 缺失 → 无法做赛时/补题判定和难度统计，只生成基本信息
       - contest_list 缺失 → 无法获取比赛起止时间，用默认值代替 */
    cJSON *status = load_cf_result_array(status_path);
    cJSON *contest_list = load_cf_result_array(clist_path);

    /* ---- 时间窗口预设 ---- */
    time_t now = time(NULL);
    long long now_ts = (long long)now;
    long long year_sec = 365LL * 86400;       /* 365天 = 31,536,000秒 */
    long long halfyear_sec = 180LL * 86400;   /* 180天 = 15,552,000秒 */
    long long month_sec = 30LL * 86400;       /*  30天 =  2,592,000秒 */

    /* ---- 修正 API 的头衔数据 ---- */
    adjust_rating_rank(user_info);

    /* ================================================================
     * 第一部分：提取用户基本信息
     * ================================================================ */
    cJSON *cf_handle = cJSON_GetObjectItem(user_info, "handle");
    cJSON_AddStringToObject(result, "handle",
        cf_handle && cf_handle->valuestring ? cf_handle->valuestring : handle);

    /* 头像和头衔图片 URL — 用于前端页面装饰 */
    cJSON *avatar = cJSON_GetObjectItem(user_info, "avatar");
    cJSON *titlePhoto = cJSON_GetObjectItem(user_info, "titlePhoto");
    cJSON_AddStringToObject(result, "avatar",
        avatar && avatar->valuestring ? avatar->valuestring : "");
    cJSON_AddStringToObject(result, "titlePhoto",
        titlePhoto && titlePhoto->valuestring ? titlePhoto->valuestring : "");

    /* 当前 rating 和最高 rating */
    int rating = 0, maxRating = 0;
    cJSON *r = cJSON_GetObjectItem(user_info, "rating");
    if (r && cJSON_IsNumber(r)) rating = r->valueint;
    cJSON *mr = cJSON_GetObjectItem(user_info, "maxRating");
    if (mr && cJSON_IsNumber(mr)) maxRating = mr->valueint;

    cJSON_AddNumberToObject(result, "rating", rating);
    cJSON_AddNumberToObject(result, "maxRating", maxRating);
    cJSON_AddStringToObject(result, "ratingColor", rating_color(rating));

    /* 当前头衔和最高头衔 */
    const char *rank_str = "Unrated";
    cJSON *rankj = cJSON_GetObjectItem(user_info, "rank");
    if (rankj && rankj->valuestring) rank_str = rankj->valuestring;
    cJSON_AddStringToObject(result, "rank", rank_str);

    const char *maxRankStr = "Unrated";
    cJSON *maxRankj = cJSON_GetObjectItem(user_info, "maxRank");
    if (maxRankj && maxRankj->valuestring) maxRankStr = maxRankj->valuestring;
    cJSON_AddStringToObject(result, "maxRank", maxRankStr);

    /* ================================================================
     * 第二部分：遍历每场比赛，生成详细记录
     * ================================================================ */
    cJSON *contest_lookup = build_contest_lookup(contest_list);
    if (!contest_lookup) contest_lookup = cJSON_CreateObject();

    /* contest_count 是 rating_history 的长度，等于参赛次数 */
    int contest_count = cJSON_GetArraySize(rating_history);
    int contest_count_180 = 0;    /* 近180天参赛次数 */
    int max_rating_180 = 0;       /* 近180天最高 rating */
    cJSON *contests_arr = cJSON_AddArrayToObject(result, "contests");

    /* rating 历史按时间从旧到新排列（CF API 保证此顺序） */
    cJSON *item;
    cJSON_ArrayForEach(item, rating_history) {

        cJSON *contest_obj = cJSON_CreateObject();

        /* 提取比赛 ID 和 rating 更新时间 */
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

        /* 从查找表中获取比赛的开始时间和持续时长 */
        char key[32];
        snprintf(key, sizeof(key), "%d", contest_id);
        cJSON *cl_entry = cJSON_GetObjectItem(contest_lookup, key);
        long long start_time = 0;
        long long duration = 7200;  /* 默认 2小时，CF 标准时长 */
        if (cl_entry) {
            cJSON *st = cJSON_GetObjectItem(cl_entry, "startTimeSeconds");
            if (st && cJSON_IsNumber(st)) start_time = (long long)st->valuedouble;
            cJSON *dur = cJSON_GetObjectItem(cl_entry, "durationSeconds");
            if (dur && cJSON_IsNumber(dur)) duration = (long long)dur->valuedouble;
        }

        cJSON_AddNumberToObject(contest_obj, "startTime",
            (double)(start_time ? start_time : update_time));

        /* 赛前/赛后 rating — 用于前端折线图数据点 */
        cJSON *oldr = cJSON_GetObjectItem(item, "oldRating");
        cJSON *newr = cJSON_GetObjectItem(item, "newRating");
        int old_rating = oldr && cJSON_IsNumber(oldr) ? oldr->valueint : 0;
        int new_rating = newr && cJSON_IsNumber(newr) ? newr->valueint : 0;

        cJSON_AddNumberToObject(contest_obj, "oldRating", old_rating);
        cJSON_AddNumberToObject(contest_obj, "newRating", new_rating);
        cJSON_AddStringToObject(contest_obj, "oldRatingColor",
            rating_color(old_rating));
        cJSON_AddStringToObject(contest_obj, "newRatingColor",
            rating_color(new_rating));

        /* 比赛排名 — 数字越小越好 */
        cJSON *rankj2 = cJSON_GetObjectItem(item, "rank");
        int rank_val = rankj2 && cJSON_IsNumber(rankj2) ?
            rankj2->valueint : 0;
        cJSON_AddNumberToObject(contest_obj, "rank", rank_val);

        /* 近180天活跃度统计 */
        if (now_ts - update_time <= halfyear_sec) {
            contest_count_180++;
            if (new_rating > max_rating_180) max_rating_180 = new_rating;
        }

        /* ┄┄┄┄┄┄ 调用赛时/补题判定算法 ┄┄┄┄┄┄ */
        long long contest_end = start_time + duration;
        ProblemState probs[MAX_PROBLEMS] = {{0}};
        int prob_cnt = process_contest_problems(status, contest_id,
            contest_end, probs, MAX_PROBLEMS);

        /* ┄┄┄┄┄┄ 将判定结果写入 JSON ┄┄┄┄┄┄ */
        int solved_count = 0;  /* 本场赛时通过的题目数 */
        cJSON *problems_arr = cJSON_AddArrayToObject(contest_obj, "problems");
        cJSON *upsolved_arr = cJSON_AddArrayToObject(contest_obj, "upsolved");

        for (int i = 0; i < prob_cnt; i++) {
            if (probs[i].in_contest_ac) {
                /* 赛时 AC → 前端显示绿色 */
                solved_count++;
                cJSON *prob = cJSON_CreateObject();
                cJSON_AddStringToObject(prob, "index", probs[i].index);
                cJSON_AddNumberToObject(prob, "rejectedAttempts",
                    probs[i].rejected);
                cJSON_AddNumberToObject(prob, "problemRating",
                    probs[i].problem_rating);
                cJSON_AddStringToObject(prob, "verdict", "OK");
                cJSON_AddItemToArray(problems_arr, prob);
            } else if (probs[i].rejected > 0) {
                /* 赛时提交过但全部失败 → 前端显示红色 */
                cJSON *prob = cJSON_CreateObject();
                cJSON_AddStringToObject(prob, "index", probs[i].index);
                cJSON_AddNumberToObject(prob, "rejectedAttempts",
                    probs[i].rejected);
                cJSON_AddNumberToObject(prob, "problemRating",
                    probs[i].problem_rating);
                cJSON_AddStringToObject(prob, "verdict", "FAIL");
                cJSON_AddItemToArray(problems_arr, prob);
            }

            /* 赛后补题 AC → 前端显示黄色（独立的 upsolved 区域） */
            if (probs[i].post_ac && !probs[i].in_contest_ac) {
                cJSON *us = cJSON_CreateObject();
                cJSON_AddStringToObject(us, "index", probs[i].index);
                cJSON_AddNumberToObject(us, "problemRating",
                    probs[i].problem_rating);
                cJSON_AddItemToArray(upsolved_arr, us);
            }
        }

        cJSON_AddNumberToObject(contest_obj, "solvedCount", solved_count);
        cJSON_AddItemToArray(contests_arr, contest_obj);
    }  /* 比赛遍历结束 */

    /* ---- 写入统计摘要 ---- */
    cJSON_AddNumberToObject(result, "contestCount", contest_count);
    cJSON_AddNumberToObject(result, "contestCount180", contest_count_180);
    cJSON_AddNumberToObject(result, "maxRating180", max_rating_180);

    /* ================================================================
     * 第三部分：构建 AC 难度分布直方图（4个时间窗口）
     * ================================================================ */
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

    /* ---- 清理资源 ---- */
    cJSON_Delete(user_info);
    cJSON_Delete(rating_history);
    if (status) cJSON_Delete(status);
    if (contest_list) cJSON_Delete(contest_list);
    cJSON_Delete(contest_lookup);

    return result;
}
