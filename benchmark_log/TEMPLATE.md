# {版本}：简述（wrk 基线 / perf 热点 / 优化后复测）

> 复制本文件并重命名：`{版本}_{YYYYMMDD}_{简述}.md`  
> 例：`v10.0_20260523_bench.md`、`v9_20260523_wrk_baseline.md`  
> **一设计版本一份报告**：wrk 与 perf 写在同一文件（§4 + §5）；版本号与 `docs/DESIGN_v*.md` 一致即代码一致。

---

## 1. 元信息

| 项 | 值 |
|----|-----|
| 设计版本 | `vN` 或 `v10.x`（对齐 `docs/DESIGN_v*.md` 与当时 server 代码） |
| 日期 | YYYY-MM-DD |
| 测试人 | luxiaohang |
| 测试类型 | wrk 基线 / wrk+perf / 优化后复测 |
| 关联说明 | （可选：FIX 编号、对照哪一版；正文自包含要点） |

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

# perf / 火焰图（可与 wrk 分次跑，报告仍写同一文件）
bash scripts/perf_bench.sh -v v10.0   # 脚本内 RelWithDebInfo 重编
```

---

## 4. wrk 测试

> **2 核 2 GiB 环境**：`-c` 不超过 20；勿用 `-c50` / `-c100`（见 `README.md`）。

### 4.1 参数说明

| 场景 | `-t` | `-c` | `-d` | 其他 |
|------|------|------|------|------|
| Keep-Alive 热身 | 1 | 10 | 5s | 首次 / 换版本后 |
| Keep-Alive 基线 | 1～2 | 10～20 | 10～30s | 默认 |
| Keep-Alive + perf | 2 | 20 | 30s | 与 perf 同跑，RPS 不与 §4 纯 wrk 比 |
| 短连接 | 1 | 5～10 | 5～10s | `-H "Connection: close"` |

### 4.2 执行命令

```bash
wrk -t2 -c20 -d30s http://127.0.0.1:8888/
```

### 4.3 原始输出

```text
（粘贴 wrk 完整终端输出）
```

### 4.4 结果汇总

| 场景 | -t | -c | -d | RPS | Latency avg | Latency max | Errors | 备注 |
|------|----|----|-----|-----|-------------|-------------|--------|------|
| KA | 2 | 20 | 30s | | | | | |

---

## 5. perf / flamegraph

> 操作与读法见 [`README.md`](./README.md)「自动 perf」「读 perf 产物（符号表 + 火焰图）」。本节填本次结果；未做 perf 可写「本版本未做 perf」或省略。

### 5.1 采样命令

```bash
bash scripts/perf_bench.sh -v v10.0
```

### 5.2 采样条件

| 项 | 值 |
|----|-----|
| 构建类型 | RelWithDebInfo |
| 并发 wrk | `wrk -t2 -c20 -d30s http://127.0.0.1:8888/` |
| 采样时长 | 30s |
| 符号表 | `benchmark_log/artifacts/{版本}_perf_report.txt` |
| 火焰图 | `benchmark_log/artifacts/{版本}_flamegraph.svg` |
| perf.data | `benchmark_log/artifacts/{版本}_perf.data` |

### 5.3 wrk（与 perf 同跑，可选）

| 场景 | -t | -c | -d | RPS | Latency avg | Errors | 备注 |
|------|----|----|-----|-----|-------------|--------|------|
| KA + perf | 2 | 20 | 30s | | | | 不与 §4 纯 wrk 严格对比 |

### 5.4 热点摘要

> 先读符号表 Overhead 前几名，再在火焰图 Search 对应符号确认调用链。

| 占比（约） | 符号 / 函数 | 说明 |
|------------|-------------|------|
| | | |

### 5.5 符号表摘录（可选）

```text
（粘贴 perf_report.txt 中 Overhead 最高的若干行）
```

---

## 6. 优化对比（可选）

| 场景 | 对照 RPS | 本次 RPS | 说明 |
|------|----------|----------|------|
| KA -t2 -c20 -d30s | | | |

---

## 7. 结论与下一步

- **结论**：
- **异常**：
- **下一步**：

---

## 8. 附录（可选）

```bash
top -H -p $(pgrep -x server)
mpstat 1 5
ss -s
```
