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
- **与运行中稳定性无关**。

---

## 建议执行顺序（按优先级，非编号）

1. **OPEN-001 / OPEN-002**：仅当产品上要「程序内可停服」时再实现。
