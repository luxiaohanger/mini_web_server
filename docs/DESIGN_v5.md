# 主从多 Reactor 架构设计

在这一版本，项目从单 Reactor + 线程池，演进到主从多 Reactor 架构。

总体上看，主线程只负责监听 fd 和 accept 新连接；多个 I/O 子线程各自运行一个 `EventLoop`，负责已建立连接的读写、`Channel` 更新和连接生命周期；业务线程池继续负责业务逻辑，不直接操作 socket、`Channel`、`Epoll`。

其中实现的重点有：

- 主 Reactor：持有 `Acceptor`，只负责监听端口和接收新连接

- Sub Reactor：每个子 Reactor 持有一个固定线程和一个 `EventLoop`，负责本线程内连接集合

- 新连接分发：`Server` round-robin 选择一个 `SubReactor`，把创建连接任务投递到对应 `EventLoop`

- 连接归属：一个 `Connection` 只属于一个 `SubReactor`，读写、`Channel` 更新和连接移除都在 owner loop 中完成

- 生命周期管理：`Connection` 使用 `std::shared_ptr` 管理，异步任务捕获 `shared_from_this()` 得到的 `self`，避免 worker 访问悬空对象

- 业务线程边界：owner loop 从 `readBuffer` 取出 `std::string` 数据快照，worker 只处理快照并生成响应；响应写回 `writeBuffer` 的动作回投 owner loop 执行

- EOF 语义：`read == 0` 只表示对端关闭写方向，不表示本端不能继续写；EOF 前已读数据会继续处理并尝试发送响应

- 当前 shutdown：采用 force shutdown 语义，不保证所有 worker 回投、善后函数或响应完整发送，只保证资源释放顺序尽量安全

## 当前架构流程

```text
Client
  |
  v
MainReactor / Acceptor
  |
  | accept Socket*
  v
Server round-robin
  |
  | addConnection 投递到某个 SubReactor
  v
SubReactor EventLoop
  |
  | make_shared<Connection>
  v
Connection / Channel
  |
  | read socket -> readBuffer
  | dataOut -> std::string 快照
  v
ThreadPool worker
  |
  | 业务处理生成 response
  v
owner EventLoop task queue
  |
  | response -> writeBuffer
  v
trySendToSck / EPOLLOUT
```

## 当前类职责

### `Server`

- 持有 `MainReactor`、多个 `SubReactor` 和 `ThreadPool`
- 负责给 `MainReactor` 注入新连接回调
- 负责 round-robin 选择 sub reactor
- 当前析构中按 force shutdown 语义停止 main/sub，再释放线程池和 reactor 资源

### `MainReactor`

- 持有主线程 `EventLoop`
- 持有一个或多个 `Acceptor`
- 只负责监听 fd 和 accept
- 不管理普通连接

### `SubReactor`

- 持有子线程和子线程内的 `EventLoop`
- 持有 `Socket* -> shared_ptr<Connection>` 的连接表
- 新连接创建、连接移除、连接清理都通过自己的 `EventLoop` 执行
- 当前 `stop()` 会投递任务到 owner loop：停止连接、清空连接表、停止 loop

### `Connection`

- 管理单连接的 `Socket`、`Channel`、`readBuffer`、`writeBuffer`
- 继承 `enable_shared_from_this`
- worker 任务和回投任务捕获 `self`，避免异步 use-after-free
- worker 不直接访问内部 buffer，只处理从 owner loop 取出的数据快照
- 响应写回和 socket 写入仍在 owner loop 线程执行

## Q & A

1. 为什么 I/O 线程不从普通线程池派发？

- 普通线程池适合执行短任务：取出任务、执行任务、结束后继续取下一个任务；业务处理函数适合这种模型

- I/O 线程执行的是长期 `EventLoop` 循环：`epoll_wait`、处理活跃 `Channel`、执行 pending tasks；这个循环通常不会退出

- 如果把 `EventLoop::loop()` 投递给普通 `ThreadPool`，这个任务会长期占住一个 worker 线程，本质上还是固定 I/O 线程，并没有发挥普通线程池的调度意义

- 多 Reactor 的关键是线程归属稳定：一个 `EventLoop` 固定在一个线程中运行，一个 `Connection` 固定归属于一个 `EventLoop`

- 如果 I/O 线程被普通线程池随意调度，同一个连接的读、写、关闭可能落到不同线程，会破坏 `Channel`、`Epoll`、`Connection` 的线程安全边界

- 因此 I/O 线程也可以池化，但应该是专门的 `EventLoopThreadPool`；当前阶段先由 `Server` 直接管理 `SubReactor` 数组，等结构稳定后再抽象线程池类

-----

2. 为什么 `Connection` 使用 `shared_ptr`？

- 多线程阶段中，worker 任务和回投任务的生命周期可能长于当前 I/O 回调调用栈

- 如果 worker 捕获裸 `this`，连接被删除后会访问悬空对象

- 当前让 `Connection` 继承 `enable_shared_from_this`，任务捕获 `self`，保证任务执行期间对象不会被提前析构

- `shared_ptr` 只保证对象活着，不保证线程安全；因此 socket I/O 和 `writeBuffer` 写入仍然回到 owner loop 执行

-----

3. 为什么 worker 只处理数据快照？

- `readBuffer` 和 `writeBuffer` 属于 `Connection`，也就是属于 owner loop 线程

- 如果 worker 直接操作内部 buffer，会和 owner loop 的读写 I/O 产生数据竞争

- 当前 owner loop 先把 `readBuffer` 可读数据取出为 `std::string` 快照，再把快照投递给 worker

- worker 生成响应字符串后，回投到 owner loop，由 owner loop 写入 `writeBuffer` 并尝试发送

-----

4. 当前 shutdown 是什么语义？

- 当前采用 force shutdown，不是 graceful shutdown

- force shutdown 不保证所有 worker 回投都执行，不保证所有响应都完整写出，也不保证所有 `EPOLLOUT` 后续事件都 drain 完成

- 当前目标是保证资源释放顺序尽量安全：停止接收新连接，通知 sub reactor 停止，清理连接表，停止 loop，回收线程和资源

- 如果后续要实现 graceful shutdown，需要增加连接状态机、输出缓冲区 drain、超时强制关闭、自定义 deleter 或 owner loop 延迟销毁机制

## 当前保留问题

- 当前 force shutdown 不保证 `Connection` 最后一定在 owner loop 线程析构；严格工业语义需要自定义 deleter 或延迟销毁队列

- 当前 shutdown 不保证响应完整发送；后续可以设计 graceful shutdown
