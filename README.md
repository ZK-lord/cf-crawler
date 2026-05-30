# CF Crawler

基于 C 语言的 Codeforces 数据分析与可视化系统。

自动抓取 Codeforces 用户数据，分析比赛表现和 AC 难度分布，生成含 ECharts 交互图表的 HTML 报告。

## 快速开始

```bash
# 1. 编辑用户列表
echo "tourist" > sample_users.txt

# 2. 编译运行
make && bin/cf_crawler.exe sample_users.txt

# 3. 浏览器打开 output/index.html 查看报告
```

或者一键启动 Web 服务器：

```bash
启动服务器.bat
# 浏览器自动打开 → 输入 CF 用户名 → 点添加 → 自动抓取并生成报告
```

## 功能一览

- 用户基本信息、Rating 变化折线图
- AC 难度直方图（4 个时间窗口：全部 / 近一年 / 近 180 天 / 近一月）
- 赛时通过 / 失败 / 赛后补题 分类标记
- 支持多用户批量处理
- HTML 自包含报告，浏览器直接打开

## 项目结构

```text
├── src/           C 源代码 (main, cf_api, analyzer, htmlgen, json_utils, rate_limiter)
├── include/       头文件
├── bin/           编译产物 (exe + DLL)
├── web/           HTML 模板
├── output/        生成的报告
├── data/          API 数据缓存
└── Makefile       构建脚本
```

## 环境要求

- MinGW-w64 GCC（编译时需要）
- libcurl（运行时需要，DLL 已在 bin/ 中）
- Python 3.x（服务器模式需要）

## 技术栈

C11 · libcurl · cJSON · ECharts 5.5.0 · Python

## License

MIT
