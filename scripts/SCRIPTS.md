# 运行脚本说明

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

**须指定设计版本 `-v`**；产物按 `{版本}_*` 命名（一版本一份）：

```bash
bash scripts/perf_bench.sh -v v10.0
```

流程：停 server → 删 `build/` → RelWithDebInfo 重编 → 起 server → wrk + dwarf perf → 火焰图。

### 其他用法

```bash
bash scripts/perf_bench.sh --skip-build -v v10.0
bash scripts/perf_bench.sh check
bash scripts/perf_bench.sh --stop-server -v v10.0
bash scripts/perf_bench.sh flamegraph -v v10.0
```

### 常用选项

| 选项 | 说明 |
|------|------|
| `-v <版本>` | **必填**（run / flamegraph），如 `v10.0` |
| `-d 秒` | 采样时长（默认 30） |
| `--skip-build` | 跳过重编 |

### 产物（`benchmark_log/artifacts/`）

| 文件 | 内容 |
|------|------|
| `{版本}_wrk.txt` | wrk 输出 |
| `{版本}_perf.data` | perf 数据 |
| `{版本}_perf_report.txt` | 文本报告 |
| `{版本}_flamegraph.svg` | 火焰图 |

记录文档：`benchmark_log/{版本}_{YYYYMMDD}_bench.md`（wrk + perf 同一份）。详见 [`benchmark_log/README.md`](../benchmark_log/README.md)。

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
