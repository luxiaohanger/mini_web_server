# 压测与性能优化（v10 系列）
----

## v10.0

| 项 | 内容 |
|----|------|
| 背景 | v9：Keep-Alive 正常；`Connection: close` 重压测 SSH 假死。根因：`handleDead()` / `stop()` 每条断开同步 `std::cout` 到终端 |
| 变更 | `src/server/Connection.cpp` 删除 `handleDead()`、`stop()` 内 `connection break` 的 `std::cout`；保留 `main.cpp` 启停各一行 |
| 结果 | KA **~32.8k RPS**（第 4 节 wrk）；短连接轻量可跑完。perf 符号表：**§0** kernel 60% / libc 26% / server 10% Self / libstdc++ 3%；**§1** All 最热 EventLoop::loop(62%)、onHttp→sendHttpOnLoop(~33%)、ThreadPool worker(22%)、enqueue 双跳(~25%)、TimerQueue::refreshClock(7%)；**§2** other 36% + network 13% + syscall 8%；**§3** pthread 6% + io 5% + timer 3% + epoll 2%。详见 [`v10.0_20260523_bench`](../benchmark_log/v10.0_20260523_bench.md) |

---

## v10.1

| 项 | 内容 |
|----|------|
| 背景 | v10.0 perf：CPU 主要在内核与 libc（§0 合计 ~86%） ；**§1** 跨线程 enqueue/future(~25%)、TimerQueue::refreshClock(7%)、写路径 sendHttpOnLoop(~33%)；Echo 下 parse All ~4.5%，ThreadPool 对 Echo 为纯开销 |
| 变更 | **ThreadPool 可选，默认关闭（Echo I/O 直出）**。<br>• `main.cpp`：`-t on\|off` 解析，默认 **off**；非法参数 exit 1<br>• `Server(bool useThreadPool)`：`off` 不创建 `ThreadPool`；`stop()` 仅 `on` 时 `threadpool->stop()`<br>• `SubReactor(bool)`：`on` 用带 `taskSubmit` 的 `addConnection`；`off` 用无池重载<br>• `Connection(..., bool)`：`off` 在 owner I/O 线程同步 `buildResponse` → `sendHttpOnLoop`（无 `working++`）；`on` 保持 v10.0 enqueue 双跳<br>• SubReactor 数量仍 = `hardware_concurrency()`，与 `-t` 无关<br>• **对照**：复现 v10.0 行为需 `./server -t on`；v10.1 验收默认不加参或 `-t off` |
| 结果 | KA **~69.6k RPS**（wrk，ThreadPool off，+112% vs v10.0）；延迟 **284us**。perf：**§0** kernel 66% / server 8% Self；**§1** onHttp→读写(~60%/50%)，enqueue/ThreadPool **消失**，refreshClock 11%；**§2** network 20%（↑）；**§3** io 9%（readv）/ pthread 1%（↓）。详见 [`v10.1_20260524_bench`](../benchmark_log/v10.1_20260524_bench.md) |
