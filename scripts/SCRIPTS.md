# 运行脚本说明

项目提供 `scripts/up.sh`，用于在 Linux 上快速启动 **HTTP server** 与 **测试 client**。

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

## 启动

### 方式一：tmux 一键启动（推荐）

```bash
bash scripts/up.sh                    # HTTP 四用例 1 轮（-n 1）
bash scripts/up.sh -n 5              # HTTP 四用例 5 轮
bash scripts/up.sh -t                 # idle 超时验收（约 65s，需等待）
bash scripts/up.sh --build -n 3       # 先编译，再跑 3 轮 HTTP
bash scripts/up.sh -t 127.0.0.1 8888
bash scripts/up.sh 5                  # 兼容旧用法，等同 -n 5
```

脚本行为：

1. 左窗格：启动 `server`（常驻，处理 HTTP 请求）
2. 右窗格：等待 0.5s 后启动 `client`（避免 connect 早于 listen）
3. 附着到 tmux 会话 `mini_web_server`

**tmux 常用操作：**

| 按键 | 作用 |
|------|------|
| `Ctrl+b` 然后 `d` | 分离会话（server/client 后台继续跑） |
| `Ctrl+b` 然后 `x` | 关闭当前窗格 |
| `tmux attach -t mini_web_server` | 重新附着 |
| `tmux kill-session -t mini_web_server` | 结束整个会话（见下方「停止」） |

---

### 方式二：两个终端分别启动

```bash
# 终端 1 — server
./build/src/server/server
# 期望输出：Server is starting!

# 终端 2 — HTTP 多轮四用例
./build/src/client/client -n 1
./build/src/client/client -n 5 127.0.0.1 8888
./build/src/client/client 3          # 兼容旧用法，等同 -n 3

# 终端 2 — idle 超时（单连接 → 等 65s → 应需重连）
./build/src/client/client -t
./build/src/client/client -t 127.0.0.1 8888
```

---

### 方式三：curl 手工验证

不依赖 client，可用 curl 对照：

```bash
curl -v http://127.0.0.1:8888/
curl -v --http1.1 http://127.0.0.1:8888/a http://127.0.0.1:8888/b
curl -v -H "Connection: close" http://127.0.0.1:8888/
curl -v -X POST http://127.0.0.1:8888/
```

idle 粗测：Keep-Alive 发一次请求后 `sleep 65`，再在同一 `nc` 连接上发请求，应断开。

---

### client 参数

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

### tmux 会话（`up.sh` 启动）

| 场景 | 操作 |
|------|------|
| 只停 server | 切到 **左窗格**（server），**Ctrl+C** |
| 只停 client | 切到右窗格，client 跑完会自行结束；或 **Ctrl+C** |
| 结束整个会话 | `tmux kill-session -t mini_web_server`（会向各窗格发 SIGHUP，server 若仍存活需先 Ctrl+C 或 `kill -SIGTERM`） |

client 结束后右窗格会提示「按回车关闭」；server 窗格在停服前会一直保持运行。

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

## `up.sh` 选项摘要

```text
bash scripts/up.sh [选项] [-n repeat | -t] [host] [port]

  -n <repeat>    HTTP 四用例重复轮数（默认 1）
  -t             idle 超时验收（约 65s，勿与 -n 同用）
  -b, --build    启动前 cmake --build build
  -h, --help     帮助
  <repeat>       无 -n/-t 时，正整数表示 -n repeat（兼容旧用法）
```

环境变量（可选）：

| 变量 | 默认 | 说明 |
|------|------|------|
| `BUILD_DIR` | `<项目根>/build` | 构建目录 |
| `SESSION_NAME` | `mini_web_server` | tmux 会话名 |

---

## perf 采样与火焰图（`perf_bench.sh`）

一键完成：依赖检查 → wrk 压测 + perf 采样（默认 **dwarf 栈**，减轻 `[unknown]`）→ 火焰图 + 文本报告。

### 前置

- 二进制建议 **RelWithDebInfo**（VS Code CMake 选 Build Type 后全量构建）
- 已安装 `perf`、`wrk`
- FlameGraph 未安装时脚本会自动 `git clone` 到 `~/FlameGraph`

### 用法

```bash
# server 已在跑（推荐：tmux 窗格 A 常驻 server）
bash scripts/perf_bench.sh -n 003

# 自动后台启动 server（日志 /tmp/server.log），采样完保留 server
bash scripts/perf_bench.sh --start-server -n 003

# 采样完并停掉本脚本拉起的 server
bash scripts/perf_bench.sh --start-server --stop-server -n 003

# 只检查 perf / wrk / FlameGraph / 二进制符号
bash scripts/perf_bench.sh check

# 从已有 perf.data 只生成 SVG + report
bash scripts/perf_bench.sh flamegraph -n 003 -i benchmark_log/artifacts/BENCH-003_perf.data
```

### 常用选项

| 选项 | 默认 | 说明 |
|------|------|------|
| `-n NNN` | 003 | BENCH 编号，产物文件名前缀 |
| `-d 秒` | 30 | wrk 与 perf 采样时长 |
| `-t` / `-c` | 2 / 20 | wrk 线程与连接数 |
| `--fp` | — | 改用帧指针 `-g`（默认 dwarf） |
| `--no-warmup` | — | 跳过 wrk 5s 热身 |
| `--skip-wrk` | — | 不跑 wrk，仅 perf |

### 产物（`benchmark_log/artifacts/`）

| 文件 | 内容 |
|------|------|
| `BENCH-NNN_wrk.txt` | wrk 完整输出 |
| `BENCH-NNN_perf.data` | perf 原始数据 |
| `BENCH-NNN_perf_report.txt` | `perf report` 文本 |
| `BENCH-NNN_flamegraph.svg` | 火焰图 |

大文件默认被 `artifacts/.gitignore` 忽略；路径与热点摘要写入 BENCH 条目第 5 节。流程说明见 [`benchmark_log/README.md`](../benchmark_log/README.md)。

### 仍有大量 `[unknown]`

1. 确认 RelWithDebInfo 全量重编：`bash scripts/perf_bench.sh check`
2. 脚本已默认 dwarf；若仍不行，重编时加帧指针：

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS="-fno-omit-frame-pointer -g" \
  -DCMAKE_C_FLAGS="-fno-omit-frame-pointer -g"
cmake --build build -j2 --clean-first
bash scripts/perf_bench.sh --start-server -n 003
```

---

## 常见问题

**client 连接失败**

- 确认 server 已启动且监听 8888
- 用 `ss -tlnp | grep 8888` 或 `curl` 检查

**`-t` 很慢**

- 正常：需等待约 65s 验证 idle

**未安装 tmux**

- `up.sh` 会打印两条手动命令，不会自动起进程
- 或使用「方式二」两个终端

**server Ctrl+C 后 hang**

- 应为 v9 之前的行为；当前版本 Main/Sub 经 `enqueueTask` 唤醒 loop，若仍 hang 见 `issue_log/open_issues.md`

**仅重新编译**

```bash
cmake --build build
# 或
bash scripts/up.sh --build -n 1
```
