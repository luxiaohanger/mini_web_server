# 运行脚本说明

项目提供 `scripts/up.sh`，用于在 Linux 上快速启动 **HTTP server** 与 **测试 client**。

---

## 前置条件

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

## 方式一：tmux 一键启动（推荐）

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
| `Ctrl+b` 然后 `d` | 分离会话（后台继续跑） |
| `Ctrl+b` 然后 `x` | 关闭当前窗格 |
| `tmux attach -t mini_web_server` | 重新附着 |
| `tmux kill-session -t mini_web_server` | 结束整个会话 |

进程结束后窗格会提示「按回车关闭」。

---

## 方式二：两个终端分别启动

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

## 方式三：curl 手工验证

不依赖 client，可用 curl 对照：

```bash
curl -v http://127.0.0.1:8888/
curl -v --http1.1 http://127.0.0.1:8888/a http://127.0.0.1:8888/b
curl -v -H "Connection: close" http://127.0.0.1:8888/
curl -v -X POST http://127.0.0.1:8888/
```

idle 粗测：Keep-Alive 发一次请求后 `sleep 65`，再在同一 `nc` 连接上发请求，应断开。

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

## 常见问题

**client 连接失败**

- 确认 server 已启动且监听 8888
- 用 `ss -tlnp | grep 8888` 或 `curl` 检查

**`-t` 很慢**

- 正常：需等待约 65s 验证 idle

**未安装 tmux**

- `up.sh` 会打印两条手动命令，不会自动起进程
- 或使用「方式二」两个终端

**修改端口**

- 需同时改 `src/server/main.cpp` 中 `listenPort` 与 client 的 `host`/`port` 参数

**停服**

- 当前 server 无程序内 graceful shutdown；在 server 终端 `Ctrl+C` 或 kill 进程（见 `issue_log/open_issues.md`）

---

## 仅重新编译

```bash
cmake --build build
# 或
bash scripts/up.sh --build -n 1
```
