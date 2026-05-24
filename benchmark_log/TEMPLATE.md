# {版本}：简述（wrk 基线 / perf 热点 / 优化后复测）

> 复制本文件并重命名：`{版本}_{YYYYMMDD}_{简述}.md`  
> 例：`v10.0_20260523_bench.md`、`v9_20260523_wrk_baseline.md`  
> **一设计版本一份报告**：wrk 与 perf 写在同一文件（第 4 节 + 第 5 节）；版本号与 `docs/DESIGN_v*.md` 一致即代码一致。  
> **序号约定**：本 md 用 **第 N 节**（`## N.`）；`*_perf_report.txt` 用 **§0～§3**（与「第 5 节 perf」无关）。

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

| 场景 | -t | -c | -d | RPS | Latency avg | Latency max | Errors | 备注 |
|------|----|----|-----|-----|-------------|-------------|--------|------|
| KA | 2 | 20 | 30s | | | | | |

---

## 5. perf / flamegraph

> 操作与读法见 [`README.md`](./README.md)「自动 perf」「读 perf 产物」。`*_perf_report.txt` 结构：**§0 预算 → §1 server → §2 kernel（分类+符号）→ §3 libc（分类+符号）**。

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
| 符号表 | `benchmark_log/artifacts/{版本}_perf_report.txt`（§0～§3） |
| 火焰图 | `benchmark_log/artifacts/{版本}_flamegraph.svg` |
| perf.data | `benchmark_log/artifacts/{版本}_perf.data` |

### 5.3 热点摘要

> 下表 § 号对应 **符号表** 段号；摘要填 §0、§1、§2 分类、§3 分类；§2/§3 符号子段见 `_perf_report.txt` 全文。

**§0 预算（Self）**

| Self | 层级 | 说明 |
|------|------|------|
| | kernel / libc / server / libpthread / … | |

**§1 server（All / Self）**

| All | Self | 符号 | 说明 |
|-----|------|------|------|
| | | | |

**§2 kernel · 分类（Self）**

| Self | 类别 | 代表符号 |
|------|------|----------|
| | syscall / network / futex / sched / other | |

**§3 libc · 分类（Self）**

| Self | 类别 | 代表符号 | 说明 |
|------|------|----------|------|
| | io | read / write / readv | 读写 syscall 封装 |
| | epoll | epoll_ctl / epoll_wait | Channel 注册与等待 |
| | timer | timerfd_settime | 连接 idle 定时器 |
| | alloc | malloc / free | 堆分配 |
| | pthread | __pthread_once 等 | libc 内 pthread 辅助（§0 的 libpthread 另计） |
| | string | memcpy / strlen 等 | 字符串/内存拷贝 |
| | other | | 其余 libc |

> §2 分类各行相加 ≈ §0 **kernel**；§3 分类各行相加 ≈ §0 **libc**。

---

## 6. 分析结果

- **wrk**：（第 4 节验收 RPS、与上一版对比、Errors）
- **符号 §0**：（哪一层占主导；kernel / libc / server 比例是否异常）
- **符号 §1**：（All 热点路径；Self 低是否表示时间在 callee / 内核）
- **符号 §2**：（network / syscall / other 构成；loopback 栈、锁/原子是否为主）
- **符号 §3**：（io / epoll / timer 构成；与读写路径、定时器、epoll 是否匹配 §1）
- **综合结论**：（瓶颈在用户态 src、跨线程、libc I/O 还是内核）
- **异常**：
- **下一步**：（下一版 src 优化方向）
