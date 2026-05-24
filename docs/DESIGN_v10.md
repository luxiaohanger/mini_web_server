# 压测与性能优化（v10 系列）
----

## v10.0

| 项 | 内容 |
|----|------|
| 背景 | v9：Keep-Alive 正常；`Connection: close` 重压测 SSH 假死。根因：`handleDead()` / `stop()` 每条断开同步 `std::cout` 到终端 |
| 变更 | `src/server/Connection.cpp` 删除 `handleDead()`、`stop()` 内 `connection break` 的 `std::cout`；保留 `main.cpp` 启停各一行 |
| 结果 | KA **~32.8k RPS**（§4 手动 wrk）；短连接轻量可跑完。perf（`perf_bench.sh` 符号表 §0～§4）：**kernel 60% / libc 26% / server 10% Self**；§1 All 最热 **EventLoop::loop(62%)**、**onHttp→enqueue 双跳(~32%)** 与写路径；§3 内核 **network 13% + syscall 8%**，锁/原子(other) **36%**。详见 [`v10.0_20260523_bench`](../benchmark_log/v10.0_20260523_bench.md) |

---

## v10.1

| 项 | 内容 |
|----|------|
| 背景 | v10.0 perf：CPU 主要在内核与 libc；业务侧为 **跨线程 enqueue** 与 **write/readv**；Echo 下 `HttpProcess::parse` All 仅 ~4.5% |
| 变更 | （待填：`src/` 代码优化） |
| 结果 | （待填：描述性结论 + 链到 `benchmark_log/` 对应记录；不写测试指令） |
