# 压测与性能优化（v10 系列）

v10.x **每个版本一张表**，仅三行：**背景 / 变更 / 结果**。**结果**只写描述性摘要并链到 `benchmark_log/BENCH-NNN`，**不写 wrk/perf 等测试指令**（命令与原始输出仅在 bench log）。压测 log 对齐 **v 版本**（如 `v10.0`），不写 stage。

---

## v10.0

| 项 | 内容 |
|----|------|
| 背景 | BENCH-001（v9）Keep-Alive 正常；`Connection: close` 重压测 SSH 假死。根因：`handleDead()` / `stop()` 每条断开同步 `std::cout` 到终端，短连接高 QPS 下 pty/SSH I/O 拖垮 2 核 2 GiB 小机 |
| 变更 | `src/server/Connection.cpp` 删除 `handleDead()`、`stop()` 内 `connection break` 的 `std::cout`；保留 `main.cpp` 启停各一行；不做 AsyncLogger |
| 结果 | Keep-Alive 主基线约 **32.8k RPS**（较 v9 ~29.2k 提升约 13%，含环境波动可能）；短连接轻量压测可完整跑完，SSH 稳定，v10.0 达成。详见 [`BENCH-002`](../benchmark_log/BENCH-002_20260523_wrk_v10.0.md) |

---

## v10.1

| 项 | 内容 |
|----|------|
| 背景 | （待填） |
| 变更 | （待填） |
| 结果 | （待填：描述性结论 + [`BENCH-NNN`](../benchmark_log/) 链接；不写测试指令） |

---

## v10.2

| 项 | 内容 |
|----|------|
| 背景 | （待填） |
| 变更 | （待填） |
| 结果 | （待填：描述性结论 + [`BENCH-NNN`](../benchmark_log/) 链接；不写测试指令） |
