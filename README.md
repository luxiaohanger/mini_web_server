# mini_web_server

> 基于 epoll 的 Linux C++ 高性能服务器

在 Linux 上实现的 C++ 多线程服务端；采用 Reactor 事件驱动与非阻塞 I/O，能处理高并发连接场景。

## 特性

- Linux + C++17，CMake 构建，附带简易测试客户端
- 非阻塞 I/O、`epoll`、Reactor 风格的事件循环
- 线程池处理业务，I/O 与计算分离
- 自研 `Buffer`、`Connection`、Reactor 组件
- 设计文档与缺陷跟踪：`docs/DESIGN_v*.md`、`issue_log/`


## 技术栈

- **C++17**：面向对象封装、RAII 资源管理、移动语义、`std::function` 回调、lambda 表达式、模板编程、右值引用、完美转发、`std::future` / `std::packaged_task` 异步任务封装、`std::unique_ptr` / `std::shared_ptr` / `std::enable_shared_from_this` 管理组件与连接生命周期。
- **Linux Network Programming**：TCP socket 编程、非阻塞 I/O、`epoll` I/O 多路复用、ET/LT 触发模式、`eventfd` 跨线程唤醒、`readv` 分散读优化。
- **Reactor Pattern**：基于 `EventLoop`、`Channel`、`Epoll` 的事件分发模型；主 Reactor 负责 accept，多个 Sub Reactor 各自管理连接 I/O 与生命周期。
- **Concurrency**：线程池、互斥锁、条件变量、任务队列、I/O 线程与 worker 线程职责分离、round-robin 连接分发。
- **Buffer Management**：应用层读写缓冲区、动态扩容、读写索引管理、部分写处理、非阻塞写兜底、I/O 线程与业务线程之间的数据快照边界。
- **Build & Tooling**：CMake target-based 构建、模块化目录组织、Shell 脚本辅助运行。

## 当前架构

当前版本采用 **主从多 Reactor + 线程池** 架构：

- **MainReactor** 运行在主线程，持有 `Acceptor`，只负责监听端口和 accept 新连接。
- **SubReactor** 数组与 CPU 核心数对齐，每个 SubReactor 固定一个 I/O 线程和一个 `EventLoop`，负责已建立连接的读写、`Channel` 更新和连接移除。
- **Server** 通过 round-robin 将新连接投递到某个 SubReactor 的 `EventLoop` 任务队列，由 owner loop 创建 `std::shared_ptr<Connection>`。
- **ThreadPool** 负责业务逻辑，worker 只处理从 owner loop 取出的 `std::string` 数据快照，不直接操作 socket、`Channel` 或内部 buffer。
- 业务处理完成后，响应写回 `writeBuffer` 的动作回投到连接所属的 owner loop，由 I/O 线程继续执行非阻塞写或注册 `EPOLLOUT`。
- **Connection** 由 `SubReactor` 以 `std::shared_ptr` 持有；继承 `std::enable_shared_from_this`；内部成员（`Socket`、`Channel`、`Buffer`）为 `std::unique_ptr`。
- **半关闭收束**：`read == 0` 进入 `peerClose`，仍留在连接表直至写排空且 `working == 0` 后进入 `dead` 并移除；避免写路径与过早 `erase` 竞态。
- **运行方式**：`main` 启动后常驻终端处理连接，当前未实现程序内优雅退出（停服为后续演进，见 issue_log）。

### 简要架构图

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
  | read socket -> readBuffer (EOF -> peerClose)
  | dataOut -> std::string 快照
  v
ThreadPool worker (working++)
  |
  | 业务处理生成 response
  v
owner EventLoop task queue (working--, dataIn)
  |
  | response -> writeBuffer
  v
trySendToSck / EPOLLOUT
  |
  | peerClose && working==0 && 写排空 -> dead -> remove
```

### 核心模块职责

- `Socket`：封装 socket fd，负责 bind、listen、accept、read、write 和 fd 生命周期管理。
- `Channel`：描述 fd 关注的事件和触发的事件，保存读写回调，不持有 fd。
- `Epoll`：封装 `epoll_create1`、`epoll_ctl`、`epoll_wait`，负责事件监听和返回活跃 `Channel`。
- `EventLoop`：事件循环核心，负责分发 `Channel` 事件、执行回投任务、通过 `eventfd` 支持跨线程唤醒。
- `MainReactor`：主 Reactor，持有主线程 `EventLoop` 和 `Acceptor`，只负责监听与 accept，不管理普通连接。
- `SubReactor`：子 Reactor，持有固定 I/O 线程、`unique_ptr<EventLoop>` 和 `Socket* -> shared_ptr<Connection>` 连接表；仅在 `Connection` 进入 `dead` 后从表移除。
- `Acceptor`：监听端口，接收新连接，并通过回调交给 `Server`。
- `Connection`：管理单连接读写 buffer、事件回调与 `ConnState`；`working` 标记异步业务是否在途；worker 只处理数据快照，socket I/O 仍在 owner loop 执行。
- `Buffer`：应用层缓冲区，支持 `readv` 读取、动态扩容、部分写处理和 buffer 间数据转移。
- `ThreadPool`：业务线程池，负责异步处理从连接中拆出的业务任务。
- `Server`：入口调度器，`unique_ptr` 持有 `MainReactor`、多个 `SubReactor` 与 `ThreadPool`，负责新连接 round-robin 分发。

## 文件结构

```text
mini_web_server/
├── CMakeLists.txt
├── README.md
├── include/
│   ├── Acceptor.h
│   ├── Buffer.h
│   ├── Channel.h
│   ├── Connection.h
│   ├── Epoll.h
│   ├── EventLoop.h
│   ├── MainReactor.h
│   ├── Server.h
│   ├── Socket.h
│   ├── SubReactor.h
│   ├── ThreadPool.h
│   └── error_solve.h
├── src/
│   ├── client/
│   │   └── main.cpp
│   ├── server/
│   │   ├── Acceptor.cpp
│   │   ├── Buffer.cpp
│   │   ├── Channel.cpp
│   │   ├── Connection.cpp
│   │   ├── Epoll.cpp
│   │   ├── EventLoop.cpp
│   │   ├── MainReactor.cpp
│   │   ├── Server.cpp
│   │   ├── Socket.cpp
│   │   ├── SubReactor.cpp
│   │   ├── ThreadPool.cpp
│   │   ├── main.cpp
│   │   └── prev_main.cpp          # stage3 单文件 epoll 雏形
│   └── share/
│       └── error_solve.cpp
├── docs/
│   ├── DESIGN_v1.md
│   ├── DESIGN_v2.md
│   ├── DESIGN_v3.md
│   ├── DESIGN_v4.md
│   ├── DESIGN_v4pro.md
│   ├── DESIGN_v5.md               
│   └── DESIGN_v6.md               # 当前
├── issue_log/
│   ├── fixed_issues.md            # 已修复 / 已确认问题归档
│   └── open_issues.md             # 未修复 / 暂缓设计问题跟踪
└── scripts/
    ├── SCRIPTS.md
    └── up.sh
```

## 近期稳定性修复

当前阶段围绕 **主从多 Reactor + 线程池** 架构完成了多项正确性修复，详见 [修复日志](./issue_log/fixed_issues.md)。重点包括：

- 初始化 `ThreadPool::stop`，避免工作线程读取未定义状态。
- 修复非阻塞写中 `Buffer::bufferToSck()` 按请求字节数推进读指针的问题，改为按实际写出字节数推进。
- 为 `Channel` / `Epoll` 增加显式删除 epoll 事件能力，修复 `Epoll` fd 释放和 `EPOLL_CTL_DEL` 参数细节。
- 修复非阻塞 `accept` 对 `EINTR`、`EAGAIN`、`EWOULDBLOCK` 的处理。
- 修复 `EventLoop` 析构顺序和 `stop` 跨线程数据竞争（`std::atomic<bool>`）。
- 修复 `SubReactor` 清理连接时的 map 迭代器失效问题。
- `Connection` 改用 `shared_ptr` + `enable_shared_from_this`，worker 只处理数据快照，响应写回回投 owner loop，修复异步 use-after-free 和 buffer 数据竞争。
- 明确 EOF 语义：`read == 0` 只表示对端关闭写方向，EOF 前已读数据仍会继续处理并尝试发送响应。
- 核心模块 **智能指针所有权重构**（`Server` / Reactor / `Connection` 成员）；`ConnState` + `working` 落地半关闭收束（FIX-023/024）；`EventLoop` 析构顺序修正（FIX-025）；修复 `Connection` 构造形参遮蔽成员（FIX-021）。

## 当前保留问题

详见 [问题清单](./issue_log/open_issues.md)。

- **运行中**：`readFromSck` 对非 `EINTR`/`EAGAIN` 读错误未处理（OPEN-003）；`epoll_wait` 遇 `EINTR` 直接退出（OPEN-004）。
- **暂缓**：程序内停服与 graceful shutdown（进程常驻、Ctrl+C 结束为当前用法）；`stop_half_force()` 为预留骨架。

## 未来方向

- 实现 **可停服** 与 **graceful shutdown**（主 loop 唤醒、公开 stop API、输出 drain、超时关闭）。
- 将 SubReactor 管理抽象为专门的 `EventLoopThreadPool`，进一步解耦 `Server` 与 I/O 线程调度。
- 在稳定架构基础上扩展 HTTP 协议解析、路由和静态资源服务。

## 演进记录

- stage1 : 实现简单的 socket 连接
- stage2 : 增加错误判断 `errif`；实现 echo 服务器；重构 CMAKE，规范基于目标的配置
- stage3 : 使用非阻塞 ET 式 epoll 监听 fd，实现单文件 [代码雏形](/src/server/prev_main.cpp)；使用面向对象重构文件；拆分 `.h` / `.cpp` 与模块依赖
- stage4 : 将裸露文件描述符封装为 `Channel`；实现简单的 Reactor 架构和事件驱动、任务分发；尝试自己重构高度耦合的类；新建 docs 文档，记录架构变化，实现 [架构设计 v1](/docs/DESIGN_v1.md)
- stage5 : 彻底重构设计架构，实现工业级 C++ 网络库的核心雏形，详见 [架构设计 v2](/docs/DESIGN_v2.md)
- stage6 : 重新设计 `Socket` 类，封装相关底层调用；调整持有裸露 fd 的类持有 `Socket*`，形成 RAII 资源管理闭环；引入 `Buffer` 类，实现高性能的缓冲区数据传输，详见 [架构设计 v3](/docs/DESIGN_v3.md)
- stage7 : 引入线程池，实现高并发；引入 eventfd，实现主线程控制 IO，任务线程负责 buffer 处理；完善 `Channel`，实现 ET 触发的非阻塞写；单一 Reactor 架构的、多线程、高性能服务器彻底形成闭环，详见 [架构设计 v4](/docs/DESIGN_v4.md)
- stage8 : 围绕单 Reactor 多线程架构进行稳定性修复；修复线程池状态初始化、非阻塞部分写、epoll 事件删除、accept 可恢复错误、连接断开路径等问题；梳理当前架构边界和主从多 Reactor 演进方向，详见 [架构设计 v4pro](/docs/DESIGN_v4pro.md)
- stage9 : 从单 Reactor 演进到 **主从多 Reactor**；引入 `MainReactor` / `SubReactor`，主线程只 accept，子 I/O 线程 round-robin 管理连接；`Connection` 改用 `shared_ptr` 管理生命周期，worker 只处理数据快照；明确 TCP 半关闭语义，详见 [架构设计 v5](/docs/DESIGN_v5.md)
- stage10 : 服务器核心 **智能指针所有权重构**；`ConnState`（`connected` / `peerClose` / `dead`）与 `working` 计数实现半关闭下延迟 `remove`；运行模型为启动后常驻终端，详见 [架构设计 v6](/docs/DESIGN_v6.md)

## Quick Start

```shell
cmake --build build
```

运行方式参见 [scripts/SCRIPTS.md](./scripts/SCRIPTS.md)。

## 致谢

本项目参考了 `https://github.com/yuesong-feng/30dayMakeCppServer`。
