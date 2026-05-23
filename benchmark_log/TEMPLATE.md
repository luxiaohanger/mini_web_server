# BENCH-NNN：简述（如 wrk 基线 / perf 热点 / 优化后复测）

> 复制本文件并重命名：`BENCH-NNN_YYYYMMDD_简述.md`

---

## 1. 元信息

| 项 | 值 |
|----|-----|
| 编号 | BENCH-NNN |
| 日期 | YYYY-MM-DD |
| 测试人 | luxiaohang |
| 设计版本 | `vN` 或 `v10.x`（见 `benchmark_log/README.md`「设计版本」表，对应 `docs/DESIGN_v*.md`） |
| 测试类型 | wrk 基线 / wrk 对比 / perf 采样 / 优化后复测 |
| 关联说明 | （可选：同版本内 FIX 编号、v10.x 优化摘要；正文自包含要点） |

---

## 2. 硬件与系统环境

> 默认环境见 `benchmark_log/README.md`「标准测试环境」；若换机器请改下表。

| 项 | 值 |
|----|-----|
| CPU | 2 逻辑核 |
| 内存 | 2 GiB |
| OS | Ubuntu 24.04（`uname -r` 内核版本：） |
| 压测方式 | 本机 loopback `127.0.0.1:8888` |
| 备注 | 压测前 `free -h` / 是否独占机器 / 是否有其他负载 |

---

## 3. 服务配置

| 项 | 值 |
|----|-----|
| 监听地址 | `127.0.0.1:8888`（默认见 `src/server/main.cpp`） |
| SubReactor 数 | 2（`hardware_concurrency()`） |
| ThreadPool 大小 | 2 |
| 其他 | idle 超时、Keep-Alive 默认行为等 |

**启动命令：**

```bash
./build/src/server/server
```

**编译命令（若与默认不同请填写）：**

```bash
# wrk 基线
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j2

# perf / 火焰图（须带符号，VS Code CMake 选 RelWithDebInfo 等价）
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build -j2
```

---

## 4. wrk 测试

> **2 核 2 GiB 环境**：`-c` 不超过 20；勿用 `-c50` / `-c100`（见 `README.md`）。perf 采样时的 wrk 与 BENCH-002 主基线对齐。

### 4.1 参数说明

| 场景 | `-t` | `-c` | `-d` | 其他 |
|------|------|------|------|------|
| Keep-Alive 热身 | 1 | 10 | 5s | 首次 / 换版本后 |
| Keep-Alive 基线 | 1～2 | 10～20 | 10～30s | 默认 |
| Keep-Alive + perf | 2 | 20 | 30s | 与 perf 同跑，RPS 不与纯 wrk 基线比 |
| 短连接 | 1 | 5～10 | 5～10s | `-H "Connection: close"` |

### 4.2 执行命令

```bash
# 示例：Keep-Alive 主基线（BENCH-002 对照）
wrk -t2 -c20 -d30s http://127.0.0.1:8888/

# 示例：短连接（轻量）
wrk -t1 -c10 -d10s -H "Connection: close" http://127.0.0.1:8888/
```

### 4.3 原始输出

<!-- 每次 wrk 完整粘贴一段，保留 Thread Stats / Latency Distribution / Errors -->

**场景 A（Keep-Alive, -t2 -c20 -d30s）：**

```text
（粘贴 wrk 完整终端输出）
```

**场景 B（Connection: close, -t1 -c10 -d10s，可选）：**

```text
（粘贴 wrk 完整终端输出）
```

### 4.4 结果汇总

| 场景 | -t | -c | -d | RPS | Latency avg | Latency max | Errors | 备注 |
|------|----|----|-----|-----|-------------|-------------|--------|------|
| KA | 2 | 20 | 30s | | | | | |
| close | 1 | 10 | 10s | | | | | 可选 |

> 同一配置建议跑 2～3 次，表中可填稳定区间或取中位数。

---

## 5. perf / flamegraph

> **逐步操作** 见 [`README.md`](./README.md)「perf 采样」。本节只填本次结果摘要。

### 5.1 采样命令（实际使用的完整命令）

```bash
# 窗格 A：server 已启动
# 窗格 B：wrk -t2 -c20 -d30s http://127.0.0.1:8888/
# 窗格 C：与 wrk 同时
sudo perf record -F 997 -g -p $(pgrep -x server) -- sleep 30
```

### 5.2 采样条件

| 项 | 值 |
|----|-----|
| 构建类型 | RelWithDebInfo |
| 并发 wrk 命令 | `wrk -t2 -c20 -d30s http://127.0.0.1:8888/` |
| 采样时长 | 30s |
| perf 参数 | `-F 997 -g -p $(pgrep -x server)` |
| 产物路径 | `benchmark_log/artifacts/BENCH-NNN_flamegraph.svg` |
| perf.data | `benchmark_log/artifacts/BENCH-NNN_perf.data`（可选保留） |

### 5.3 热点摘要（自包含，勿依赖外部笔记）

从 `sudo perf report --stdio -g --no-children` 与火焰图归纳，填 3～8 行：

| 占比（约） | 符号 / 函数 | 说明 |
|------------|-------------|------|
| | | 例：HttpProcess 解析路径 |
| | | 例：Buffer 拷贝 |
| | | |

### 5.4 perf report 摘录（可选）

```text
（粘贴 perf report 前 30～50 行，或 Overhead 最高的符号列表）
```

---

## 6. 优化对比（可选）

若本次为优化后复测，与基线条目对照：

| 场景 | 优化前 RPS | 优化后 RPS | 变化 | 说明 |
|------|------------|------------|------|------|
| KA -t2 -c20 -d30s | | | | |

**改动摘要：**

- （如：Buffer 写路径、锁粒度、任务队列等，一两句话）

---

## 7. 结论与下一步

- **结论**：（如：CPU 已饱和 / accept 瓶颈 / ThreadPool 排队导致 p99 抬升）
- **异常**：（Errors、连接被拒、与功能测试冲突等）
- **下一步**：（如：perf 确认热点 → 针对 Buffer 优化 → BENCH-NNN+1 复测）

---

## 8. 附录（可选）

```bash
# 压测时观察负载
top -H -p $(pgrep -x server)
mpstat 1 5
ss -s
```
