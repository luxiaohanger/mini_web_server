# 已修复问题


## FIX-001 `ThreadPool::stop` 未初始化

- 修复日期：2026-05-18
- 状态：Fixed
- 位置：`include/ThreadPool.h`、`src/server/ThreadPool.cpp`
- 影响：worker 线程可能读取未定义值，导致线程池随机退出、卡住或行为不稳定。
- 根因：`stop` 是普通成员变量，构造函数未显式初始化。
- 修复：构造函数初始化为 `stop(false)`；线程池文件名对齐为 `ThreadPool.cpp`。
- 验证建议：多次启动 server/client，确认任务稳定执行。

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
- 修复：先销毁 `eloopChannel`，再销毁 `ep`，最后 `close(eloopFd)`（早期为 `delete`；现见 FIX-025 `reset` 写法）。
- 备注：智能指针重构后曾再次出现错误顺序（OPEN-005），已由 FIX-025 纠正。

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

## FIX-020 附记：EOF 与 `handleDead` / `disableAll`（随 FIX-023 更新）

- 说明日期：2026-05-19
- 结论（FIX-023 后）：读 EOF 只进入 **`peerClose`**，不 `remove`、不 `disableAll()`；**`dead`** 时才 `handleDead()` → `remove`；`stop()` 仍 `disableAll()`。
- EOF 后无新可读数据；写排空与 `EPOLLOUT` 在 `peerClose` 阶段完成后再转入 `dead`（见 OPEN-007）。

## FIX-021 `Connection` 构造函数字形参遮蔽成员 `sck`

- 修复日期：2026-05-19（`229739a`）
- 状态：Fixed
- 位置：`src/server/Connection.cpp` 构造函数
- 现象：`make_shared<Connection>` / `addConnection` 时 SIGSEGV；`Socket::getFd(this=0x0)`。
- 根因：形参与成员同名 `sck`，初始化列表 `sck(std::move(sck))` 后形参已空；体内 `sck->getFd()` 命中**形参**而非成员。`errif(this->sck.get()==nullptr)` 只检查成员，故不触发。
- 修复：形参改名为 `sock`（与成员 `sck` 区分），`sck(std::move(sock))` 后体内 `sck->getFd()` 解析为成员（约第 18 行）。
- 验证：`bt` 栈顶为 `Connection::Connection` 构造；单 client 短消息 echo 不崩溃。

## FIX-022 析构路径半强制 `stop_half_force`

- 修复日期：2026-05-19（`229739a`）
- 状态：Fixed（部分语义，完整 graceful 仍见 OPEN-001）
- 位置：`Server`、`MainReactor`、`SubReactor`、`ThreadPool`
- 现象：`~Server()` 为空；`ThreadPool` 析构不 `join` worker；进程退出时资源与线程未收束。
- 修复：`Server::~Server()` 调用 `stop_half_force()`：依次 `mainReactor->stop()`、各 `SubReactor::stop()`（含 `join` I/O 线程）、`threadpool->stop()`（`join` worker）。`ThreadPool` 析构体改为空，依赖先 `stop()`。
- 备注：属 force/half-force **骨架**，供将来停服/析构用；当前 `main` 在 `start()` 常驻，用户通常杀进程退出，不走完整停服路径（见 OPEN-001/002 暂缓）。

## FIX-023 `ConnState`：EOF 仅 `peerClose`，写错误才 `remove`

- 修复日期：2026-05-19（本地同步，待服务器提交）
- 状态：Fixed（与 FIX-024 一并关闭原 **OPEN-006**）
- 位置：`include/Connection.h`、`src/server/Connection.cpp`
- 现象（修复前）：读 EOF 即 `handleDead()` → `Connections.erase`，写未完成时 `Connection`/`Channel` 可能先析构（OPEN-006）。
- 修复：
  - `enum class ConnState { connected, peerClose, dead }`；构造默认 `connected`。
  - `readFromSck()` 中 `read==0` 只设 `state = peerClose`，**不**调用 `removeConnectionCallBack`。
  - 写致命错误：`state = dead` 后 `handleDead()` → 从 `SubReactor` 移除。
- 与 FIX-020：保留「EOF 后仍可写」；表项在 `peerClose` 阶段仍由 map 持有 `shared_ptr`，**写未完成前不析构**，消除「`EPOLLOUT` 时 `Channel`/`Connection` 已释放」主因（OPEN-006）。
- 备注：FIX-020 附记中「EOF 时 `handleDead` 只 remove」的描述已过时；现改为 **仅 `dead` 时 remove**。

## FIX-024 `working` 计数与 `peerClose` 收束

- 修复日期：2026-05-19（本地同步，待服务器提交）
- 状态：Fixed
- 位置：`include/Connection.h`、`src/server/Connection.cpp`
- 修复：
  - 成员 `int working`：投递 `process` 前 `working++`；owner 线程善后 `enqueueTask` 内 **`working--` 后** `dataIn` + `trySendToSck`。
  - 纯 EOF 无数据：`Echo` 中 `n==0 && peerClose && working==0 && writeBuffer 空` → `dead` + `handleDead()`。
  - 写排空：`trySendToSck` / `handleWriteCallBack` 在 `disableWriting` 后若 `peerClose && working==0` → `dead` + `handleDead()`。
- 语义：`peerClose && working==0` 表示不会再有新 worker，且已登记的 worker 善后已执行到 `--`（写回任务已进入或已完成该次 `trySendToSck` 调用链）。
- 验证建议：client 发消息后退出；纯连接关闭无数据；大消息触发 `EPOLLOUT` 后再关 client。
- 说明：Channel 回调仍为 `[this]`；在 `dead` 之前由 `Connections` 的 `shared_ptr` 保活即可，**不另记 OPEN 残留**。`weak_ptr` 仅为可选风格加固，非必做。

## FIX-025 `EventLoop` 析构顺序（对齐 FIX-013）

- 修复日期：2026-05-19（本地同步，待服务器提交）
- 状态：Fixed（原 OPEN-005）
- 位置：`src/server/EventLoop.cpp` `~EventLoop()`（约 19–23 行）
- 现象：析构体先 `close(eloopFd)` 再析构成员，`~Channel` 对已关闭 fd 做 `epoll_ctl DEL`。
- 修复：显式 `eloopChannel.reset(); ep.reset(); ::close(eloopFd);`——先拆 channel（`removeChannel`），再拆 epoll，最后关 eventfd。

## FIX-026 `handleDead` 延迟 `remove`（对齐 muduo `runInLoop`）

- 修复日期：2026-05-20
- 状态：Fixed（关闭 **OPEN-005**，2026-05-20 复核的「回调栈内 erase / 延后析构」条目；与 **FIX-025** 的「原 OPEN-005」为不同问题，FIX-025 指 `EventLoop` 析构顺序）
- 位置：`src/server/Connection.cpp` `handleDead()`（约 181–189 行）
- 现象：在 `Channel::handle()` → `readCallBack` / `writeCallBack` 栈内同步 `Connections.erase`，map 唯一 `shared_ptr` 释放后，`onHttp` 末尾 `self` 或写回调路径可能在 `Channel::handle` 未返回时触发 `~Connection` / `~Channel`，存在对象生存期 UB（test3 短连接收束、test4 同步 400 等路径）。
- 修复：
  - `handleDead()`：`shared_from_this()` 续命；`state = dead`；`channel->disableAll()`；
  - `removeConnectionCallBack` 放入 `eloop->enqueueTask`，在本轮 `poll` 全部 `channel->handle()` 结束后再 `erase`（与 `EventLoop::loop` 先 poll 后 tasks 一致）。
- 验证：HTTP client 全套用例（含 `Connection: close`、POST→400）；逻辑上 `Channel::handle` 返回前不再 `erase` map。
- 可选后续（未做）：`handleDead` 幂等 / `onHttp` 入口 `state==dead` 早退，防重复入队。

