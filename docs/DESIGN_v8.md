# TimerQueue + idle timeout 架构设计

在 v7 HTTP Keep-Alive 之上，为 SubReactor 的 owner `EventLoop` 增加 **用户态定时器队列** 与 **连接 idle 超时**，避免长连接占坑。本文档含架构正文、实现要点与 Q&A。

## 目标

- Keep-Alive 连接在 **长时间无 Channel 激活** 时被主动 `handleDead`，与 EOF、写错误、`Connection: close` 并列。
- 每个 SubReactor 的 `EventLoop` **一个 timerfd**，驱动多条逻辑 Timer（每条连接一条 idle Timer）。
- 为后续 graceful shutdown 的「超时强关」预留 `addTimer` / `deleteTimer` 能力。

## 模块划分

```text
Timer              // 单条任务：id、expiration(timespec)、callback
TimerQueue         // timerfd_ + timeChannel_；map<id,Timer>；扫表、refreshClock
EventLoop          // 拥有 TimerQueue；addTimer/deleteTimer；loop 分 pass
Connection         // timerId_；读/写 handle 入口 refreshTimer；到期 handleDead
SubReactor         // 注入 addTimer/deleteTimer 回调（不直接碰 Queue）
```

| 资源 | 持有者 |
|------|--------|
| `timerfd` | `TimerQueue` |
| timer `Channel` | `TimerQueue` |
| 逻辑 Timer 表 | `TimerQueue`（`std::map<int, unique_ptr<Timer>>`） |
| pass 调度 | `EventLoop`（`isTimeChannel` 比对，无 Channel tag） |

## EventLoop 一轮 loop

```text
channels = ep->poll()   // EINTR 重试（FIX-032）

pass1: 对每个 ch（跳过 timerfd Channel）
         ch->handle()   // socket + eventfd 读清；Connection 内可 refreshTimer

pass2: 若本轮有 timerfd 就绪
         timerQueue->handleRead()   // read timerfd → 扫表 → refreshClock

pass3: drain enqueueTask 队列（含 remove、sendHttpOnLoop 等）
```

**顺序约束**：pass1 先于 pass2，避免同批 poll 里 idle 先到期、读事件后到达时误关未 refresh 的连接。

## idle 语义（定稿：Channel 激活）

- **激活**：`handleReadCallBack` / `handleWriteCallBack` 被调用（EPOLLIN/EPOLLOUT 进入 handle 即算，不要求本轮读到字节）。
- **refresh**：`deleteTimer(id)` + `addTimer(cb)`，得到新 id，`expiration = now + 60s`（常量目前在 `TimerQueue::addTimer`）。
- **到期**：Timer 回调 → `Connection::handleDead()`（幂等、`deleteTimer`、延迟 remove）。
- **worker 迟返**：回投 task 先 `working--`，若已 `dead` 则不再 `sendHttpOnLoop`（FIX-030）。

**Timer 表与 id**：全局 `timerId` 单调递增；并存条目满足 id 小 ⇒ 创建早 ⇒ expiration 早，扫表按 id 升序并在首个未过期处 `break` 即可。




## Q & A

1. idle 是什么？为什么要做 idle timeout？

- **idle（空闲）**：TCP 仍连接，但该连接 socket 的 **`Channel` 在阈值时间内没有任何 epoll 激活**（未进入读/写 `handle`）。
- 与「仅等下一 HTTP 请求」的窄语义不同：本阶段采用 **连接级 I/O 活跃超时**——读、写回调入口均算激活并续期。
- v7 下静默占坑连接可一直占 fd 与 `SubReactor::Connections`；**idle timeout** 到期后主动 `handleDead`，与 EOF、写错误、`Connection: close` 并列，作为长连接治理兜底。

-----
2. 每个 SubReactor / EventLoop 需要几个 timerfd？

- **每个 owner `EventLoop` 一个 timerfd** 即可（与 eventfd 并列）。
- 一个 timerfd 驱动 **多条逻辑 Timer**；不为每条连接各创建一个 timerfd。


-----
3. `TimerQueue` 与 `Timer` 如何分工？谁持有 timerfd？

- **`Timer`**：一条逻辑任务——句柄 id、**绝对过期时刻**、回调；本阶段 **无** 周期 repeat。
- **`TimerQueue`**：**拥有** timerfd 及其 `Channel`；维护有序表；负责增删、到期扫表、重设内核闹钟；syscall 封装在此。
- **`EventLoop`**：拥有 `TimerQueue`；对外 `addTimer` / `deleteTimer`；**不**再单独持有一份 timerfd。

----
4. `EventLoop` 如何在 poll 结果里区分 timer Channel？要给 `Channel` 加类型 tag 吗？

- **不需要 tag**。Connection 的 socket `Channel` 默认视为 I/O。
- `TimerQueue` **暴露** timer 的 `Channel` 指针供比对；`EventLoop` 分阶段调度，与 eventfd 的 `Channel` 同理。


-----
5. timerfd 可读时，如何知道是哪条 Timer 到期？

- **timerfd 不标识具体 Timer**。可读只表示「该扫用户态定时器表了」。
- 用 **当前单调钟时间 now**，取出表中所有 **`expiration <= now`** 的条目并执行回调；可能一次多条。


-----
6. loop 处理延迟会不会误杀「尚未到期」的 Timer？

- **不会**（实现正确时）。判定只有 **`expiration <= 查表时的 now`**，不是「fd 响就执行表头」。
- 延迟只会 **晚执行** 或 **一批补做** 已到期任务；略早唤醒而尚未到点时扫表为空，再重设闹钟即可。


-------
7. idle refresh 何时真正生效？与 poll 顺序有何关系？

- **激活**：`handleReadCallBack` 或 `handleWriteCallBack` 被调用（不论本轮是否读到/写到字节，只要 Channel 因 EPOLLIN/EPOLLOUT 进入 handle 即算）。
- **refresh**：在上述入口 **`deleteTimer` + `addTimer`**（`cancel` + `runAfter` 等价），**同步**执行，**不进** worker task 队列。
- 「socket 已进本轮 poll」**不等于** 已 refresh；**只有 handle 执行后**旧 Timer 才从表删除。
- **同一轮 poll** 若 idle 到期与 socket 就绪同时存在，必须先 **业务 I/O（pass1）**，再 **timer 扫表（pass2）**，否则可能先 idle 关连接、后进入 handle 续期。
- **loop**：pass1 处理 socket（可含 eventfd 读清计数）；pass2 timer；最后再 drain tasks。eventfd **不必**单独成 pass。



------
8. refresh 与 idle 到期分别做什么？

- **refresh**：`cancel(旧句柄)` + `runAfter`（实现为删旧 id、分配新 id，`expiration = now + delay`），不是原地改旧条目。
- **idle 到期**：`handleDead` 关连接，**不是** 自动再续期；`handleDead` 应 **cancel** 该连接 Timer 并幂等。


---------
9. 是否需要周期重复的 Timer？

- **本阶段不需要**。全部为 **一次性定时 + 在 Channel 激活时手动 refresh**。
- 内核闹钟每次指向表中 **最早 expiration**；逻辑层不做 `runEvery` 类周期任务。


------
10. 过期时间在架构上如何理解？

- 每条 Timer 存 **单调钟上的绝对过期时刻**（注册时 `now + delay` 一次算定）。
- 同一 `TimerQueue` 内 Timer 与 timerfd **共用同一时间维度**；不混用墙钟做相对超时。
- 扫表、refresh、reset 内核闹钟都基于同一套绝对 expiration 语义。


-------
11. idle 与连接生命周期、线程边界？

- 超时仍走 **`handleDead()`**（`disableAll` + 延迟 `remove`），不在 timer 回调栈内同步从连接表 `erase`。
- idle 的 register / cancel / refresh **仅在 owner I/O 线程**；与 buffer、Channel 边界一致。
- 回调侧注意连接 **生命周期**：idle 到期回调直接 `handleDead()`；**`handleDead` 幂等**且 **`deleteTimer`**，避免 Timer 残留与重复 remove。
- **Timer 表顺序**：全局 `timerId` 单调递增，每次 `addTimer` 用当时 `now+delay`；并存条目满足 **id 小 ⇒ 创建早 ⇒ expiration 早**，故可按 id 扫表并在首个未过期处停止（与按 expiration 有序等价）。



--------
12. 与「仅读 idle」策略的差异？

- **仅读**：只认新请求字节或 keepAlive 响应后 refresh；在途 worker 期间客户端不发数据也会计时——更贴近 RFC Keep-Alive 等下一请求。
- **本实现（Channel 激活）**：写就绪、发缓冲也会续期；客户端长期不占读但服务端仍在写 drain 时不会误杀——更贴近 **连接仍有 I/O 事件**。
- 若 worker 很慢且客户端已不发数据，**本策略仍可能到期关连接**（未再激活 Channel）；属预期，非 bug。
