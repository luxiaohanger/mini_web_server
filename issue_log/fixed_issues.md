# 已修复问题


## FIX-001 `ThreadPool::stop` 未初始化

- 修复日期：2026-05-18
- 状态：Fixed
- 位置：`include/ThreadPool.h`、`src/server/ThreadPool.cpp`
- 影响：worker 线程可能读取未定义值，导致线程池随机退出、卡住或行为不稳定。
- 根因：`stop` 是普通成员变量，构造函数未显式初始化。
- 修复：构造函数初始化为 `stop(false)`；线程池文件名对齐为 `ThreadPool.cpp`。
- 验证建议：Linux 侧多次启动 server/client，确认任务稳定执行。

## FIX-002 `Buffer::bufferToSck()` 错误推进读指针

- 修复日期：2026-05-18
- 状态：Fixed
- 位置：`src/server/Buffer.cpp`
- 影响：非阻塞部分写、`EAGAIN` 或 `EINTR` 时可能错误丢弃待发送数据。
- 根因：原逻辑按请求写入长度推进 `readIdx`，而不是按实际写出字节数推进。
- 修复：仅在 `res > 0` 时按实际写出字节数推进读指针；错误返回不移动 buffer。
- 验证建议：用大消息和高并发连接验证 echo 数据完整性。

## FIX-003 `Server` 未释放 `threadpool`

- 修复日期：2026-05-18
- 状态：Fixed
- 位置：`src/server/Server.cpp`
- 影响：线程池资源泄漏，析构阶段 worker 线程无法正常 join。
- 根因：`Server` 构造中分配 `ThreadPool`，析构中未释放。
- 修复：`Server::~Server()` 补充 `delete threadpool`。
- 备注：完整 shutdown 顺序仍作为 open issue 跟踪。

## FIX-004 `Channel` 未显式从 epoll 摘除

- 修复日期：2026-05-18
- 状态：Fixed
- 位置：`Channel`、`EventLoop`、`Epoll`
- 影响：连接销毁依赖 `close(fd)` 的内核副作用清理 epoll 关注，释放语义不清晰。
- 根因：缺少显式 `EPOLL_CTL_DEL` 路径。
- 修复：补充 `Channel::remove()`、`EventLoop::removeChannel()`、`Epoll::removeChannel()`，`Channel` 析构时显式摘除。
- 验证建议：连接频繁创建/关闭时观察是否有 epoll 残留或异常事件。

## FIX-005 `Epoll` 析构未关闭 `epfd`

- 修复日期：2026-05-18
- 状态：Fixed
- 位置：`src/server/Epoll.cpp`
- 影响：`epoll_create1` 返回的 fd 泄漏。
- 根因：`Epoll::~Epoll()` 未释放 `epfd`。
- 修复：析构中 `close(epfd)` 并置为 `-1`；合法性判断使用 `epfd >= 0`。

## FIX-006 `Epoll::removeChannel()` 删除事件参数不清晰

- 修复日期：2026-05-18
- 状态：Fixed
- 位置：`src/server/Epoll.cpp`
- 影响：删除 epoll 事件时语义不够明确。
- 根因：`EPOLL_CTL_DEL` 传入成员 `ev`，但删除操作不需要事件结构。
- 修复：第四个参数改为 `nullptr`。

## FIX-007 `Epoll.h` 头文件依赖过重

- 修复日期：2026-05-18
- 状态：Fixed
- 位置：`include/Epoll.h`
- 影响：增加不必要编译依赖，容易放大头文件耦合。
- 根因：已有前置声明时仍包含 `Server.h`。
- 修复：移除 `Server.h` include，只保留必要前置声明和 `sys/epoll.h`。

## FIX-008 `Socket::acceptConnection()` 非阻塞错误处理不完整

- 修复日期：2026-05-18
- 状态：Fixed
- 位置：`src/server/Socket.cpp`、`src/server/Acceptor.cpp`
- 影响：非阻塞 `accept` 遇到可恢复错误时可能误判为致命错误并退出进程。
- 根因：`EINTR`、`EAGAIN`、`EWOULDBLOCK` 未按非阻塞语义区分处理。
- 修复：`EINTR` 重试；`EAGAIN` / `EWOULDBLOCK` 返回 `nullptr` 并由 `Acceptor` 忽略；新 fd 使用 `>= 0` 判断。
- 验证建议：在高并发连接建立/断开场景验证 server 不因可恢复错误退出。

## FIX-009 当前调用栈内 delete 后继续访问对象

- 修复日期：2026-05-18
- 状态：Fixed
- 位置：`src/server/Connection.cpp`
- 影响：读到 EOF 后立即删除 `Connection`，调用栈返回后可能继续访问已释放成员。
- 根因：删除回调在底层读函数中触发，当前成员函数调用栈仍未结束。
- 修复：读写错误只设置 `alive = false`；由 `Echo()` / `trySendToSck()` / `handleWriteCallBack()` 在安全位置统一处理并立即返回。
- 备注：异步任务生命周期问题已在 FIX-015 单独修复。

## FIX-010 多 Reactor 最小编译和分发问题

- 修复日期：2026-05-18
- 状态：Fixed
- 位置：`include/Server.h`、`src/server/Server.cpp`
- 影响：头文件单独编译失败；线程数为 0 时越界；连接无法均匀分配到 sub reactor。
- 根因：缺少 `MainReactor` 前置声明；未处理 `hardware_concurrency()` 返回 0；`subIdx` 未递增。
- 修复：补充 `class MainReactor;`；线程数为 0 时兜底；`handleNewConnection()` 分发后递增 `subIdx` 实现 round-robin。

## FIX-011 `deleteConnectionCallback()` 的 `sck` 有效性确认

- 确认日期：2026-05-18
- 状态：Confirmed
- 位置：`src/server/SubReactor.cpp`
- 原担忧：`Connections[sck]` 在 key 不存在时会隐式插入空值。
- 结论：删除回调由 `Connection` 发起，传入的是该连接持有的 `Socket*`；正常路径下它来自已有连接，不作为当前 bug 处理。
- 后续建议：如需增强健壮性，可改用 `find()`。

## FIX-012 `SubReactor` 清理连接时遍历并删除同一 map

- 修复日期：2026-05-18
- 状态：Fixed
- 位置：`src/server/SubReactor.cpp`
- 影响：析构清理时迭代器失效，可能导致未定义行为。
- 根因：range-for 遍历 `Connections`，同时删除回调会 `erase` 同一个 map。
- 修复：改为 `while (!Connections.empty())`，每次取 `begin()` 删除。

## FIX-013 `EventLoop` 析构顺序依赖错误

- 修复日期：2026-05-18
- 状态：Fixed
- 位置：`src/server/EventLoop.cpp`
- 影响：`Channel::~Channel()` 依赖 `EventLoop::removeChannel()`，如果先删 `Epoll` 会访问已释放资源。
- 根因：`EventLoop` 析构中先删 `ep`，后删 `eloopChannel`。
- 修复：先 `delete eloopChannel`，再 `delete ep`，最后关闭 `eloopFd`。

## FIX-014 `EventLoop::stop` 跨线程访问未同步

- 修复日期：2026-05-18
- 状态：Fixed
- 位置：`include/EventLoop.h`、`src/server/EventLoop.cpp`
- 影响：`stopLoop()`、`loop()`、`enqueueTask()` 跨线程读写普通 `bool`，存在数据竞争。
- 根因：停止标志没有原子化或统一加锁。
- 修复：`stop` 改为 `std::atomic<bool>`，并在头文件加入 `<atomic>`。

## FIX-015 `Connection` 异步任务捕获裸 `this`

- 修复日期：2026-05-18
- 状态：Fixed
- 位置：`include/Connection.h`、`src/server/Connection.cpp`、`include/SubReactor.h`、`src/server/SubReactor.cpp`
- 影响：worker 任务和回投任务访问已释放 `Connection`，导致 use-after-free。
- 根因：异步 lambda 捕获裸 `this`，连接生命周期不覆盖异步任务执行期。
- 修复：`Connection` 继承 `std::enable_shared_from_this<Connection>`；`SubReactor::Connections` 保存 `std::shared_ptr<Connection>`；创建连接使用 `std::make_shared`；删除连接改为 `Connections.erase(sck)`；异步任务捕获 `shared_from_this()` 得到的 `self`。

## FIX-016 `Connection::alive` 跨线程访问风险收敛

- 修复日期：2026-05-18
- 状态：Fixed
- 位置：`include/Connection.h`、`src/server/Connection.cpp`
- 影响：owner loop 与 worker 线程跨线程读写连接状态，存在数据竞争。
- 根因：早期 worker 线程会读取 `alive`，owner loop 线程会修改 `alive`。
- 修复：业务线程不再判断连接状态，只处理请求快照；`alive` 的读写收敛回 owner loop 线程，因此当前不再需要 `std::atomic<bool>`。
- 备注：`alive` 的状态语义仍需在 EOF 半关闭场景继续细化，见 `OPEN-001`。

## FIX-017 worker 线程直接访问 `Connection` 内部 Buffer

- 修复日期：2026-05-18
- 状态：Fixed
- 位置：`include/Connection.h`、`src/server/Connection.cpp`
- 影响：owner loop 和 worker 线程可能同时访问 `readBuffer` / `writeBuffer`，存在 buffer 数据竞争。
- 根因：业务函数 `processEcho()` 直接操作 `Connection` 内部 buffer。
- 修复：owner loop 先从 `readBuffer` 取出数据快照；worker 只处理普通 `std::string` 数据；响应写入 `writeBuffer` 的动作回投到 owner loop 执行。

## FIX-018 异步任务按引用捕获局部响应变量

- 修复日期：2026-05-18
- 状态：Fixed
- 位置：`src/server/Connection.cpp`
- 影响：`Echo()` 返回后，worker 或回投 lambda 访问已销毁的局部变量，产生悬空引用。
- 根因：异步任务生命周期超过当前函数调用栈，但捕获了栈上局部变量引用。
- 修复：worker lambda 通过移动捕获拥有请求数据；响应 `res` 在 worker 内部生成，并通过移动捕获交给 owner loop 回投任务。

## FIX-019 `Buffer::dataOut` 写入未扩容的 `std::string`

- 修复日期：2026-05-18
- 状态：Fixed
- 位置：`src/server/Connection.cpp`
- 影响：向空 `std::string::data()` 写入 buffer 数据会越界写，导致未定义行为。
- 根因：`std::string::data()` 不会按外部写入长度自动扩容。
- 修复：读取前先保存可读长度并 `resize()`，再调用 `readBuffer->dataOut(data.data(), n)`；写回时使用 `res.data()` 和 `res.size()`。

## FIX-020 EOF 前已读数据处理语义

- 修复日期：2026-05-18
- 状态：Fixed
- 位置：`include/Connection.h`、`src/server/Connection.cpp`
- 影响：同一轮读循环中先读到数据、随后读到 EOF 时，已读数据可能未处理或响应无法写回。
- 根因：早期用 `alive=false` 同时表达“对端读方向 EOF”和“本端不可继续写”，混淆了 TCP 半关闭语义。
- 修复：移除 `alive` 状态门控；读到 EOF 时从连接表移除连接，但由 `shared_ptr self` 延长 `Connection` 生命周期，保证 EOF 前已读数据继续进入业务处理并尝试写回；写失败时再按错误路径处理。
- 备注：该实现选择严格 TCP 语义：`read == 0` 表示对端不再发送，不代表本端不能继续发送。


