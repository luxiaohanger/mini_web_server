# 运行脚本说明

| 章节 | 内容 |
|------|------|
| [多终端启动](#多终端启动) | tmux、client、curl |
| [client 参数](#client-参数) | `-n` / `-t` 验收 |
| [停止](#停止) | 停 server |
| [perf_bench.sh](#perf-自动测试perf_benchsh) | **子命令、选项、环境变量、产物** |
| [常见问题](#常见问题) | client / 编译 |

---

## 前置条件（构建）

```bash
# 项目根目录，首次构建
cmake -S . -B build
cmake --build build
```

产物路径（默认）：

| 目标 | 路径 |
|------|------|
| server | `build/src/server/server` |
| client | `build/src/client/client` |

server 默认监听 **8888**（见 `src/server/main.cpp`）。

---

## 多终端启动

### tmux 双窗格（推荐）

左窗格跑 server，右窗格跑 client 或 wrk；SSH 断开可 `tmux attach` 恢复。

```bash
tmux new -s mini_web_server
```

**窗格 A（默认）— server：**

```bash
./build/src/server/server
# 期望输出：Server is starting!
```

**分屏** — `Ctrl+b` 然后 `%`（左右）或 `"`（上下）。

**窗格 B — HTTP 功能验收（等 server 就绪后）：**

```bash
sleep 0.5
./build/src/client/client -n 1
./build/src/client/client -n 5 127.0.0.1 8888
./build/src/client/client -t          # idle 超时验收，约 65s
```

**tmux 常用操作：**

| 按键 / 命令 | 作用 |
|-------------|------|
| `Ctrl+b` 然后 `d` | 分离会话（后台继续跑） |
| `Ctrl+b` 然后 `x` | 关闭当前窗格 |
| `tmux attach -t mini_web_server` | 重新附着 |
| `tmux kill-session -t mini_web_server` | 结束整个会话 |

wrk 压测时同样用双窗格：A 常驻 server，B 跑 wrk（见 [`benchmark_log/README.md`](../benchmark_log/README.md)）。

### 两个终端分别启动

```bash
# 终端 1 — server
./build/src/server/server

# 终端 2 — HTTP 多轮四用例
./build/src/client/client -n 1
./build/src/client/client -n 5 127.0.0.1 8888
./build/src/client/client 3          # 兼容旧用法，等同 -n 3

# 终端 2 — idle 超时（单连接 → 等 65s → 应需重连）
./build/src/client/client -t
./build/src/client/client -t 127.0.0.1 8888
```

### curl 手工验证

不依赖 client，可用 curl 对照：

```bash
curl -v http://127.0.0.1:8888/
curl -v --http1.1 http://127.0.0.1:8888/a http://127.0.0.1:8888/b
curl -v -H "Connection: close" http://127.0.0.1:8888/
curl -v -X POST http://127.0.0.1:8888/
```

idle 粗测：Keep-Alive 发一次请求后 `sleep 65`，再在同一 `nc` 连接上发请求，应断开。

---

## client 参数

| 模式 | 用法 | 含义 |
|------|------|------|
| **HTTP 多轮** | `client -n <repeat> [host] [port]` | 每轮跑完 4 个用例，重复 `repeat` 次 |
| **idle 验收** | `client -t [host] [port]` | 同连接 Keep-Alive GET /、/a、/b → 等待 65s → 同连接再 GET 应失败 → 重连验证 |
| **兼容** | `client <repeat> [host] [port]` | 等同 `-n <repeat>` |

默认值：`host=127.0.0.1`，`port=8888`。

#### HTTP 四用例（`-n` 每轮顺序）

1. 单次 `GET /` → 期望 200 + `Hello, World`
2. 同连接两次 GET（Keep-Alive）
3. `Connection: close`
4. `POST /` → 期望 400

#### idle 验收（`-t`）

1. 单连接依次 GET `/`、`/a`、`/b`（Keep-Alive）
2. 等待 **65s**（服务端 idle 约 **60s**）
3. 同一 socket 再发 GET：应失败（连接已被 idle 关闭）
4. 新建连接 GET `/`：应成功

client 源码：`src/client/main.cpp`。

---

## 停止

server 启动后会阻塞在控制线程，等待停服信号；收到信号后走 `Server::stop()`（停 accept → 关连接 → join 线程池），**不应** 依赖 `kill -9` 日常退出。

### 前台 server（单终端 / tmux 左窗格）

| 操作 | 说明 |
|------|------|
| **Ctrl+C** | 发送 SIGINT，触发程序内停服 |
| 期望输出 | `Server stop safely!` 后进程退出、shell 提示符返回 |

### 后台或另开终端

```bash
# 推荐：SIGTERM（与 Ctrl+C 同属 v9 停服路径）
kill -SIGTERM $(pgrep -x server)

# 确认已退出
wait $(pgrep -x server) 2>/dev/null || echo "server 已停止"

# 或检查端口
ss -tlnp | grep 8888    # 无输出表示已释放
```

### tmux 会话

| 场景 | 操作 |
|------|------|
| 只停 server | 切到 server 窗格，**Ctrl+C** |
| 只停 client | client 跑完会自行结束；或 **Ctrl+C** |
| 结束整个会话 | 先在各窗格停 server，再 `tmux kill-session -t mini_web_server` |

### 停服验收（可选）

```bash
./build/src/server/server &
SERVER_PID=$!
sleep 1
curl -s http://127.0.0.1:8888/ >/dev/null
kill -SIGTERM "$SERVER_PID"
wait "$SERVER_PID"
echo "exit code: $?"
# 期望：exit code 0，无 hang
```

### 注意

- **`kill -9` / `SIGKILL`**：无法被捕获，跳过 `Server::stop()`，仅紧急强杀时使用。
- **调用约定**：`main` 必须显式调用 `Server::stop()`；当前 `main.cpp` 已保证，无需额外脚本。
- 修改监听端口需同时改 `src/server/main.cpp` 中 `listenPort` 与 client 的 `host`/`port` 参数。

---

## perf 自动测试（`perf_bench.sh`）

一条命令完成 **RelWithDebInfo 重编 → wrk 负载 → perf 采样 → inclusive 符号表 + 火焰图 SVG**。  
**必须** 指定设计版本 `-v`（与 `benchmark_log/{版本}_*_bench.md` 对齐）。

```bash
bash scripts/perf_bench.sh -v v10.0          # 完整流程（默认 run）
bash scripts/perf_bench.sh -h                # 脚本内简要帮助
```

压测记录、符号表读法、手动 perf：见 [`benchmark_log/README.md`](../benchmark_log/README.md)。

---

### 子命令

| 子命令 | 何时用 | 做什么 | 是否重编 |
|--------|--------|--------|----------|
| **`run`**（默认） | 新版本首次 perf，或需干净 RelWithDebInfo 二进制 | 停 server → 删 `build/` → 重编 → 起 server → wrk∥perf → 符号表 + SVG | 是（可用 `--skip-build` 跳过） |
| **`flamegraph`** | 已有 `{版本}_perf.data`，只重生报告/图 | 读 `perf.data` → 生成 `{版本}_perf_report.txt` + `{版本}_flamegraph.svg` | 否 |
| **`check`** | 上线前检查依赖、符号、FlameGraph | 检查 `cmake/perf/wrk/curl`、server 是否含 debug 符号、内存 | 否 |

```bash
bash scripts/perf_bench.sh run -v v10.0              # 可省略 run
bash scripts/perf_bench.sh --skip-build -v v10.0     # 不重编，仍 wrk+perf
bash scripts/perf_bench.sh flamegraph -v v10.0        # 仅 report + SVG
bash scripts/perf_bench.sh check
bash scripts/perf_bench.sh --stop-server -v v10.0    # run 结束后 SIGTERM 停 server
```

---

### 命令行选项

| 选项 | 默认值 | 作用 |
|------|--------|------|
| **`-v <版本>`** | （无，**必填**） | 设计版本号，如 `v10.0`、`v10.1`；产物命名为 `{版本}_*`；格式须匹配 `vN` 或 `vN.x` |
| **`-d <秒>`** | `30` | wrk 持续时间，与 `perf record` 采样时长一致 |
| **`-t <N>`** | `2` | wrk 线程数（`-t2`） |
| **`-c <N>`** | `20` | wrk 连接数（`-c20`）；SSH 环境勿过大，防 OOM |
| **`-u <url>`** | `http://127.0.0.1:8888/` | wrk 目标 URL（Keep-Alive GET） |
| **`-i <file>`** | `artifacts/{版本}_perf.data` | **`flamegraph` 专用**：指定输入 `perf.data` 路径 |
| **`-j <N>`** | `nproc` | `cmake --build` 并行编译线程数 |
| **`--skip-build`** | 关 | 不删 `build/`、不重编；仍停/起 server 并采样（需已有 RelWithDebInfo 二进制） |
| **`--fp`** | 关 | perf 用 **帧指针** `-g` 展开栈；默认 **`dwarf`**（`--call-graph dwarf`） |
| **`--no-warmup`** | 关 | 跳过 wrk 5s 热身（`-t1 -c10 -d5s`） |
| **`--stop-server`** | 关 | 流程结束后 `SIGTERM` 停 server |
| **`--skip-wrk`** | 关 | 只 `perf record`，不跑 wrk（需自行保证采样期间有负载） |
| **`-h` / `--help`** | — | 打印简要用法并退出 |

**说明：**

- **`run` 默认可写产物**：`{版本}_wrk.txt`（除非 `--skip-wrk`）、`{版本}_perf.data`、`{版本}_perf_report.txt`、`{版本}_flamegraph.svg`。
- **`flamegraph` 只覆盖**：`{版本}_perf_report.txt`、`{版本}_flamegraph.svg`（**不删** `perf.data`）。
- 同版本产物已存在 → 提示 **`[y/N]`** 覆盖；确认后 **先删除将被覆盖的旧文件** 再生成。非交互：`PERF_BENCH_FORCE=1`。
- perf 同跑 wrk 的 RPS **不作** 版本 wrk 验收（见 bench 报告 §4 vs §5）。

---

### 环境变量

| 变量 | 默认值 | 含义 |
|------|--------|------|
| **`BUILD_DIR`** | `<项目根>/build` | CMake 构建目录 |
| **`BUILD_JOBS`** | `nproc` | 编译 `-j`；也可用 `-j` 选项 |
| **`ARTIFACTS_DIR`** | `benchmark_log/artifacts` | 产物输出目录 |
| **`FLAMEGRAPH_DIR`** | `$HOME/FlameGraph` | FlameGraph 克隆路径；缺失时脚本自动 `git clone` |
| **`SERVER_LOG`** | `/tmp/server.log` | 后台 server 日志 |
| **`PERF_REPORT_PERCENT_LIMIT`** | `0.1` | 符号表只保留 Overhead ≥ 该值（%）的符号 |
| **`PERF_BENCH_FORCE`** | `0` | 设为 `1` 时跳过覆盖确认，直接删旧产物并重生 |

示例：

```bash
PERF_REPORT_PERCENT_LIMIT=0.2 bash scripts/perf_bench.sh flamegraph -v v10.0
PERF_BENCH_FORCE=1 bash scripts/perf_bench.sh -v v10.0
BUILD_JOBS=2 bash scripts/perf_bench.sh -v v10.0
```

---

### 产物（`benchmark_log/artifacts/`）

| 文件 | 内容 | 入库 |
|------|------|------|
| `{版本}_wrk.txt` | 与 perf 同跑的 wrk 输出（仅写入文件） | 否 |
| `{版本}_perf.data` | perf 原始采样 | 否 |
| `{版本}_perf_report.txt` | **分层符号表**（§1 内核边界 + §2 server All/Self） | 否 |
| `{版本}_flamegraph.svg` | 火焰图（调用链） | 否 |

**符号表生成（脚本内置，勿改口径）：**

| 段落 | 方法 |
|------|------|
| **§1 内核态** | 同火焰图栈 + **负向过滤**（跳过跳板/中断/内核实现）→ 可读 syscall 层 |
| **§2 用户态** | `perf report --sort comm,dso,symbol -g none` 全量 inclusive → 筛 `Shared Object=server` |

| 要点 | 说明 |
|------|------|
| **§1** | 一行一个 **syscall/内核入口**（如 `__x64_sys_epoll_wait`），非整段 `[kernel.kallsyms]` 一行，也非 `do_*`/`tcp_*` 内部 |
| **§2 inclusive** | **不用** `--no-children`；报告表头 **All**（perf 原始列名 Children）；旧版仅 Overhead 时语义同 All |
| **§2 flat** | **`-g none`** → 无 `\|---` 树 |
| **§2 读列** | **All** = 含子函数 **及内核/libc 路径**（排序依据）；**Self** = 仅 server 函数体内；分母均为全部 perf 样本 |
| **读法** | §1 看内核入口；§2 按 All 定 src 优先级；调用链看 SVG |

结论写入 `benchmark_log/{版本}_{YYYYMMDD}_bench.md` §5（模板见 `benchmark_log/TEMPLATE.md`）。

---

### 典型场景

| 场景 | 命令 |
|------|------|
| 新版本完整 perf | `bash scripts/perf_bench.sh -v v10.1` |
| 已编译，只重跑采样 | `bash scripts/perf_bench.sh --skip-build -v v10.1` |
| 只重生符号表/火焰图 | `bash scripts/perf_bench.sh flamegraph -v v10.0` |
| 指定 perf.data | `bash scripts/perf_bench.sh flamegraph -v v10.0 -i /path/to/perf.data` |
| 采样后自动停 server | `bash scripts/perf_bench.sh --stop-server -v v10.0` |
| 检查环境 | `bash scripts/perf_bench.sh check` |

---

### perf 相关常见问题

| 现象 | 处理 |
|------|------|
| `Permission denied`（perf） | ECS 常见 `paranoid=2`，脚本会尝试 `sudo perf` |
| `perf report 未生成 flat 符号表` | 确认 `git pull` 最新脚本；手动：`perf report ... -g none \| head -30` |
| 符号表上万行、有 `\|---` | 非正常 flat；按上条排查 |
| `stackcollapse-perf.pl` 找不到 | 安装 FlameGraph 或让脚本自动克隆到 `~/FlameGraph` |
| 覆盖确认在非交互环境卡住 | `PERF_BENCH_FORCE=1` 或先手动删 `{版本}_*` |

---

## 常见问题

**client 连接失败**

- 确认 server 已启动且监听 8888
- 用 `ss -tlnp | grep 8888` 或 `curl` 检查

**`-t` 很慢**

- 正常：需等待约 65s 验证 idle

**未安装 tmux**

- 使用「两个终端分别启动」

**server Ctrl+C 后 hang**

- 应为 v9 之前的行为；当前版本 Main/Sub 经 `enqueueTask` 唤醒 loop，若仍 hang 见 `issue_log/open_issues.md`

**仅重新编译**

```bash
cmake --build build
```
