# 性能测试记录

本目录存放 **wrk / perf** 等基准测试的可复现记录，与 `issue_log/`（缺陷跟踪）分工：

| 目录 | 用途 |
|------|------|
| `benchmark_log/` | 性能基线、优化前后对比、压测环境与原始输出 |
| `issue_log/` | Bug、资源泄漏、并发与语义错误 |

## 标准测试环境

本项目性能基线默认在以下机器上采集（各次记录须填写**设计版本**与日期，便于跨版本对比）：

| 项 | 值 |
|----|-----|
| OS | Ubuntu 24.04 |
| CPU | 2 逻辑核 |
| 内存 | 2 GiB |
| 压测方式 | 本机 loopback `127.0.0.1:8888` |

**环境注意（2 核 + 2 GiB，宜轻压）：**

- server 约 5～6 个活跃线程与 wrk **共用 2 核**；RPS 为整机能力，勿与多核大内存机器绝对值对比。
- **内存紧张**：高 `-c` 易触发 swap / OOM，表现为 SSH 卡死、会话断联；基线阶段**不要用 `-c50` / `-c100`**。
- 推荐 **tmux**（`tmux new -s bench`），SSH 断开仍可 `tmux attach` 恢复。
- 压测前、压测中执行 `free -h`；**available 内存持续 < 200 MiB** 或 swap 活跃时立即停止。
- **perf 与纯 wrk 分开记录**：perf 采样与 wrk 同跑时负载更重，RPS 不与纯 wrk 基线直接对比。

## 设计版本（压测对照轴）

性能记录只对齐 **`docs/DESIGN_v*.md` 的版本号**，不用 Git commit，**不写 stage**：

| 版本 | 文档 | 要点 |
|------|------|------|
| v7 | `DESIGN_v7` | HTTP/1.1 最小子集 |
| v8 | `DESIGN_v8` | TimerQueue + idle timeout |
| v9 | `DESIGN_v9` | 程序内停服与退出闭环 |
| v10.0 | `DESIGN_v10` | 压测优化：移除 `Connection` 热路径 `cout` |
| v10.1+ | `DESIGN_v10` | perf 热点、针对性优化 |

- 每条 BENCH 元信息填 **`vN`** 或 **`v10.x`**（例如 `v9`、`v10.0`）。
- 同版本内小修复写在「关联说明」，**不单独升版本**；优化变更按 v10.0 / v10.1 递进并新跑基线。
- 优化前后对比：**不同 v 版本或同文档内明确标注的 v10.x**，勿混用未标注版本的数据。

## 使用方式

1. 复制 [`TEMPLATE.md`](./TEMPLATE.md) 为新文件。
2. 文件命名：`BENCH-NNN_YYYYMMDD_简述.md`（例如 `BENCH-001_20260523_wrk_baseline.md`）。
3. 按模板填写环境、命令、**完整原始输出**与汇总表（**测试人**统一填 `luxiaohang`）。
4. wrk 条目填第 4 节；perf 条目另填第 5 节热点表；优化后新建条目保留对照数据。
5. 更新本文「索引」表；perf 结论同步 `docs/DESIGN_v10.md` 对应 v10.x 小节（只写摘要，链 BENCH 条目）。

## 测试阶段顺序

```text
BENCH-001 (v9 wrk) → BENCH-002 (v10.0 wrk) → BENCH-003 (v10.1 perf) → 改代码 → BENCH-004 (v10.2 wrk 复测)
```

---

## wrk 压测

### 用途

| 目标 | 说明 |
|------|------|
| RPS / 延迟基线 | 不同设计版本、优化前后的吞吐量对比 |
| 回归验收 | Errors=0、SSH 稳定、短连接可跑完 |
| **不是** | 定位具体 CPU 热点（交给 perf） |

### 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j2
```

VS Code CMake 插件：Build Type 选 **Release** → Configure → Build。产物：`build/src/server/server`。

### tmux 分工（两窗格）

| 窗格 | 职责 |
|------|------|
| A | 常驻 server |
| B | 跑 wrk |

```bash
tmux new -s bench
```

**窗格 A — 启动 server：**

```bash
./build/src/server/server > /tmp/server.log 2>&1
```

**窗格 B — 确认后再压：**

```bash
curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:8888/
free -h
```

### 递进压测（Keep-Alive）

每组稳定、**Errors=0** 后再加一档；`-d` 先用 10s，正式基线再用 30s。

```bash
# 0. 热身（首次必跑）
wrk -t1 -c10 -d5s http://127.0.0.1:8888/

# 1. 轻量基线
wrk -t1 -c10 -d10s http://127.0.0.1:8888/
wrk -t1 -c20 -d10s http://127.0.0.1:8888/
wrk -t2 -c20 -d30s http://127.0.0.1:8888/   # 主基线（BENCH-002 ≈ 32.8k RPS）
```

| 阶段 | `-t` | `-c` | `-d` | 说明 |
|------|------|------|------|------|
| 热身 | 1 | 10 | 5s | 首次压测、换设计版本后复验 |
| KA 基线 | 1～2 | 10～20 | 10～30s | 入库对比 |
| 上限试探 | 2 | ≤20 | ≤30s | 仅 KA；勿 `-c50` / `-c100` |

### 短连接（显著重于 Keep-Alive）

每条请求：**accept → 建连 → 响应 → handleDead → remove**。**勿与 KA 使用相同 `-t/-c/-d`。**

```bash
wrk -t1 -c5 -d5s -H "Connection: close" http://127.0.0.1:8888/
wrk -t1 -c10 -d10s -H "Connection: close" http://127.0.0.1:8888/
```

| 阶段 | `-t` | `-c` | `-d` | 说明 |
|------|------|------|------|------|
| close 基线 | 1 | 5～10 | 5～10s | 单独递进，**不用 30s 起步** |

### 压测中观察（可选）

```bash
watch -n1 'free -h | head -2'
top -H -p $(pgrep -x server)
```

### 异常排查

```bash
sudo dmesg -T | tail -20   # 是否 OOM Killed
pgrep -a server
```

### wrk 填表

- 复制 `TEMPLATE.md` → `BENCH-NNN_..._wrk_....md`
- 第 4 节：粘贴**完整** wrk 终端输出 + 汇总表
- 测试类型：`wrk 基线` 或 `wrk 对比` / `优化后复测`

---

## perf 采样

### 用途

| 目标 | 说明 |
|------|------|
| 找 CPU 热点 | 哪条调用链占时间最多（HttpProcess、Buffer、锁、分配器等） |
| 指导 v10.2+ 优化 | 先优化火焰图里最宽的函数 |
| **不是** | 再验 wrk RPS 上限；同跑 wrk 时 RPS 会低于 BENCH-002 |

**负载对齐**：Keep-Alive `wrk -t2 -c20 -d30s`（与 BENCH-002 主基线一致），但 **不要** 把同跑 perf 时的 RPS 与 BENCH-002 直接对比。第一轮 **只做 Keep-Alive**，短连接噪声大。

### 前置条件

**perf 已安装：**

```bash
perf --version
# 未找到：sudo apt install -y linux-tools-common linux-tools-$(uname -r)
```

**采样权限：**

```bash
cat /proc/sys/kernel/perf_event_paranoid
```

| 值 | 建议 |
|----|------|
| ≤1 | 一般可直接 `perf record` |
| 2（ECS 常见） | 使用 `sudo perf record` |
| 3 | 必须 sudo |

**构建（须带符号，火焰图才能显示函数名）：**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build -j2
```

VS Code CMake：Build Type 选 **RelWithDebInfo** → Configure → Build。

```bash
file build/src/server/server   # 期望 with debug_info / not stripped
```

**火焰图脚本（一次性）：**

```bash
git clone https://github.com/brendangregg/FlameGraph.git ~/FlameGraph
export PATH="$HOME/FlameGraph:$PATH"
mkdir -p benchmark_log/artifacts
```

SVG / `perf.data` 体积大，**默认不入库**；路径写入 BENCH 第 5 节。

### tmux 分工（三窗格）

```bash
tmux new -s perf
free -h   # available 过低则不要开 perf+wrk
```

| 窗格 | 职责 | 快捷键 |
|------|------|--------|
| A | 常驻 server | 默认 |
| B | wrk 压测 | `Ctrl+b` `%` |
| C | perf 采样 | 再 `%` 或 `"` |

**步骤 0 — 停旧进程（若需要）：**

```bash
kill -SIGTERM $(pgrep -x server) 2>/dev/null; sleep 1
```

**步骤 1 — 窗格 A：**

```bash
./build/src/server/server > /tmp/server.log 2>&1
curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:8888/
pgrep -x server
```

**步骤 2 — 窗格 B（与 C 同时，30s）：**

```bash
wrk -t1 -c10 -d5s http://127.0.0.1:8888/          # 可选热身
wrk -t2 -c20 -d30s http://127.0.0.1:8888/          # 正式负载
```

**步骤 3 — 窗格 C（wrk 开始后几秒内启动）：**

```bash
SERVER_PID=$(pgrep -x server)
sudo perf record -F 997 -g -p "$SERVER_PID" -- sleep 30
```

| 参数 | 含义 |
|------|------|
| `-F 997` | 采样频率，避免与时钟节拍对齐 |
| `-g` | 记录调用栈 |
| `-p PID` | 只采 server |
| `sleep 30` | 与 wrk `-d30s` 对齐 |

栈展开失败、火焰图大量 `[unknown]` 时：

```bash
sudo perf record -F 997 --call-graph dwarf -p "$SERVER_PID" -- sleep 30
```

**步骤 4 — 文本报告：**

```bash
sudo perf report --stdio -g --no-children | tee /tmp/perf_report.txt
```

| 符号类型 | 通常含义 | 优化优先级 |
|----------|----------|------------|
| `epoll_wait` | 等 I/O | 正常；RPS 已高则先看用户态 |
| `read` / `write` | syscall | 视占比 |
| `HttpProcess` / `parse` | HTTP 路径 | **高** |
| `Buffer::` / `memcpy` | 缓冲拷贝 | **高** |
| `std::string` / `malloc` | 分配 | **高** |
| `pthread` / `mutex` | 锁竞争 | 视占比 |

**步骤 5 — 火焰图：**

```bash
NNN=003   # 换成 BENCH 编号
sudo perf script | stackcollapse-perf.pl | flamegraph.pl \
  > benchmark_log/artifacts/BENCH-${NNN}_flamegraph.svg
cp perf.data benchmark_log/artifacts/BENCH-${NNN}_perf.data
```

读图：**横轴** = 样本占比（越宽越热）；**纵轴** = 调用栈；找最宽的**用户态**帧，不要只盯 `epoll_wait`。

### perf 填表

1. 复制 `TEMPLATE.md` → `BENCH-003_YYYYMMDD_perf_v10.1.md`
2. 元信息：`设计版本 = v10.1`，`测试类型 = perf 采样`
3. 第 3 节：`RelWithDebInfo`
4. 第 4 节：同跑 wrk 完整输出（备注：与 perf 同跑，RPS 不与 BENCH-002 比）
5. 第 5 节：采样命令、产物路径、热点摘要表 + 可选 `perf report` 摘录
6. 更新 `docs/DESIGN_v10.md` v10.1 三行表

### perf 常见问题

| 现象 | 处理 |
|------|------|
| `Permission denied` | `sudo perf record` |
| 火焰图全是 `[unknown]` | `RelWithDebInfo` 重建；或 `--call-graph dwarf` |
| `pgrep` 无输出 | 确认 server 已启动；用 `pgrep -x server` |
| SSH 卡死 | 只用 `-c20`；`free -h`；tmux |
| RPS 低于 BENCH-002 | **预期**，perf 条目不与纯 wrk 比 |
| `stackcollapse-perf.pl` 找不到 | 安装 FlameGraph 并加入 PATH |

### perf 命令速查

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build -j2
./build/src/server/server > /tmp/server.log 2>&1
wrk -t2 -c20 -d30s http://127.0.0.1:8888/
sudo perf record -F 997 -g -p $(pgrep -x server) -- sleep 30
sudo perf report --stdio -g --no-children | head -80
sudo perf script | stackcollapse-perf.pl | flamegraph.pl > benchmark_log/artifacts/BENCH-003_flamegraph.svg
```

---

## 索引

| 编号 | 设计版本 | 文件 | 摘要 |
|------|----------|------|------|
| BENCH-001 | v9 | [BENCH-001_20260523_wrk_baseline.md](./BENCH-001_20260523_wrk_baseline.md) | KA ~29k RPS；close 未完成 |
| BENCH-002 | v10.0 | [BENCH-002_20260523_wrk_v10.0.md](./BENCH-002_20260523_wrk_v10.0.md) | KA ~33k RPS；close 轻量完成 |
| BENCH-003 | v10.1 | （待建：perf 热点） | — |
