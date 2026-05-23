# 压测与性能优化（v10 系列）

v10.x **每个版本一张表**，仅三行：**背景 / 变更 / 结果**。**结果**只写描述性摘要并链到 `benchmark_log/` 测试记录（`{版本}_{YYYYMMDD}_{简述}.md`），**不写 wrk/perf 等测试指令**。压测 log 对齐 **设计版本**（如 `v10.0`），不写 stage。

**升版规则**：仅当 **`src/` 下 server 业务代码**有优化或行为变更时递增 v10.x。一设计版本一份测试报告（wrk + perf 同文件）。

---

## v10.0

| 项 | 内容 |
|----|------|
| 背景 | v9：Keep-Alive 正常；`Connection: close` 重压测 SSH 假死。根因：`handleDead()` / `stop()` 每条断开同步 `std::cout` 到终端 |
| 变更 | `src/server/Connection.cpp` 删除 `handleDead()`、`stop()` 内 `connection break` 的 `std::cout`；保留 `main.cpp` 启停各一行 |
| 结果 | KA ~32.8k RPS；短连接轻量可跑完；perf 热点 **onHttp → ThreadPool::enqueue** 与 **sendHttpOnLoop → write**。详见 [`v10.0_20260523_bench`](../benchmark_log/v10.0_20260523_bench.md) |

---

## v10.1

| 项 | 内容 |
|----|------|
| 背景 | （待填：如 v10.0 结论——线程双跳与写路径为首要优化方向） |
| 变更 | （待填：`src/` 代码优化） |
| 结果 | （待填：描述性结论 + 链到 `benchmark_log/` 对应记录；不写测试指令） |
