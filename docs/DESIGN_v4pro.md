# 单 Reactor 多线程阶段复盘与演进方向

## 版本定位

当前项目处于 **单 Reactor + 线程池** 阶段。主线程负责 `epoll_wait`、`eventfd` 唤醒、非阻塞 socket I/O 和 `Channel` 更新；任务线程负责业务处理，不直接操作 fd。

本阶段的核心价值是完成了从“能处理连接”到“具备并发业务处理能力”的升级，同时暴露出高性能网络库必须面对的关键问题：非阻塞 I/O 的部分读写、跨线程任务通知、连接生命周期、资源释放顺序。

## 当前架构闭环

```text
client fd
   |
   v
Channel
   |
   v
EventLoop + Epoll  <------ eventfd wakeup
   |
   v
Connection
   |
   +--> readBuffer  --业务任务-->  ThreadPool
                                  |
                                  v
                              writeBuffer
                                  |
                                  v
EventLoop 善后队列 <--------------+
   |
   v
trySendToSck / EPOLLOUT
```

## 本轮架构 bug 修复总结

### 1. 修复线程池停止标志未初始化

`ThreadPool::stop` 原本没有初始化，工作线程启动后可能读取未定义值。现在构造函数通过初始化列表设置为 `false`，保证线程池启动状态确定。

这个问题属于典型的并发基础设施 bug：表现可能不稳定，甚至与业务逻辑无关，因此应该优先修复。

### 2. 修复非阻塞写的 buffer 游标推进

`Buffer::bufferToSck()` 原先按照请求写入的 `count` 推进 `readIdx`，但非阻塞 `write` 可能只写出部分数据，也可能返回 `EAGAIN` 或 `EINTR`。

现在只在 `res > 0` 时按照实际写出字节数推进读指针，避免部分写时丢失数据，并让 `EPOLLOUT` 兜底机制真正有效。

### 3. 显式删除 epoll 关注事件

`Channel` 析构时通过 `EventLoop -> Epoll` 调用 `EPOLL_CTL_DEL`，不再单纯依赖 `close(fd)` 的内核副作用清理 epoll 关注。

这让资源释放更显式，也让 `Channel` 和 `Epoll` 的关系更接近成熟 Reactor 网络库的设计方式。

### 4. 修复 `Epoll` fd 释放细节

`Epoll::~Epoll()` 现在显式关闭 `epfd`，并使用 `if (epfd >= 0)` 判断合法 fd，避免 `fd == 0` 被误判，也避免 `-1` 被当作 true。

`EPOLL_CTL_DEL` 的 event 参数也调整为 `nullptr`，符合删除事件时的常见写法。

### 5. 修复 `accept` 可恢复错误处理

监听 fd 是非阻塞的，`accept` 可能返回 `EINTR`、`EAGAIN` 或 `EWOULDBLOCK`。现在：

- `EINTR` 会重试。
- `EAGAIN` / `EWOULDBLOCK` 返回 `nullptr`，由 `Acceptor` 忽略。
- 新连接 fd 使用 `>= 0` 判断，兼容合法 fd `0`。
- 致命错误仍通过 `errif` 退出。

### 6. 修复当前调用栈内的 use-after-free

早期 `readFromSck()` 在读到 EOF 时直接调用删除回调，导致 `Connection` 被释放后，调用栈返回到 `Echo()` 仍可能访问成员。

现在 `readFromSck()` 只修改 `alive` 状态，`Echo()`、`trySendToSck()` 和 `handleWriteCallBack()` 在安全位置统一触发删除回调并立即返回，避免当前调用栈继续访问已释放对象。

## 仍需关注的问题

### 1. 异步任务捕获裸 `this`

当前任务线程通过 `[this]` 访问 `Connection` 内部成员。如果任务已经投递，但连接在任务执行前或执行中被主线程删除，任务线程仍可能访问悬空对象。

这个问题和 fd 无关，即使任务线程不做 socket I/O，只要访问 `readBuffer`、`writeBuffer` 或 `eloop`，本质上仍依赖 `Connection` 的对象生命周期。

### 2. EOF 前已读数据的处理语义

如果一次读循环中先读到数据，随后读到 EOF，当前逻辑可能直接关闭连接，导致 EOF 前已进入 `readBuffer` 的数据没有被业务处理。

这个问题属于更严格的 TCP 语义处理。当前如果只验证简单 echo，可以暂缓；后续做协议解析时需要重新设计读状态。

### 3. 完整停止流程

虽然当前析构顺序已经避免 `Channel` 删除时访问已释放的 `EventLoop`，但完整的 server shutdown 仍未设计，包括：

- 如何停止接收新连接。
- 如何处理已投递但未执行的任务。
- 如何等待或取消连接上的业务任务。
- 如何安全销毁 `Connection`、`EventLoop`、`ThreadPool`。

## 下一阶段：主从多 Reactor

下一阶段建议升级为 **Main-Sub Reactor** 架构：

```text
                 +----------------+
                 |  Main Reactor  |
                 | accept listen  |
                 +--------+-------+
                          |
             dispatch new connection
                          |
        +-----------------+-----------------+
        |                 |                 |
        v                 v                 v
+---------------+ +---------------+ +---------------+
| Sub Reactor 0 | | Sub Reactor 1 | | Sub Reactor N |
| connections   | | connections   | | connections   |
+-------+-------+ +-------+-------+ +-------+-------+
        |                 |                 |
        +-----------------+-----------------+
                          |
                          v
                    ThreadPool
                  business tasks
```

### 核心不变量

升级时建议优先建立以下规则：

- 每个 `Connection` 只属于一个 `Sub EventLoop`。
- `Connection` 的创建、销毁、`Channel` 更新都只在所属 `EventLoop` 线程执行。
- worker 线程不持有裸 `Connection*`，只处理脱离连接生命周期的业务数据。
- worker 完成后把结果投递回原 `EventLoop`。
- 原 `EventLoop` 在安全点检查连接是否仍有效，再写入 `writeBuffer` 并执行发送。

## 推荐演进路线

1. 抽象 `EventLoopThread`，让每个 sub reactor 拥有自己的线程和 `EventLoop`。
2. 抽象 `EventLoopThreadPool`，由 `Server` 负责轮询分配新连接。
3. `Acceptor` 仍由 main reactor 管理，只负责接收新连接。
4. 新连接分配给某个 sub reactor 后，由该 loop 创建并管理 `Connection`。
5. 重构业务任务模型，worker 只处理请求数据快照，不捕获裸 `this`。
6. worker 返回响应结果时，通过所属 loop 的任务队列回投。
7. 引入连接 id、generation 或 weak token，避免结果回投到已经销毁或 fd 复用后的旧连接。

