# 基于 epoll 的高性能 Web Server

这是一个面向 Linux 环境的 C++ 高性能网络服务器学习项目。项目从原始 socket 编程逐步演进到 Reactor 架构，当前阶段已实现 **单 Reactor + 线程池** 的并发模型，并封装了 `Socket`、`Channel`、`EventLoop`、`Epoll`、`Connection`、`Buffer`、`ThreadPool` 等核心组件。

项目目标不是直接复刻成熟框架，而是通过逐步实现网络库关键模块，系统学习 Linux 网络编程、事件驱动模型、非阻塞 I/O、缓冲区设计、跨线程任务通知和 C++ 资源管理。

## 技术栈

- **C++17**：面向对象封装、RAII 资源管理、移动语义、`std::function` 回调、lambda 表达式、模板编程、右值引用、完美转发、`std::future` / `std::packaged_task` 异步任务封装。
- **Linux Network Programming**：TCP socket 编程、非阻塞 I/O、`epoll` I/O 多路复用、ET/LT 触发模式、`eventfd` 跨线程唤醒、`readv` 分散读优化。
- **Reactor Pattern**：基于 `EventLoop`、`Channel`、`Epoll` 的事件分发模型，实现 I/O 事件监听、回调注册、业务处理解耦和连接生命周期管理。
- **Concurrency**：线程池、互斥锁、条件变量、任务队列、主线程 I/O 与 worker 线程业务处理分离。
- **Buffer Management**：应用层读写缓冲区、动态扩容、读写索引管理、部分写处理、非阻塞写兜底。
- **Build & Tooling**：CMake target-based 构建、模块化目录组织、Shell 脚本辅助运行。

## 当前架构

当前版本采用 **单 Reactor + 线程池** 架构：

- 主线程运行唯一 `EventLoop`，负责 `epoll_wait`、新连接接入、socket 非阻塞读写、`Channel` 事件更新。
- worker 线程由 `ThreadPool` 管理，负责处理业务逻辑，目前主要是 echo 数据处理。
- `Buffer` 作为 I/O 线程和业务线程之间的数据边界。
- worker 处理完成后通过 `eventfd` 唤醒主线程，由主线程继续执行 socket 写入或注册 `EPOLLOUT`。

### 简要架构图

```text
Client
  |
  v
Socket fd
  |
  v
Channel
  |
  v
EventLoop + Epoll  <---- eventfd wakeup
  |
  v
Connection
  |
  +--> readBuffer --> ThreadPool --> writeBuffer
                                  |
                                  v
                         EventLoop task queue
                                  |
                                  v
                         non-blocking write
```

### 核心模块职责

- `Socket`：封装 socket fd，负责 bind、listen、accept、read、write 和 fd 生命周期管理。
- `Channel`：描述 fd 关注的事件和触发的事件，保存读写回调，不持有 fd。
- `Epoll`：封装 `epoll_create1`、`epoll_ctl`、`epoll_wait`，负责事件监听和返回活跃 `Channel`。
- `EventLoop`：事件循环核心，负责分发 `Channel` 事件、执行回投任务、通过 `eventfd` 支持跨线程唤醒。
- `Acceptor`：监听端口，接收新连接，并通过回调交给 `Server`。
- `Connection`：管理单个客户端连接的读写 buffer、事件回调和业务处理流程。
- `Buffer`：应用层缓冲区，支持 `readv` 读取、动态扩容、部分写处理和 buffer 间数据转移。
- `ThreadPool`：业务线程池，负责异步处理从连接中拆出的业务任务。
- `Server`：项目入口级调度器，管理 `EventLoop`、`ThreadPool`、`Acceptor` 和 `Connection` 容器。

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
│   ├── Server.h
│   ├── Socket.h
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
│   │   ├── Server.cpp
│   │   ├── Socket.cpp
│   │   ├── ThreadPool.cpp
│   │   └── main.cpp
│   └── share/
│       └── error_solve.cpp
├── docs/
│   ├── DESIGN_v1.md
│   ├── DESIGN_v2.md
│   ├── DESIGN_v3.md
│   ├── DESIGN_v4.md
│   └── DESIGN_v4pro.md
├── scripts/
│   └── SCRIPTS.md
```

## 近期稳定性修复

当前阶段围绕单 Reactor 多线程架构完成了多项正确性修复：

- 初始化 `ThreadPool::stop`，避免工作线程读取未定义状态。
- 修复非阻塞写中 `Buffer::bufferToSck()` 按请求字节数推进读指针的问题，改为按实际写出字节数推进。
- 为 `Channel` / `Epoll` 增加显式删除 epoll 事件能力，减少对 `close(fd)` 副作用的依赖。
- 修复 `Epoll` fd 释放和 `EPOLL_CTL_DEL` 参数细节。
- 修复非阻塞 `accept` 对 `EINTR`、`EAGAIN`、`EWOULDBLOCK` 的处理。
- 调整连接断开路径，避免当前调用栈中 `Connection` 被删除后继续访问成员。

## 未来方向

下一阶段计划从当前 **单 Reactor + 线程池** 演进为 **Main-Sub Reactor** 架构：

- main reactor 只负责监听 fd 和 accept 新连接。
- sub reactor 负责具体连接的 I/O 事件、`Channel` 更新和连接生命周期。
- worker 线程只处理脱离连接生命周期的业务数据，不直接持有裸 `Connection*`。
- 业务处理完成后，将响应结果回投到连接所属的 sub reactor，由 owner loop 安全执行 I/O。

该方向可以进一步降低主线程压力，明确连接归属线程，为更高并发和更清晰的生命周期管理打基础。

## 学习日志
- stage1 : 实现简单的socket连接
- stage2 : 增加错误判断 `errif` ; 实现 echo 服务器 ; 重构CMAKE , 规范基于目标的配置
- stage3 : 使用 非阻塞 ET 式 epoll 监听 fd ，实现单文件 [代码雏形](/src/server/prev_main.cpp); 使用面向对象重构文件 ; 学习复杂依赖关系的类如何编写 .h 和 .cpp 文件
- stage4 : 将裸露文件描述符封装为 `Channel`；实现简单的 Reactor 架构和事件驱动、任务分发；尝试自己重构高度耦合的类；新建docs文档，记录架构变化，实现 [架构设计v1](/docs/DESIGN_v1.md)
- stage5 : 彻底重构设计架构，实现工业级 C++ 网络库的核心雏形，详见 [架构设计v2](/docs/DESIGN_v2.md)
- stage6 : 重新设计 `Socket` 类，封装相关底层调用；调整持有裸露 fd 的类持有 `Socket*` ,形成 RAII 资源管理闭环；引入 `Buffer` 类，实现高性能的缓冲区数据传输，详见 [架构设计v3](/docs/DESIGN_v3.md)
- stage7 : 引入线程池，实现高并发；引入 eventfd，实现主线程控制 IO，任务线程负责 buffer 处理；完善 `Channel`,实现 ET 触发的非阻塞写；单一 Reactor 架构的、多线程、高性能服务器彻底形成闭环；详见 [架构设计v4](/docs/DESIGN_v4.md)
- stage8 : 围绕单 Reactor 多线程架构进行稳定性修复；修复线程池状态初始化、非阻塞部分写、epoll 事件删除、accept 可恢复错误、连接断开路径等问题；梳理当前架构边界和主从多 Reactor 演进方向，详见 [架构设计v4pro](/docs/DESIGN_v4pro.md)

## Quick Start

```shell
cmake --build build
```

运行方式参见 [scripts/SCRIPTS.md](./scripts/SCRIPTS.md)。


## 致谢
本项目学习参考了 `https://github.com/yuesong-feng/30dayMakeCppServer`。
