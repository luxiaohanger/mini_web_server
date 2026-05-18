# 未修复问题


## OPEN-001 force shutdown 残留问题

- 发现日期：2026-05-19
- 状态：Open
- 优先级：P1
- 位置：`Server`、`SubReactor`、`EventLoop`、`ThreadPool`
- 风险：当前阶段采用 force shutdown，不保证 worker 回投、善后函数和响应完整发送；目标只是不出现 use-after-free 或线程泄漏。
- 当前状态：`SubReactor::stop()` 已在 owner loop 中停止连接、清空 `Connections` 并停止 loop；`Server` 在 sub stop 后再删除 `ThreadPool`。这可以避免 worker 回投访问已释放 `EventLoop`。
- 保留限制：当前不保证 `Connection` 最后一定在 owner loop 线程析构；如果 worker 持有最后一份 `shared_ptr<Connection>`，回投被 stopped `EventLoop` 丢弃后，`Connection` 可能在 worker 线程析构。
- 下一步：将严格 owner loop 析构作为后续工业化增强，通过自定义 deleter 或延迟销毁队列实现；如果需要 graceful shutdown，再设计输出缓冲区 drain 和超时关闭。
- 验收标准：worker 结束时即使回投被丢弃，也不会访问已释放 `EventLoop`；`SubReactor` 析构时 `Connections` 已在 `EventLoop` 存活期间清空。

## 建议执行顺序

1. 后续如需工业级析构语义，设计 owner loop 延迟销毁或自定义 deleter。
2. 后续如需 graceful shutdown，设计 outputBuffer drain、超时关闭和强制关闭路径。
