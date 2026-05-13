# Reactor 架构高性能 Linux 服务器

## 1. 核心架构：Reactor 模式

本项目实现了一个基于事件驱动的 Reactor 架构。通过将 **I/O 多路复用 (Epoll)** 与 **非阻塞 I/O** 结合，实现了单线程内处理成千上万个并发连接的能力。

### 核心设计原则

* **资源所有权 (RAII)**：每个系统资源（如文件描述符 FD）都由一个 C++ 对象的生命周期严格管理。
* **事件驱动**：通过 `Channel` 与回调函数（Callback）机制，将网络底层的 FD 读写事件映射到业务层的逻辑函数中。
* **解耦**：底层 `EventLoop/Epoll` 仅负责分发信号，不感知业务数据的存在。

---

## 2. 核心类及其职责

### 2.1 资源与封装层

* **`Socket` (基础组件)**：
* **职责**：FD 的唯一所有者。负责 `socket()`、`bind()`、`listen()` 以及 `close()`。
* **打包信息**：除了持有 `fd`，还打包了本地 `port` 和远端 `peerAddr`（客户端地址）。
* **RAII 实现**：析构函数自动执行 `close(fd)`，这是防止文件描述符泄漏的核心防线。



### 2.2 事件驱动层

* **`Channel` (分发枢纽)**：
* **职责**：FD 的“说明书”。它不持有 FD，但知道 FD 关注什么事件（`events`）以及发生了什么事件（`revents`）。
* **最新机制**：
* `enableListen()`：设置为 **LT (水平触发)**，专门用于 `Acceptor`，确保新连接接入的稳健性。
* `enableReading()`：设置为 **ET (边缘触发)**，专门用于 `Connection`，提升大数据量读取的效率。




* **`Epoll` & `EventLoop` (动力核心)**：
* `Epoll` 负责操作内核红黑树，`EventLoop` 负责无限循环执行 `epoll_wait` 并调用 `Channel::handleEvent()`。



### 2.3 业务逻辑层

* **`Acceptor` (连接工厂)**：
* **职责**：私有持有监听 `Socket`，专职“接客”。
* **流程**：`listenFd` 可读 -> `accept` 获取新 FD -> 打包成新 `Socket` 对象 -> 回调给 `Server`。


* **`Connection` (通信载体)**：
* **职责**：代表一个活跃的客户端连接。持有通信用的 `Socket` 和 `Channel`。
* **机制**：在 ET 模式下循环读取数据直到 `EAGAIN`。当检测到 `read == 0` 时，发起“自毁”信号。


* **`Server` (总调度器)**：
* **职责**：管理所有的 `Acceptor`（支持多端口监听）和 `Connection` 容器。
* **管理方式**：通过 `std::map<int, Connection*>` 维护连接生命周期。



---

## 3. 关键业务流程

### 3.1 监听与接入流程

1. `Server::listenPort(port)` 被调用，检查重复后 `new Acceptor`。
2. `Acceptor` 创建 `Socket` 并执行 `bind`，创建 `Channel` 并调用 `enableListen()`。
3. `Acceptor::startListen()` 显式开启内核 `listen` 队列。
4. 新连接到来，`Acceptor::handleRead` 产生 `Socket*` 指针，通过回调传回 `Server::handleNewConnection`。

### 3.2 资源销毁链条

为了保证内存和 FD 安全，系统执行以下自上而下的销毁：

1. `Connection` 感知到对方关闭连接。
2. 回调 `Server::handleDeleteConnection(fd)`。
3. `Server` 从 `map` 中移除该项并执行 `delete connection`。
4. **`Connection` 析构函数启动**：
* 先 `delete channel`：从 `Epoll` 中摘除关注（`disableAll`）。
* 后 `delete sck`：`Socket` 析构函数调用 `close(fd)` 归还内核资源。



---

## 4. 架构优势总结

* **多端口支持**：`Server` 可以动态开启多个 `Acceptor`，每个 `Acceptor` 独立负责一个端口的接入。
* **ET/LT 混合模式**：针对监听套接字使用 LT 保证可靠性，针对通信套接字使用 ET 保证高性能，达到了平衡。
* **自动资源回收**：通过 `Socket` 封装和回调机制，确保了即使在复杂的逻辑分支下，每一个被 `accept` 出来的 FD 都能被正确关闭。


----

## 5. 现阶段成就
构建了一套**符合工业标准的 C++ 高性能网络框架**。

1. **架构跨越**：从零散的系统调用进化到成熟的 **Reactor 模式**，实现了 `EventLoop` 驱动下的全异步事件分发机制。
2. **权责分明**：通过 **RAII 思想** 封装了 `Socket` 与 `Channel`，彻底解决了 FD 泄漏与生命周期管理难题，让资源的“生”与“死”变得严丝合缝。
3. **性能优化**：精准应用了 **LT（水平触发）与 ET（边缘触发）混合模式**，在确保连接接入稳健性的同时，榨取了非阻塞 I/O 的最高读取效率。
4. **工程闭环**：通过回调机制打通了 `Acceptor`（工厂）、`Connection`（载体）与 `Server`（调度）的闭环，建立了一套自愈且可扩展的单线程高并发服务器原型。


-----
## 6. 未来扩展方向

* **线程池 (ThreadPool)**：引入多线程处理计算密集型业务，避免阻塞 Reactor 线程。
* **缓冲区 (Buffer)**：引入应用层读写缓冲区，解决 TCP 粘包/拆包问题。
* **智能指针**：使用 `std::unique_ptr` 或 `std::shared_ptr` 进一步优化对象生命周期的自动管理。


------

## Q & A

1.为什么acceptor和server间的回调函数要传递对方地址，不能从fd获取对方地址信息吗？

- 可以通过系统调用向内核查询，但在高并发场景下，成千上万次重复查询会累积性能损失

------
2.为什么抽离出 `Acceptor::startListen()`?

- 避免 server 创建 acceptor 后、设置回调函数前，epoll 事件触发