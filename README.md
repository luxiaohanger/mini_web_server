# mini_web_server 

> 基于 epoll 的 Linux C++ 高性能服务器

在 Linux 上实现的 C++ 多线程服务端；采用 Reactor 事件驱动与非阻塞 I/O，能处理高并发连接场景。

## 特性

- Linux + C++17，CMake 构建，附带简易测试客户端
- 非阻塞 I/O、`epoll`、Reactor 风格的事件循环
- **可选** ThreadPool（`-t on|off`，Echo 默认 **off**，I/O 线程直出；重业务可 `-t on`）
- 自研 `Buffer`、`Connection`、Reactor 组件
- 支持 HTTP/1.1 子集 ：合法 GET、Keep-Alive、短连接、`400` 拒绝非法请求
- 超时连接处理：`timerfd` + `TimerQueue`；Channel 激活续期 idle，到期 `handleDead`
- 程序内停服：SIGINT / SIGTERM → `main` 控制线程 → Main/Sub Reactor（及可选 ThreadPool）有序 join
- 设计文档与缺陷跟踪：`docs/DESIGN_v*.md`、`issue_log/`
- 性能工程：wrk 吞吐基线 + perf 热点采样 : `benchmark_log/`


## 技术栈

- **C++17**：面向对象封装、RAII 资源管理、移动语义、`std::function` 回调、lambda 表达式、模板编程、右值引用、完美转发、`std::future` / `std::packaged_task` 异步任务封装、`std::unique_ptr` / `std::shared_ptr` / `std::enable_shared_from_this` 管理组件与连接生命周期、`enum class` 状态机。
- **Linux Network Programming**：TCP socket 编程、非阻塞 I/O（`fcntl` / `O_NONBLOCK`）、`epoll` I/O 多路复用、ET 触发、`eventfd` 跨线程唤醒、`timerfd` + `CLOCK_MONOTONIC` 定时、`readv` 分散读、`EINTR` / `EAGAIN` 错误处理。
- **Reactor Pattern**：基于 `EventLoop`、`Channel`、`Epoll` 的事件分发；MainReactor accept，多 Sub Reactor 管连接 I/O；`TimerQueue` 与业务 I/O 分 pass 调度；`eventfd` 唤醒 loop 以支持跨线程 `stop`。
- **Concurrency**：可选 ThreadPool（`-t on|off`）、互斥锁、条件变量、任务队列、I/O 与 worker 职责分离（`-t on`）、`enqueueTask` 回投 owner loop、round-robin 连接分发、`working` 协调异步与半关闭。
- **Buffer & Protocol**：应用层读写缓冲区、动态扩容、部分写与 EPOLLOUT 兜底；HTTP/1.1 最小子集（GET、Keep-Alive、短连接）；连接 idle 超时治理。
- **Build & Tooling**：CMake 构建、Shell / tmux 脚本、HTTP 验收 client（`-n` / `-t` idle）；server 启动 `-t on|off` 控制 ThreadPool
- **Performance Engineering**：wrk 回归验收（RPS / 延迟 / Errors）；`perf record` + 调用栈定位热点；`scripts/perf_bench.sh` 一键采样 + 结果写回 `benchmark_log/artifacts`

## 当前架构

当前版本采用 **主从多 Reactor + 可选 ThreadPool + HTTP/1.1 最小业务 + 程序内停服**：

- **main 线程** 为控制线程：解析 `-t on|off`（默认 **off**），注册 SIGINT/SIGTERM，等待停服标志后调用 `Server::stop()`；**不** 跑 `epoll_wait`。
- **MainReactor** 运行在独立 I/O 线程，持有 `Acceptor`，只负责监听与 accept；`listenPort` / `stop` 经 `enqueueTask` 与 loop 通信。
- **SubReactor** 数组与 CPU 核心数对齐，每个 SubReactor 固定一个 I/O 线程和一个 `EventLoop`，负责已建立连接的读写、`Channel` 更新和连接移除。
- **Server** 通过 round-robin 将新连接投递到某个 SubReactor 的 `EventLoop` 任务队列，由 owner loop 创建 `std::shared_ptr<Connection>`；`stop()` 顺序为 MainReactor → SubReactor(s) → ThreadPool（仅 `-t on` 时）。
- **ThreadPool（`-t on`）**：HTTP 响应组包在 worker 完成，再 `enqueueTask` 回 owner 写回；worker 不直接操作 socket、`Channel` 或 buffer。
- **Echo 默认（`-t off`）**：`HttpProcess::parse` 与 `buildResponse` 均在 owner I/O 线程完成，经 `sendHttpOnLoop` → `trySendToSck` 写回，无跨线程双跳。
- **Connection** 由 `SubReactor` 以 `std::shared_ptr` 持有；`-t on` 时 `working` 协调 worker 与半关闭；`-t off` 时业务同步，`working` 恒为 0。
- **半关闭收束**：`read == 0` 进入 `peerClose`，写排空且 `working == 0` 后 `handleDead`；避免回调栈内同步 `erase`。
- **TimerQueue**：每个 SubReactor 的 `EventLoop` 持一个 `timerfd`；连接在读/写 `handle` 入口续期 idle Timer，到期 `handleDead`。


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
  | read -> readBuffer -> HttpProcess::parse (I/O)
  | kComplete -> HttpRequest
  v
  +-- [-t off 默认] buildResponse -> sendHttpOnLoop -> trySendToSck (同 I/O 线程)
  |
  +-- [-t on] ThreadPool (working++) -> buildResponse
              -> owner enqueueTask (working--, sendHttpOnLoop)
  |
  | writeBuffer -> trySendToSck / EPOLLOUT
  v
handleDead -> enqueueTask(remove)

停服：
  SIGINT/SIGTERM -> main -> Server::stop()
    -> MainReactor stop (join) -> SubReactor stop: Connection::stop() (join)
    -> ThreadPool stop（仅 -t on）
```

### 核心模块职责

- `Socket`：封装 socket fd，负责 bind、listen、accept、read、write 和 fd 生命周期管理。
- `Channel`：描述 fd 关注的事件和触发的事件，保存读写回调，不持有 fd；`remove()` 从 epoll 显式摘除（停服与 `handleDead` 共用）。
- `Epoll`：封装 `epoll_create1`、`epoll_ctl`、`epoll_wait`，负责事件监听和返回活跃 `Channel`。
- `EventLoop`：事件循环核心；pass1 业务 I/O → pass2 timer → drain tasks；`eventfd` 跨线程唤醒；`addTimer` / `deleteTimer` 转发至 `TimerQueue`。
- `Timer` / `TimerQueue`：单调钟绝对过期时间、按 id 扫表、`timerfd_settime` 重设最早闹钟；表空时 disarm。
- `MainReactor`：主 Reactor，独立 I/O 线程运行 `EventLoop`，持有 `Acceptor`；只负责监听与 accept，不管理普通连接。
- `SubReactor`：子 Reactor，持有固定 I/O 线程、`unique_ptr<EventLoop>` 和 `Socket* -> shared_ptr<Connection>` 连接表；运行时 `handleDead` 延迟 remove；停服时对连接调 `Connection::stop()`。
- `Acceptor`：监听端口，接收新连接，并通过回调交给 `Server`。
- `Connection`：管理单连接读写 buffer、`ConnState` 与 `onHttp`；`-t on` 时 `working` 标记 worker 在途；`handleDead`（运行时）与 `stop()`（停服）分工见 DESIGN_v9。
- `HttpProcess`：HTTP 请求行/头解析与响应组包；解析在 I/O 线程；组包在 I/O 线程（`-t off`）或 worker（`-t on`）。
- `Buffer`：应用层缓冲区，支持 `readv`、按行解析（`findCRLF`）、动态扩容与部分写处理。
- `ThreadPool`：可选业务线程池（`-t on`）；Echo 默认不创建。
- `Server`：入口调度器，持有 `MainReactor`、多个 `SubReactor` 与可选 `ThreadPool`；`start()` 启动各 I/O 线程后返回；`stop()` 编排停服（`main` 必须显式调用）。

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
│   ├── HttpProcess.h
│   ├── Epoll.h
│   ├── EventLoop.h
│   ├── MainReactor.h
│   ├── Server.h
│   ├── Socket.h
│   ├── SubReactor.h
│   ├── ThreadPool.h
│   ├── Timer.h
│   ├── TimerQueue.h
│   └── error_solve.h
├── src/
│   ├── client/
│   │   └── main.cpp
│   ├── server/
│   │   ├── Acceptor.cpp
│   │   ├── Buffer.cpp
│   │   ├── Channel.cpp
│   │   ├── Connection.cpp
│   │   ├── HttpProcess.cpp
│   │   ├── Epoll.cpp
│   │   ├── EventLoop.cpp
│   │   ├── MainReactor.cpp
│   │   ├── Server.cpp
│   │   ├── Socket.cpp
│   │   ├── SubReactor.cpp
│   │   ├── ThreadPool.cpp
│   │   ├── Timer.cpp
│   │   ├── TimerQueue.cpp
│   │   ├── main.cpp
│   │   └── prev_main.cpp          # stage3 单文件 epoll 雏形
│   └── share/
│       └── error_solve.cpp
├── docs/
│   ├── DESIGN_v1.md … DESIGN_v9.md
│   └── DESIGN_v10.md              # 压测与性能优化记录
├── benchmark_log/
│   ├── README.md                  # 命名规则、环境约束、wrk / perf 流程
│   ├── TEMPLATE.md
│   ├── v9_20260523_wrk_baseline.md
│   ├── v10.0_20260523_bench.md    # v10.0：wrk + perf
│   ├── v10.1_20260524_bench.md    # v10.1：ThreadPool 默认 off，wrk + perf
│   └── artifacts/                 # perf.data / 火焰图等（产物不跟踪）
├── issue_log/
│   ├── fixed_issues.md            # 已修复 / 已确认问题归档
│   └── open_issues.md             # 未修复 / 暂缓设计问题跟踪
└── scripts/
    ├── SCRIPTS.md
    └── perf_bench.sh              # RelWithDebInfo 重编 + wrk 负载 + perf 采样
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
- **HTTP 阶段**：`HttpProcess` 解析/组包、Echo 改为 `onHttp`；`handleDead` 延迟 `remove`（FIX-026）；内置 HTTP 验收客户端。
- **v8 idle**：`TimerQueue` + 连接 idle；`epoll_wait` EINTR 重试、读错误收束、worker 回投 `dead` 守卫（FIX-027～032）；client 支持 `-t` idle 验收。
- **v9 停服**：`main` 控制线程 + 信号触发 `Server::stop()`；MainReactor 独立 I/O 线程与 `enqueueTask` 唤醒；`Connection::stop` / `handleDead` 分工（FIX-033）。

## 当前保留问题

详见 [问题清单](./issue_log/open_issues.md)。当前无 OPEN 缺陷条目。

## 性能分析

本项目在功能与稳定性闭环之后进入 **性能工程阶段（v10+）**：不凭直觉改代码，而是 **先度量、再画像、后优化、用同版本复测验收**。流程与产物见 [`benchmark_log/README.md`](./benchmark_log/README.md)、[`docs/DESIGN_v10.md`](./docs/DESIGN_v10.md)。

### 工程方法

```text
现象 / 瓶颈  →  可复现基线（wrk）  →  热点画像（perf）
      ↑                                      |
      └──── src 变更  ←  结论写入 DESIGN + benchmark_log
```

### 优化记录

- **基础设施（v10.0 前）**：建立 `benchmark_log/` 版本报告、`scripts/perf_bench.sh`（RelWithDebInfo 重编 + wrk 负载 + perf 符号表 §0～§3 + 火焰图）；wrk 作 Release 验收 RPS，perf 定 CPU 优化方向。

- **v10.0 · 移除热路径 I/O**  
  - **分析**：v9 close 重压测终端假死；v10.0 wrk KA ~32.8k RPS；perf 显示 enqueue/future 双跳 ~25% All、write 与 kernel loopback 为主，热路径 `std::cout` 干扰短连接压测。  
  - **变更**：删除 `Connection::handleDead()` / `stop()` 内同步 `std::cout`。  
  - **结果**：短连接可跑完；KA 相对 v9 +12.6%；perf 画像确立「双跳 + write」为下一优化目标。详见 [v10.0 bench](./benchmark_log/v10.0_20260523_bench.md)。

- **v10.1 · Echo 默认关 ThreadPool（I/O 直出）**  
  - **分析**：v10.0 perf 中 ThreadPool worker ~22% All、enqueue ~25%、§3 pthread ~6%；Echo 下 `buildResponse` 极轻，双跳为纯开销。  
  - **变更**：`main` 增加 `-t on|off`（默认 **off**）；关池时在 owner I/O 线程同步 `parse` → `buildResponse` → `sendHttpOnLoop`；`-t on` 保留 v10.0 跨线程路径。  
  - **结果**：KA **~69.6k RPS**（+112% vs v10.0）；延迟 avg **284us**（v10.0 611us）；perf 中 enqueue/ThreadPool **消失**，§2 network ~20%、§3 readv ~9%，§1 热点收敛为 onHttp → 读写；下一项为 Timer 刷新与写路径减拷贝。详见 [v10.1 bench](./benchmark_log/v10.1_20260524_bench.md)。

## 未来方向

- **v10.2+**：Timer idle 刷新降频/合并、写路径减拷贝；重 CPU 业务时再 `-t on` 启用 ThreadPool

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
- stage11 : **HTTP/1.1 最小实现**（合法 GET、Keep-Alive、短连接、非法→400）；`HttpProcess` + 验收 client；`handleDead` 对齐 `enqueueTask` 延迟 remove（FIX-026），详见 [架构设计 v7](/docs/DESIGN_v7.md)
- stage12 : **TimerQueue + idle timeout**（`timerfd`、Channel 激活续期、约 60s 无 I/O 则 `handleDead`）；client `-t` / `-n`，详见 [架构设计 v8](/docs/DESIGN_v8.md)
- stage13 : **程序内停服**（SIGINT/SIGTERM、`main` 控制线程、`Server::stop()`、Main/Sub `enqueueTask` 唤醒 loop）；`Connection::stop` 与 `handleDead` 分工，详见 [架构设计 v9](/docs/DESIGN_v9.md)
- stage14 : **性能工程**（wrk + perf 度量闭环与基于画像的 src 优化），详见上文 [性能分析](#性能分析)

## Quick Start

详见 [scripts/SCRIPTS.md](./scripts/SCRIPTS.md)。


## 致谢

本项目参考了 `https://github.com/yuesong-feng/30dayMakeCppServer`。
