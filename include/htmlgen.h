/*
 * htmlgen.h — HTML 报告生成模块接口
 *
 * 将模板中的占位符替换为实际数据，
 * 生成自包含的 HTML 可视化报告。
 */

#pragma once
#include "cJSON.h"

/*
 * 生成单个 HTML 文件
 * 参数：
 *   template_path — HTML 模板路径
 *   output_path   — 输出文件路径
 *   title         — 页面标题
 *   data          — cJSON 数据（将被序列化为 JSON 嵌入页面）
 */
int generate_html(const char *template_path, const char *output_path,
                  const char *title, cJSON *data);

/*
 * 生成所有 HTML 报告
 * 包含 index.html（用户列表）和 report_<handle>.html（详情页）
 */
int generate_all_html(cJSON *users, const char *template_path,
                      const char *output_dir);
