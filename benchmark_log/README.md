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
- **perf 里的 wrk 只造负载**：版本验收 RPS 以 §4 纯 wrk（Release）为准，不与 perf 同跑时的 RPS 对比。

## 命名规则

所有 wrk / perf 测试均针对某一**设计版本**（`v9`、`v10.0` …，对齐 `docs/DESIGN_v*.md`）。**一版本一份报告**：版本号一致则代码一致，wrk 与 perf 合写在同一 md 文件。

| 部分 | 含义 | 谁定 |
|------|------|------|
| **设计版本** | 被测 server 代码所在版本 | 人工（与 DESIGN 一致） |
| **日期** | `YYYYMMDD` | 测试日 |
| **简述** | 可选后缀，如 `bench`、`wrk_baseline` | 人工 |

**记录文档**（沿用原 `BENCH-NNN_日期_简述` 风格，NNN 换为版本号）：

`benchmark_log/{版本}_{YYYYMMDD}_{简述}.md`

例：`v10.0_20260523_bench.md`、`v9_20260523_wrk_baseline.md`

**产物**（默认 gitignore）：`benchmark_log/artifacts/{版本}_wrk.txt`、`{版本}_perf.data` 等

**升版**：仅 `src/` server 业务代码变更时递增 v10.x。


## 使用方式

1. 复制 [`TEMPLATE.md`](./TEMPLATE.md) 为新文件，按上表命名。
2. 元信息填 **设计版本** 与日期。
3. 按模板填写环境、命令、**完整原始输出**与汇总表（§4 wrk、§5 perf；**测试人**统一填 `luxiaohang`）。
4. 更新本文「索引」表（**仅追加**新版本行，不改旧报告内容）；**仅 src 代码变更时**在 `docs/DESIGN_v10.md` **追加**下一 v10.x 小节，不改写已有小节。

## 测试阶段顺序（示例）

```text
v9 手动 wrk → v10.0 手动 wrk + 自动 perf（一份报告）→ src 优化 v10.1 → v10.1 新报告
```

---

## 手动 wrk

### 用途

| 目标 | 说明 |
|------|------|
| RPS / 延迟基线 | 不同设计版本、优化前后的吞吐量对比 |
| 回归验收 | Errors=0、SSH 稳定、短连接可跑完 |
| **不是** | 定位具体 CPU 热点（交给 perf） |

### 构建（Release）

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j2
```

VS Code CMake 插件：Build Type 选 **Release** → Configure → Build。产物：`build/src/server/server`。

### tmux 双窗格

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

**分屏** — `Ctrl+b` 然后 `%`。

**窗格 B — 确认后再压：**

```bash
curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:8888/
free -h
```

### 两个独立终端

未安装 tmux 时：终端 1 跑 server，终端 2 跑 wrk。

### 递进压测（Keep-Alive）

每组稳定、**Errors=0** 后再加一档；`-d` 先用 10s，正式基线再用 30s。

```bash
# 0. 热身（首次必跑）
wrk -t1 -c10 -d5s http://127.0.0.1:8888/

# 1. 轻量基线
wrk -t1 -c10 -d10s http://127.0.0.1:8888/
wrk -t1 -c20 -d10s http://127.0.0.1:8888/
wrk -t2 -c20 -d30s http://127.0.0.1:8888/   # 主基线（v10.0 ≈ 32.8k RPS）
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

### 填表

- 复制 `TEMPLATE.md` → `{版本}_{日期}_{简述}.md`
- 第 4 节：粘贴**完整** wrk 终端输出 + 汇总表
- 测试类型：`wrk 基线` / `wrk+perf` / `优化后复测`

HTTP 功能验收用 client 代替 wrk，见 [`scripts/SCRIPTS.md`](../scripts/SCRIPTS.md)。

---

## 自动 perf

**推荐**：一条命令完成 RelWithDebInfo 重编、起 server、wrk 造负载、perf 采样、火焰图。**须指定 `-v <版本>`**。

### 命令

```bash
bash scripts/perf_bench.sh -v v10.0
```

已构建且不想重编：

```bash
bash scripts/perf_bench.sh --skip-build -v v10.0
```

其他子命令：

```bash
bash scripts/perf_bench.sh check
bash scripts/perf_bench.sh flamegraph -v v10.0
bash scripts/perf_bench.sh --stop-server -v v10.0
```

流程：停 server → 删 `build/` → RelWithDebInfo 重编 → 起 server → wrk+perf 并行 30s → 生成 **符号表** 与 **火焰图 SVG**。

### 前置条件

```bash
perf --version
# 未找到：sudo apt install -y linux-tools-common linux-tools-$(uname -r)
cat /proc/sys/kernel/perf_event_paranoid   # ECS 常见为 2，需 sudo perf
```

| 值 | 建议 |
|----|------|
| ≤1 | 一般可直接 `perf record` |
| 2（ECS 常见） | 使用 `sudo perf record` |
| 3 | 必须 sudo |

FlameGraph（脚本可自动克隆到 `~/FlameGraph`）：

```bash
git clone https://github.com/brendangregg/FlameGraph.git ~/FlameGraph
export PATH="$HOME/FlameGraph:$PATH"
mkdir -p benchmark_log/artifacts
```

### 产物

路径 `benchmark_log/artifacts/`（SVG / perf.data 默认不入库，写入该版本报告 §5）：

| 文件 | 内容 |
|------|------|
| `{版本}_wrk.txt` | 同跑 wrk 输出（**RPS 不作版本验收**） |
| `{版本}_perf.data` | perf 原始数据 |
| `{版本}_perf_report.txt` | **分层符号表**（§1 内核边界 + §2 server All/Self，≥ 0.1%） |
| `{版本}_flamegraph.svg` | **火焰图**（调用链，浏览器打开） |

> 符号表：**两层** — §1 内核边界（`perf script` 取用户→内核第一个符号，不展开内核内部）+ §2 server 用户态（`perf report` inclusive，看 **All** 列排序）。

### 读 perf 产物（符号表 + 火焰图）

两份文件来自同一份 `perf.data`，分工阅读：

| 产物 | 回答的问题 | 怎么用 |
|------|------------|--------|
| **`_perf_report.txt` 符号表** | 内核花在哪些入口；用户态哪些函数最热 | §1 看 `__x64_sys_*` / `entry_SYSCALL_*` 等边界；§2 按 **All** 从高到低扫 server 符号 |
| **`_flamegraph.svg` 火焰图** | 调用链：热点从哪条路径来、如何分叉 | 浏览器打开；Search 符号名；点宽条放大子树 |

**符号表（分层）**

- **§1 内核态**：`perf script` + `stackcollapse-perf.pl --kernel`，从最外层 caller 向里找 **第一个** `_[k]` 内核符号（等价于用户→内核边界），**不含**更深层 `do_*` / `tcp_*`。
- **§2 用户态 (server)**：`perf report --sort symbol --dsos=server -g none`（inclusive，**不用** `--no-children`）。
- **§2 表头**：报告内精简为 **`All / Self / Symbol` 三列**（脚本去掉 perf IPC 宽表与点线分隔）；perf 原始列名 Children 归一为 All。
- **All 与 Self**（§2，分母均为 **全部 perf 样本**）：
  - **All**：栈上 **经过** 该函数及其子函数的样本占比（含 callees；定优化优先级 **看此列**）
  - **Self**：PC **仅落在该函数体内** 的样本占比（不含 callees；判断热点在自身还是下游）
  - **勿** 将同一行的 Self + All 相加；**勿** 将表中各行百分比相加（父子行重叠计数）
- **读法**：§1 判断 syscall/内核入口占比；§2 定 src 优化优先级；`invoke`/`lambda` 多为 `std::function` 间接调用。
- **`PERF_REPORT_PERCENT_LIMIT`** 可调阈值（默认 `0.1`）；完整调用链见火焰图。

**火焰图**

- **横轴（宽度）** = 该栈路径占样本比例（越宽越热）；**不是**时间轴。
- **纵轴（高度）** = 调用深度（越往上越靠近上层 caller）。
- **Search** 高亮符号；**点击** 某条放大以该帧为根的子树。
- 优先看最宽的 **用户态** 帧（如 `onHttp`、`enqueue`、`bufferToSck`），再理解其下的 `write`、`tcp_*`、`futex`；不要只盯内核 leaf。

**推荐流程（定方向 → 落方案）**

1. 符号表：§1 内核边界（syscall 入口占比）→ §2 server 按 **All** 排序。
2. 火焰图：对 §2 前几名 Search，确认从 `EventLoop::loop` 等根上的调用分支。
3. 将结论写入该版本报告 §5.4「热点摘要」；§5.5 可粘贴符号表前几行或火焰图关键路径文字。
4. wrk RPS 仍以报告 §4 为准；perf 只说明 CPU 花在哪，不代替版本验收。

### 用途与注意

| 目标 | 说明 |
|------|------|
| 定 CPU 优化方向 | 先读 **符号表** 前几名（HttpProcess、Buffer、enqueue、write 等） |
| 定具体改哪条路径 | 再读 **火焰图** 里对应宽条的上游 caller |
| 指导下一版 src | 优先动符号表与火焰图 **都宽** 的用户态路径 |
| **不是** | 再验 wrk RPS；同跑 wrk 仅保证采样期间有负载 |

脚本内 wrk 参数：`wrk -t2 -c20 -d30s`（Keep-Alive）。**不要**把 `{版本}_wrk.txt` 里的 RPS 与「手动 wrk」§4 对比。

同版本产物已存在时，脚本会列出路径并询问 `[y/N]` 是否覆盖；确认后 **先删除** 旧文件（`run` 含 `perf.data`，`flamegraph` 仅删 report/SVG），再重新生成。非交互可设 `PERF_BENCH_FORCE=1`。

### 填表

1. 在**该版本**报告 md 中填写 §5（与 §4 wrk 同文件）
2. 元信息：**设计版本**、测试类型
3. 第 5 节：产物路径用 `{版本}_*` 格式；§5.4 热点摘要（符号表 + 火焰图）；§5.5 可贴符号表前几行

### 常见问题

| 现象 | 处理 |
|------|------|
| `Permission denied` | `sudo perf record` |
| 符号表上万行、含 `\|---` 分支 | 调用树异常；`git pull` 后 `flamegraph -v` 重生 |
| 表头为 `All/Self`（或 perf 原始 `Children/Self`）且无 `\|---` | **正常** flat；按 **All** 列读 inclusive |
| 符号表只有少量 `[unknown]` | 正常；若大面积 unknown → RelWithDebInfo 重建 |
| `pgrep` 无输出 | 确认 server 已启动；用 `pgrep -x server` |
| SSH 卡死 | 只用 `-c20`；`free -h`；tmux |
| 同跑 wrk RPS 与 §4 不一致 | **预期**（构建类型与 perf 开销不同） |
| `stackcollapse-perf.pl` 找不到 | 安装 FlameGraph 并加入 PATH |

---

## 手动 perf

需要自行对齐 wrk 与 perf 采样窗口时使用；日常优先「自动 perf」。

### 构建（RelWithDebInfo，须带符号）

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build -j2
file build/src/server/server   # 期望 with debug_info / not stripped
```

栈展开失败、火焰图大量 `[unknown]` 时改用 `--call-graph dwarf`（见下方采样命令）。

### tmux 三窗格

| 窗格 | 职责 | 快捷键 |
|------|------|--------|
| A | 常驻 server | 默认 |
| B | wrk 造负载 | `Ctrl+b` `%` |
| C | perf 采样 | 再 `%` 或 `"` |

```bash
tmux new -s perf
free -h   # available 过低则不要开 perf+wrk
```

**窗格 A：**

```bash
./build/src/server/server > /tmp/server.log 2>&1
curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:8888/
pgrep -x server
```

**窗格 B（与 C 同时，30s）：**

```bash
wrk -t1 -c10 -d5s http://127.0.0.1:8888/          # 可选热身
wrk -t2 -c20 -d30s http://127.0.0.1:8888/
```

**窗格 C（wrk 开始后几秒内启动）：**

```bash
SERVER_PID=$(pgrep -x server)
sudo perf record -F 997 --call-graph dwarf -p "$SERVER_PID" -o benchmark_log/artifacts/v10.0_perf.data -- sleep 30
```

| 参数 | 含义 |
|------|------|
| `-F 997` | 采样频率，避免与时钟节拍对齐 |
| `--call-graph dwarf` | 栈展开（比 `-g` 更稳） |
| `-p PID` | 只采 server |
| `sleep 30` | 与 wrk `-d30s` 对齐 |

**符号表（与脚本一致，inclusive flat，Overhead ≥ 0.1%）：**

```bash
sudo perf report -i benchmark_log/artifacts/v10.0_perf.data \
  --stdio --sort comm,dso,symbol --percent-limit 0.1 -g none \
  > benchmark_log/artifacts/v10.0_perf_report.txt
head -40 benchmark_log/artifacts/v10.0_perf_report.txt
```

（不要加 `--no-children`；不要加 `-g` 以外的调用图选项。）

**火焰图（需 FlameGraph）：**

```bash
git clone https://github.com/brendangregg/FlameGraph.git ~/FlameGraph
export PATH="$HOME/FlameGraph:$PATH"
mkdir -p benchmark_log/artifacts
BASE=v10.0   # 设计版本
sudo perf script -i benchmark_log/artifacts/${BASE}_perf.data | stackcollapse-perf.pl | flamegraph.pl \
  > benchmark_log/artifacts/${BASE}_flamegraph.svg
```

读法见上文「读 perf 产物（符号表 + 火焰图）」。

### perf 符号参考

| 符号类型 | 通常含义 | 优化优先级 |
|----------|----------|------------|
| `epoll_wait` | 等 I/O | 正常；RPS 已高则先看用户态 |
| `read` / `write` | syscall | 视占比 |
| `HttpProcess` / `parse` | HTTP 路径 | **高** |
| `Buffer::` / `memcpy` | 缓冲拷贝 | **高** |
| `std::string` / `malloc` | 分配 | **高** |
| `pthread` / `mutex` | 锁竞争 | 视占比 |

### 填表

与「自动 perf」相同：写入该版本报告 §5；wrk 同跑 RPS **不作版本验收**。

---

## 索引

| 设计版本 | 文件 | 摘要 |
|----------|------|------|
| v9 | [v9_20260523_wrk_baseline.md](./v9_20260523_wrk_baseline.md) | KA ~29k RPS；close 未完成 |
| v10.0 | [v10.0_20260523_bench.md](./v10.0_20260523_bench.md) | KA ~32.8k RPS；perf：enqueue+write 热点 |
