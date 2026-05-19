# 主从多 Reactor + 智能指针所有权 架构设计

在这一版本，项目在 v5「主从多 Reactor + `shared_ptr` 连接生命周期」的基础上，完成 **服务器核心组件的所有权重构**，并引入 **`ConnState` 状态机** 与 **`working` 计数**，统一 TCP 半关闭下的「何时从 `SubReactor` 移除连接」。

总体上看，v5 已解决的问题（数据快照、worker 不碰 buffer、半关闭仍可写）在 v6 中用 **明确状态 + 延迟 remove** 落地；v5 中「EOF 后靠临时 `self` 撑住、却仍可能过早 `erase`」的路径被收束。

其中实现的重点有：

- **所有权**：`Server` / `MainReactor` / `SubReactor` / `EventLoop` / `Acceptor` / `Connection` 成员以 `std::unique_ptr` 持有；连接表为 `Socket* -> std::shared_ptr<Connection>`；业务路径不再手写 `delete` 主要组件

- **连接状态**：`enum class ConnState { connected, peerClose, dead }`；读到 `read == 0` 仅进入 `peerClose`，**不**立刻 `remove`；仅在 `dead` 时从 `SubReactor` 移除

- **异步收束**：`working` 计数；`process` 前 `++`，owner 线程善后任务内 `--` 后再 `dataIn` / `trySendToSck`；**不能**仅凭「写缓冲空 + peerClose」判定 `dead`，须 `working == 0`

- **accept 移交**：`Socket::acceptConnection()` 仍 `new` 客户端 `Socket`，在 `SubReactor` 内立即包成 `unique_ptr` 并入 `make_shared<Connection>`（工厂 `new` → 独占移交）

- **非拥有指针**：`EventLoop*`、`Channel*`（epoll `data.ptr`）、`Socket*`（map 键与回调参数）保留，符合 Reactor 惯例

- **运行模型**：`main` 在 `start()` 中常驻，**未设计程序内退出**；`stop_half_force()` 为将来停服/析构预留骨架（见 Q4）

## 当前架构流程

```text
Client
  |
  v
MainReactor / Acceptor
  |
  | accept -> Socket* (new，随即移交)
  v
Server round-robin
  |
  | addConnection -> SubReactor::enqueueTask
  v
SubReactor EventLoop
  |
  | make_shared<Connection>(unique_ptr<Socket>)
  v
Connection / Channel
  |
  | EPOLLIN -> readFromSck
  |   read==0 -> peerClose (仍留在 Connections)
  |   有数据 -> readBuffer -> string 快照
  v
ThreadPool (working++)
  |
  | processEcho -> enqueueTask 回 owner
  v
owner EventLoop (working--, dataIn, trySendToSck)
  |
  | 写排空 && peerClose && working==0 -> dead -> remove
  v
map erase -> Connection 析构
```

## 连接状态机

```text
connected ──read==0──> peerClose ──写排空且 working==0──> dead ──handleDead──> 从 map 移除
     |                      |
     |                      └── 写致命错误 ──> dead
     └── 写致命错误 ───────────────────────> dead
```

**纯 EOF、无应用数据**：`n == 0` 且 `peerClose && working == 0 && writeBuffer 空` 时，在 `Echo` 内直接进入 `dead`。

**有数据再 EOF**（同轮 `readFromSck`）：`n > 0` 走 `working++` → worker → 写回；写完成后再按上表收束。

## 当前类职责

### `Server`

- `unique_ptr` 持有 `ThreadPool`、`MainReactor`、`vector<unique_ptr<SubReactor>>`
- 新连接 round-robin 分发到 `SubReactor`
- `~Server()` 调用 `stop_half_force()`（当前 `main` 常驻时通常走不到完整停服路径）

### `MainReactor` / `SubReactor`

- 与 v5 相同：主 reactor 只 accept；子 reactor 一线程一 `EventLoop`
- `Connections`：`unordered_map<Socket*, shared_ptr<Connection>>`；`remove` 仅在 `Connection` 进入 `dead` 后由回调触发

### `Connection`

- 成员：`unique_ptr<Socket|Channel|Buffer>`；`enable_shared_from_this`
- `ConnState state`；`int working`
- `handleDead()`：设置 `dead` 后 `removeConnectionCallBack`（不再在 EOF 时调用）

### `EventLoop`

- `unique_ptr<Epoll>`、`unique_ptr<Channel>`（eventfd）
- 析构顺序：`eloopChannel.reset()` → `ep.reset()` → `close(eloopFd)`（对齐 FIX-013 / FIX-025）

## 与 v5 的差异摘要

| 主题 | v5 | v6 |
|------|----|----|
| 组件释放 | 部分路径仍涉及手工资源语义 | 核心树 `unique_ptr` / 连接 `shared_ptr` |
| EOF | 从表移除与 `self` 延长并存，易与写路径竞态 | `peerClose` 留在 map，写排空 + `working==0` 再 `dead` |
| 半关闭收束 | 文档级语义 | `ConnState` + `working` 代码落地 |
| 程序退出 | force shutdown 描述 | 明确为常驻模型；停服为暂缓设计 |
| accept | `Socket*` 传递 | 仍为 `new`，在 `SubReactor` 转 `unique_ptr` |

## Q & A

1. 为什么说「用智能指针重构了服务器核心」，而不是「整个项目零裸指针」？

- **已重构**：`Server` 子系统树、`Connection` 及其成员、连接表 `shared_ptr`；业务层无配对 `delete Connection/Acceptor/EventLoop`。
- **仍保留裸指针**：epoll 返回的 `Channel*`、`EventLoop*` 借用、`Socket*` 作 map 键与 accept 回调参数；`Channel` 回调 `[this]`（`dead` 前由 map 的 `shared_ptr` 保活）。
- **一处 `new`**：`acceptConnection` 创建客户端 `Socket`，所有权在 `make_shared<Connection>` 前交给 `unique_ptr`。

2. 为什么 EOF 不能立刻 `removeConnection`？

- `read == 0` 只表示对端关写；本端可能仍有 `writeBuffer` 未发完，或 worker 尚未回投响应。
- 过早 `erase` 会导致 `Connections` 中无 `shared_ptr`，`EPOLLOUT` / 善后任务可能 UAF（v5 阶段的 OPEN-006 类问题）。
- v6 在 `peerClose` 阶段保持 map 条目，直至 `dead`。

3. 为什么需要 `working`，而不能只看写缓冲是否为空？

- `Echo` 投递 `process` 后，I/O 线程上 `writeBuffer` 可能仍空，但 worker 尚未 `enqueueTask` → `dataIn`。
- 此时 `peerClose && writeBuffer 空` **不能** 判定 `dead`。
- `working++/--` 标记「已投递且尚未完成 owner 善后」的异步区间。

4. 当前如何退出进程？

- **设计选择**：启动后 `main` 阻塞在 `MainReactor::start()`，终端常驻，靠 Ctrl+C / kill 结束；**未实现** Ctrl+C 优雅退出或命令行 `quit`。
- `stop_half_force()`、`SubReactor::stop()`（`enqueueTask` + `join`）为将来「可停服」预留；与运行中 echo 稳定性（读错误、EINTR）分开演进。

5. 与 v5 相同的线程边界是否不变？

- 不变：worker 只处理 `std::string` 快照；`writeBuffer` 与 socket 写仅在 owner `EventLoop` 线程执行。
- `shared_ptr` / `working` 只解决**生命周期**与**收束时机**，不替代线程安全边界。

## 当前保留问题

- **运行中**：`readFromSck` 对非 `EINTR`/`EAGAIN` 的读错误未跳出循环（OPEN-003）；`epoll_wait` 遇 `EINTR` 直接 `exit`（OPEN-004）。

- **暂缓**：程序内停服与 graceful/force 边界（原 OPEN-001/002）；待产品需要「可退出」时再实现主 loop 唤醒与公开 `stop` API。

- **可选演进**：`accept` 改为 `make_unique` 工厂；`peerClose` 时 `disableReading`；Channel 回调 `weak_ptr`（非当前必做）。
