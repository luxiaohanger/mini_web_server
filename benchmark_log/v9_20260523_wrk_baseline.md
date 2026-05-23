# v9：wrk 轻量基线（Keep-Alive 完成，短连接未完成）

---

## 1. 元信息

| 项 | 值 |
|----|-----|
| 设计版本 | v9 |
| 日期 | 2026-05-23 |
| 测试人 | luxiaohang |
| 测试类型 | wrk 基线 |
| 关联说明 | 含 FIX-034（`Buffer::bufToBuf`）；短连接 30s 用例运行中 SSH/会话断联，无完整输出 |

---

## 2. 硬件与系统环境

| 项 | 值 |
|----|-----|
| CPU | 2 逻辑核 |
| 内存 | 2 GiB |
| OS | Ubuntu 24.04 |
| 压测方式 | 本机 loopback `127.0.0.1:8888` |
| 主机名 | `iZuf6etxjlhvlqox5s38pcZ`（阿里云 ECS） |
| 备注 | Keep-Alive 四组均正常结束；`-H "Connection: close" -t2 -c20 -d30s` 运行中断联 |

---

## 3. 服务配置

| 项 | 值 |
|----|-----|
| 监听地址 | `127.0.0.1:8888` |
| SubReactor 数 | 2 |
| ThreadPool 大小 | 2 |
| 工作目录 | `~/test_server` |

**启动命令：**

```bash
./build/src/server/server
```

---

## 4. wrk 测试

### 4.4 结果汇总

| 场景 | -t | -c | -d | RPS | Latency avg | Latency max | Errors | 备注 |
|------|----|----|-----|-----|-------------|-------------|--------|------|
| KA | 1 | 10 | 5s | 27143.55 | 438.87us | 6.09ms | 0 | 热身 |
| KA | 1 | 10 | 10s | 28218.70 | 416.02us | 5.66ms | 0 | |
| KA | 1 | 20 | 10s | 28947.86 | 673.88us | 5.92ms | 0 | |
| KA | 2 | 20 | 30s | 29154.15 | 682.21us | 7.41ms | 0 | 正式基线 |
| close | 2 | 20 | 30s | — | — | — | — | **未完成，运行中断联** |

### 4.3 原始输出

**Keep-Alive, -t1 -c10 -d5s：**

```text
Running 5s test @ http://127.0.0.1:8888/
  1 threads and 10 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   438.87us  454.78us   6.09ms   88.24%
    Req/Sec    27.32k     3.30k   31.28k    86.00%
  135734 requests in 5.00s, 9.84MB read
Requests/sec:  27143.55
Transfer/sec:      1.97MB
```

**Keep-Alive, -t1 -c10 -d10s：**

```text
Running 10s test @ http://127.0.0.1:8888/
  1 threads and 10 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   416.02us  417.77us   5.66ms   88.54%
    Req/Sec    28.36k     2.53k   32.09k    85.00%
  282211 requests in 10.00s, 20.45MB read
Requests/sec:  28218.70
Transfer/sec:      2.05MB
```

**Keep-Alive, -t1 -c20 -d10s：**

```text
Running 10s test @ http://127.0.0.1:8888/
  1 threads and 20 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   673.88us  421.68us   5.92ms   81.01%
    Req/Sec    29.10k     2.44k   33.35k    79.00%
  289558 requests in 10.00s, 20.99MB read
Requests/sec:  28947.86
Transfer/sec:      2.10MB
```

**Keep-Alive, -t2 -c20 -d30s（主基线）：**

```text
Running 30s test @ http://127.0.0.1:8888/
  2 threads and 20 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   682.21us  375.46us   7.41ms   80.40%
    Req/Sec    14.68k     1.17k   27.10k    86.02%
  877575 requests in 30.10s, 63.61MB read
Requests/sec:  29154.15
Transfer/sec:      2.11MB
```

**Connection: close, -t2 -c20 -d30s：**

```text
（未完成 — 运行过程中服务器/SSH 断联，无 wrk 汇总输出）
```

---

## 7. 结论与下一步

- **结论（Keep-Alive）**：`-t2 -c20 -d30s` 稳定约 **29.1k RPS**，avg 延迟 ~682us，无 Errors；`-c` 从 10→20、`-d` 从 5s→30s 对 RPS 影响不大，2 核小机已接近该路径 CPU 饱和。
- **短连接未完成**：`Connection: close` 每请求触发 accept → 建连 → 响应 → `handleDead`（含 `std::cout` 打日志）， churn 远高于 Keep-Alive；在 2 GiB + 2 核上 **30s 短连接压测过重**，易导致 SSH 卡死，不一定是 HTTP 逻辑 bug。
- **断联后排查（2026-05-23）**：压测中 SSH 无法连上，**用户手动重启**后恢复（非 dmesg 可见的自动 OOM 重启）。重启后 `sudo dmesg -T | tail -20` 仅为新开机的初始化日志，**压测当时是否 OOM / 杀进程已无法从 dmesg 追溯**。
- **推断**：更可能是短连接高 churn + `handleDead` 终端日志导致 **CPU/终端 I/O 饱和、SSH 假死**；Keep-Alive 基线正常，暂不按 server 崩溃处理。
- **下一步**：
  1. 短连接改用更轻参数 + **server 日志重定向**（见 `benchmark_log/README.md`）；
  2. 用 **tmux** 跑 server，即使 SSH 抖一下也可 `tmux attach`；
  3. 补跑 close 轻量用例后更新本表；v10.0 已含 wrk+perf 见 [`v10.0_20260523_bench.md`](./v10.0_20260523_bench.md)。
