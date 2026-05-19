# 未修复问题

## OPEN-001 / OPEN-002 运行时退出（暂缓，非当前缺陷）

- 发现日期：2026-05-19
- 状态：**暂缓设计**（学习阶段服务器按「启动后常驻终端」使用，未提供 Ctrl+C / 命令行退出等机会）
- 优先级：P3（待明确要做「可停服」时再提）
- 设计事实：
  - `main.cpp`：`listenPort` → `start()` 阻塞在主 `EventLoop::loop()`，进程靠 **Ctrl+C 杀进程** 或关终端结束，**没有**从程序内部退出的路径。
  - 因此 OPEN-002 里「主 reactor 不唤醒 `epoll_wait`」「`~Server` 与 `start()` 死锁」等在**当前用法下不是必现 bug**，而是**尚未实现停服**时的代码留白。
- 已有骨架（FIX-022）：`stop_half_force()`、`SubReactor::stop()`（`enqueueTask` + `join`）、`ThreadPool::stop()`，面向**将来**析构/信号停服，不是现网运行路径。
- 若以后要做「可退出」再一并定稿：
  - 公开 `stop_half_force()` 或等价 API；`start()` 放独立线程，或 `MainReactor::stop()` 用 `enqueueTask` 唤醒主 loop；
  - 再区分 graceful（等连接/任务）与 force（OPEN-001 原语义）。
- **与运行中稳定性无关**；优先 **OPEN-003**、**OPEN-004**。

## OPEN-003 `readFromSck` 未处理其它读错误

- 发现日期：2026-05-19
- 状态：Open
- 优先级：P0（运行中）
- 位置：`src/server/Connection.cpp` `readFromSck()`（约 28–45 行）
- 现象：`readv` 返回 `-1` 且 `errno` 不是 `EINTR` / `EAGAIN` / `EWOULDBLOCK` 时，**无 `break`**，`while (true)` 持续循环。
- 机制：每次循环 `Buffer::sckToBuffer` 在栈上分配约 64KB（`Buffer.cpp`）。
- 建议：增加 `else if (read_byte == -1) { state = ConnState::dead; handleDead(); break; }`（与写错误路径一致）。

## OPEN-004 `epoll_wait` 遇 `EINTR` 直接 `exit`

- 发现日期：2026-05-19
- 状态：Open
- 优先级：P1
- 位置：`src/server/Epoll.cpp` `Epoll::poll()`（约 37–38 行）
- 现象：`epoll_wait` 返回 `-1` 且 `errno == EINTR` 时，`errif` 打印并 **`exit(1)`**。
- 对比：`EventLoop::readCallback`、`Socket::acceptConnection` 对 `EINTR` 会重试或忽略。
- 建议：`EINTR` 时重新 `epoll_wait`，不要 `errif`。

---

## 建议执行顺序（按优先级，非编号）

1. **OPEN-003**、**OPEN-004**：运行中稳定性。
2. **OPEN-001 / OPEN-002**：仅当产品上要「程序内可停服」时再实现。
