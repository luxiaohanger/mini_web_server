# 主从多 Reactor + HTTP/1.1 最小实现 架构设计

在 v6「`ConnState` + `working` + 智能指针所有权」之上，业务从 **Echo 回显** 替换为 **HTTP/1.1 最小子集**：仅处理合法 **GET**；非法请求返回 **400**；支持 **Keep-Alive** 与 **Connection: close** 短连接；**POST** 等非 GET 返回 400。连接收束仍沿用 v6 半关闭语义，并采用 **延迟 `remove`**（FIX-026）避免在 `Channel::handle` 回调栈内析构连接。

## 本版本重点

- **`HttpProcess`**：I/O 线程增量解析（`parse` / `releaseRequest` / `reset`）；worker 线程无状态组包（`buildResponse` / `build400`）
- **`Buffer` 辅助**：`findCRLF` / `retrieve` 支撑按行解析与粘包/半包
- **`Connection::onHttp`**：读回调内解析；完整 GET 投递 worker；响应经 `enqueueTask` 回 owner 写入
- **`handleDead`**：`disableAll()` + `enqueueTask` 中 `erase`（对齐 `EventLoop` 先 poll 后 tasks，关闭 OPEN-005 类回调栈析构问题）
- **验收**：`src/client/main.cpp` 内置四组 HTTP 用例；`scripts/SCRIPTS.md` / `up.sh` 支持一键双窗测试

## 协议范围（stage11）

| 支持 | 不支持（本阶段） |
|------|------------------|
| `GET /path HTTP/1.1`（或 `HTTP/1.0`） | POST body、路由区分、静态文件 |
| Header：`Host`、`Connection`、`Content-Length`（GET 须为 0） | Chunked、Pipeline 语义细化 |
| 固定 200 + `Hello, World!` | HTTPS、HTTP/2 |

非法（非 GET、格式错、`Content-Length > 0` 等）→ `400 Bad Request` + `Connection: close`。请求行/头超长 → `kLineTooLong` → `disableReading` + `peerClose` 收束。

## 架构流程

```text
Client (HTTP)
  |
  v
MainReactor / Acceptor
  |
  v
Server round-robin -> SubReactor
  |
  v
Connection / Channel
  |
  | EPOLLIN -> readFromSck -> readBuffer
  v
HttpProcess::parse (I/O 线程，状态机)
  |
  | kComplete -> releaseRequest -> working++
  v
ThreadPool
  |
  | buildResponse(req) / build400()
  v
owner EventLoop enqueueTask (working--, sendHttpOnLoop, trySendToSck)
  |
  | peerClose && working==0 && 写排空 -> handleDead
  v
enqueueTask(remove)  // 本轮 poll 结束后再 erase map
```

## 解析状态机（`HttpProcess`）

```text
kExpectRequestLine --[一行]--> 解析 GET 行
        | 失败 -> kError
        v
kExpectHeaders --[Header 行]--> Host / Connection / Content-Length
        | 空行 -> kComplete（须 contentLength==0）
        | 超长 / 头过多 -> kLineTooLong
        v
releaseRequest -> 重置为 kExpectRequestLine（粘包：同连接继续 while parse）
```

## 连接状态机（继承 v6）

与 v6 相同：`connected` → `peerClose`（EOF 或短连接响应后）→ `dead` → **延迟** `remove`。

```text
handleDead():
  state = dead
  channel->disableAll()
  enqueueTask([self]{ removeConnectionCallBack(sck); })
```

**线程边界不变**：worker **不**读写 `readBuffer` / `writeBuffer`；仅对 `HttpRequest` 快照调用 `buildResponse`。

## 类职责补充

### `HttpProcess`

| 方法 | 线程 | 职责 |
|------|------|------|
| `parse(Buffer*)` | owner I/O | 增量解析；`kNeedMore` / `kComplete` / `kError` / `kLineTooLong` |
| `releaseRequest()` | owner I/O | 取出合法 GET，重置解析状态 |
| `reset()` | owner I/O | 错误后清空（如已发 400） |
| `buildResponse(req)` | worker | 固定 200 + body + Connection 头 |
| `build400()` | worker / I/O | 非法请求响应 |

### `Connection`（相对 v6）

- 成员增加 `unique_ptr<HttpProcess> httpProcess_`
- `handleReadCallBack` → `onHttp()`（替代 `Echo`）
- `sendHttpOnLoop` / `checkEmptyReadAfterEof`：短连接与 EOF 收束
- `handleDead`：见上，**不再**在回调栈内同步 `erase`

### 测试客户端

- `client [repeat] [host] [port]`：每轮 GET / Keep-Alive 双 GET / close / POST→400
- 仅失败时打印详情；退出码 0/1 表示通过/失败

## 与 v6 的差异摘要

| 主题 | v6 | v7 |
|------|----|----|
| 业务 | Echo 回显 | HTTP/1.1 最小 GET |
| 解析 | 无 | `HttpProcess` 状态机 + `Buffer` 行 API |
| `handleDead` | 同步 `erase`（有回调栈析构风险） | `disableAll` + `enqueueTask` 延迟 remove（FIX-026） |
| 客户端 | 交互式裸写 | HTTP 验收四用例 |
| 文档/脚本 | — | `SCRIPTS.md`、`up.sh` |

## Q & A

1. 为什么解析在 I/O 线程、组包在 worker？

- 与 v5/v6 相同：buffer 与 socket 只属 owner loop；worker 只处理不可变 `HttpRequest` 快照，避免数据竞争。

2. 为什么 `handleDead` 要 `enqueueTask`？

- `EventLoop::loop` 先执行本轮所有 `channel->handle()`，再跑任务队列。同步 `erase` 可能在 `Channel::handle` 未返回时析构 `Connection`/`Channel`（UB）。延迟 remove 与 muduo `runInLoop` 同类。

3. Keep-Alive 下粘包如何处理？

- `onHttp` 中 `while (parse)`：`kComplete` 后不 `break`，继续解析 readBuffer 中下一条请求。

4. 当前如何验收？

```bash
# 终端 1
./build/src/server/server
# 终端 2
./build/src/client/client
# 或 bash scripts/up.sh
```

5. 与 v6 相同的未解决问题？

- 见 `issue_log/open_issues.md`：**OPEN-003**（读错误循环）、**OPEN-004**（`epoll_wait` EINTR）；停服仍为 **OPEN-001/002** 暂缓。

## 后续演进（未实现）

- 路径路由、`/a` `/b` 不同响应
- 静态文件、`sendfile`
- POST/PUT 与 body 读取
- `handleDead` 幂等、读回调 `dead` 早退（可选加固）
