# 停服与退出闭环 架构设计

在 v8 TimerQueue + idle 之上，为进程增加 **程序内可停服** 能力：外部信号触发 `Server::stop()`，按固定顺序停止 MainReactor、SubReactor、ThreadPool，全部 I/O / worker 线程 join 后进程正常退出。

本文档定义 **单一停服语义**，不区分 force / graceful，不保证停服时 in-flight 响应完整发出。

## 目标

- 进程收到 **SIGINT / SIGTERM** 后，不经 `kill -9`，走程序内停服路径并 **正常 return**。
- **main 线程** 为控制线程：注册信号、等待停服请求、调用 `Server::stop()`。
- **MainReactor** 与 **SubReactor** 并列：各自独立 I/O 线程，各自 `EventLoop::loop()`；`stop()` 经 `enqueueTask` + `eventfd` 唤醒阻塞在 `epoll_wait` 的 loop。
- 停服顺序确定、可写进验收：停 accept → 关连接 → 停 worker → join 完毕。

## 模块划分

```text
main.cpp           // 信号注册、等 g_stop_requested、显式 Server::stop()
Server             // start/stop 编排；Main → Sub(s) → ThreadPool
MainReactor        // eloopThread + Acceptor；listenPort 经 enqueueTask
SubReactor         // eloopThread + Connections；stop 时对连接调 Connection::stop()
Connection::stop() // 停服专用：dead + Channel::remove + cancel idle
Connection::handleDead()  // 运行时收束：dead + remove + cancel idle + 延迟 remove from map
Channel::remove()  // EPOLL_CTL_DEL；供 stop / handleDead 共用
```

| 模块 | 运行时 | 停服 |
|------|--------|------|
| `main` | `start()` 后立即返回；轮询停服标志 | `Server::stop()` |
| `MainReactor` | `eloopThread` 跑 loop；`listenPort` 投递 task 建 Acceptor | `enqueueTask` 清 `Acceptors` → `stopLoop` → `join` |
| `SubReactor` | `eloopThread` 跑 loop；维护 `Connections` | `enqueueTask` 对各连接 `stop()` → `stopLoop` → `join` |
| `Connection` | `handleDead` 延迟 remove | `stop()` 同步摘 channel、cancel timer、置 `dead` |
| `ThreadPool` | worker 处理 HTTP 快照 | `stop_` + `join` workers |

## 线程模型（定稿）

```text
main 线程           控制：signal → wait → Server::stop() → return
MainReactor 线程    主 EventLoop：Acceptor / listen
SubReactor × N      各一条 EventLoop：Connection I/O
ThreadPool × N      worker 业务线程
```

改前（v8）：`main` 阻塞在 `MainReactor::start()` → `eloop->loop()`，无法 concurrently 等信号；Ctrl+C 直接杀进程。  
改后（v9）：`MainReactor::start()` 与 `SubReactor::start()` 对称，在独立 I/O 线程跑 loop；**main 不跑 epoll**，新连接处理 **不唤醒** main。

## 当前架构流程

### 运行时（与 v8 一致）

```text
Client → MainReactor(Acceptor) → Server round-robin → SubReactor::addConnection
  → Connection I/O → HttpProcess → ThreadPool → enqueueTask 回 owner → trySendToSck
  → EOF/idle/写错误 → handleDead → enqueueTask(remove from map)
```

### 停服

```text
SIGINT / SIGTERM
  → main: g_stop_requested = true
  → Server::stop()
       MainReactor::stop()     // enqueueTask: Acceptors.clear → stopLoop → join
       SubReactor::stop() × N  // enqueueTask: Connection::stop() × N → stopLoop → join
       ThreadPool::stop()      // join workers
  → main: return 0
```

**顺序理由**：先停 accept；Sub 仍在跑时 worker 可回投 `enqueueTask`（loop 已 `stop` 时回投 no-op）；ThreadPool 最后 join。

## 停服语义（定稿）

- **触发**：SIGINT、SIGTERM（Ctrl+C 即 SIGINT）。
- **不保证**：停服时 in-flight HTTP 响应一定写完；worker 尚未回投的任务可能因 loop 已停而丢弃回投。
- **保证**：不再 accept；各 EventLoop 与 worker 线程 join；进程正常路径下无 hang。
- **不引入**：graceful / force 两档；shutdown 超时 Timer；独立 `SignalHandler` 类。
- **调用约定**：`main` **必须** 显式 `Server::stop()`；`~Server()` **不** 兜底（见 Q11）。

## 连接收束：`stop()` 与 `handleDead()` 分工

| | `Connection::stop()` | `Connection::handleDead()` |
|---|----------------------|----------------------------|
| **调用场景** | 仅 `SubReactor::stop()` task | EOF 收束、写错误、idle 到期 |
| **state** | `dead` | `dead` |
| **Channel** | `remove()` | `remove()` |
| **idle Timer** | `deleteTimerCallBack` | `deleteTimerCallBack` |
| **从 map 移除** | 否（`~SubReactor` 析构释放 `shared_ptr`） | `enqueueTask(remove)` 延迟 erase |
| **worker 守卫** | 回投见 `dead` 即 return | 同左 |

**不等待** `writeBuffer` drain、`working` 归零。

## 与 v6 / v8 的关系

- **v6** `ConnState` / `working`：正常运行期半关闭收束不变；停服走 `stop()` 专用路径，不扩展 `peerClose` 状态机。
- **v8** `TimerQueue`：idle 仍按 Channel 激活 refresh；`stop()` 与 `handleDead()` 均 cancel idle timer。
- v8 文档「为后续超时强关预留 addTimer」：**v9 不启用**；若将来加 drain / 超时，另开设计条目。

## Q & A

1. 为什么需要 main 作控制线程，而不是让 MainReactor 自己捕信号？

- 信号处理器 **不能** 安全调用 `Server::stop()`（锁、join、堆分配均非 async-signal-safe）。
- handler 只设 **原子标志**；**主线程** 在安全上下文调 `stop()`。
- `main` 若阻塞在 `start()` 的 loop 里，无法等信号并编排 Sub / ThreadPool；故 MainReactor 与 Sub 一样放到独立 I/O 线程。

-----
2. MainReactor 的 `stop()` 为什么要 `enqueueTask`，不能直接 `stopLoop()`？

- `loop()` 阻塞在 `epoll_wait(-1)`；跨线程只写 `stop=true` **不会** 唤醒 wait。
- 与 SubReactor 相同：`enqueueTask` → 写 **eventfd** → loop 返回 → 执行 task → `stopLoop()`。
- 若仅 `stopLoop()` 且不唤醒，停服路径会 hang（原 OPEN-002）。

-----
3. `Server::start()` / `stop()` 语义？

- **`start()`**：启动各 SubReactor I/O 线程 → 启动 MainReactor I/O 线程 → **立即返回**。
- **`stop()`**：唯一停服入口（原 `stop_half_force` 公开化）；顺序 Main → Sub → ThreadPool。
- **`~Server()` 不调用 `stop()`**；由 `main` 保证显式停服（见 Q11）。

-----
4. 信号在哪里注册？要不要单独的 Signal 类？

- **仅在 `main.cpp`**：`std::signal(SIGINT/SIGTERM, …)`；静态 `std::atomic<bool> g_stop_requested`。
- **不需要** 独立信号处理类；停服编排属于 `Server`。
- 控制线程用短 `sleep` 轮询标志即可；本阶段不必 `signalfd`。

-----
5. 停服时单个 `Connection` 如何收束？

- **`Connection::stop()`**（SubReactor 停服 task、owner I/O 线程）：
  - 若已 `dead`，return；
  - `state = dead`；`channel->remove()`；`deleteTimerCallBack(timerId)`。
- **`SubReactor::stop()`** 只对 map 中连接调 `stop()`，**不** 调 `handleDead()`。
- map 条目随 **`~SubReactor`** 析构释放 `shared_ptr`；不在停服 task 内 `erase`。

-----
6. `MainReactor::stop()` 里 Acceptor 如何处理？

- 在 **loop 线程** 的 stop task 内：`Acceptors.clear()`（`unique_ptr` 析构 → `Acceptor` / listen fd 关闭）。
- 再 `eloop->stopLoop()`；控制线程 `join` MainReactor 线程。
- `listenPort` 亦经 `enqueueTask` 注册，保证 Acceptor 操作均在 owner loop 线程。

-----
7. ThreadPool 为何放在 SubReactor 之后 stop？

- worker 完成后 **enqueueTask** 回 Sub 的 EventLoop；Sub 仍运行时回投可执行；loop 已 `stop` 时 `enqueueTask` 直接 return。
- ThreadPool 最后 `join`；停服路径不应再向 pool 提交新业务。

-----
8. 是否区分 graceful shutdown 与 force shutdown？

- **不区分**；仅一套语义（见「停服语义」）。
- 验收：进程正常退出、线程 join、无 accept 泄漏；不验收响应是否发完。

-----
9. 验收方式？

```bash
./build/server          # 或 scripts 启动
curl http://127.0.0.1:8888/
kill -SIGTERM $(pgrep -x server)   # 或终端 Ctrl+C
# 期望：打印停服日志，进程退出，wait 返回 0
```

- 可选：并发 `curl` 过程中发 SIGTERM，确认无崩溃。

-----
10. 新连接会唤醒 main 控制线程吗？

- **不会**。accept 与 `Server::handleNewConnection` 在 **MainReactor I/O 线程** 执行；main 仅在收到 SIGINT/SIGTERM 后被 `g_stop_requested` 唤醒并调 `stop()`。

-----
11. 为何 `~Server()` 不调用 `stop()`？

- **调用约定**：`main` 创建 `Server`；退出前 **必须** `s.stop()`。
- **不做析构兜底**：避免双重 join、掩盖忘记 `stop()` 的用法错误。
- 未显式 `stop()` 即析构属于违反约定，不在 v9 防护范围。

-----
12. 与缺陷跟踪的对应关系？

- 原 **OPEN-001 / OPEN-002** 已由本版实现关闭，归档 **FIX-033**（见 `issue_log/fixed_issues.md`）。
