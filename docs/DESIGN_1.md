# Reactor 设计架构
我们的服务器模型是 “事件驱动”，采用的设计模式是 “Reactor” 模式，目前的类封装包括：Server , EventLoop , Epoll , Channel

------
## Server
最外层的用户调用类

私有 EventLoop 和 Epoll 指针 , 提供端口监听和开启事件循环api
```cpp
void listenPort(int port);
void startLoop();
```
------
## EventLoop
事件循环核心

不断从 epoll 获取新的待处理事件（channel）并调用事件对应处理函数进行处理

和 Server 共享 Epoll 指针，Server 创建时赋值给它

-------
## Epoll
事件监听器

封装内核 epfd 文件描述符，维护事件数组，向 EventLoop 提供触发事件列表，向 channel 提供挂载、更改api

```cpp
// 获取事件 channel 列表
std::vector<Channel*> poll();
// 同步 channel 的最新信息，更新epoll
// 挂载新channel
void updateChannel(Channel* channel);
```

--------
## Channel
文件描述符封装类

新事件将调用自己的处理函数 `Channel::handle()` 处理不同情况，不同情况通过自定义的希望被监听事件和触发事件区分；`Epoll::poll()` 加载触发事件时，将内核返回的触发原因传入 `Channel::revents`


### details
为了更好的区分不同的文件描述符，从而进行不同的处理，我们希望封装一个 channel 类记录文件描述符更多的信息

```cpp
typedef union epoll_data {
  void *ptr;
  int fd;
  uint32_t u32;
  uint64_t u64;
} epoll_data_t;

struct epoll_event {
  uint32_t events;	/* Epoll events */
  epoll_data_t data;	/* User data variable */
} __EPOLL_PACKED;
```

由于 epoll_data 是一个联合体，使用共享内存，而我们之前把它当作 fd 使用，现在可以使用 ptr 指针，指向 Channel 实例，从而实现更多信息记录


----

## 缺陷和改进计划
`Channel` 需要调用 `Epoll` 的 API（如 `updateChannel`）来把自己挂载上去，导致 `Channel` 必须持有 `Epoll` 的引用。而 `Server` 又创建了它们两者，形成了一个复杂的三角关系。


这种耦合违反了**依赖倒置原则**：

1. **循环依赖**：`Epoll` 监控 `Channel`，而 `Channel` 又要操作 `Epoll`。
2. **难以测试**：你很难在没有一个真实 `Epoll` 实例的情况下单独测试 `Channel` 的逻辑。
3. **扩展受限**：如果以后想引入多个 `EventLoop`（每个 Loop 有自己的 Epoll），`Channel` 该选哪一个？

---

### 解耦的“标准姿势”：引入 EventLoop 作为中介

在成熟的框架（如 Muduo）中，`Channel` 并不直接看到 `Epoll`，它只看到 **`EventLoop`**。

#### 调整后的逻辑：

* **Channel**：持有一个 `EventLoop*`。当它需要更新自己的状态时，调用 `loop_->updateChannel(this)`。
* **EventLoop**：它作为“中间人”，持有 `Epoll` 的唯一所有权。它接收 `Channel` 的请求，然后转发给自己的 `Epoll` 成员。
* **Server**：只负责初始化 `EventLoop`。

#### 代码逻辑对比：

**耦合版 (你目前的做法)：**

```cpp
// Channel 内部直接调用 Epoll
void Channel::enableReading() {
    epoll_->updateChannel(this); // Channel 必须持有 Epoll*
}

```

**解耦版 (推荐做法)：**

```cpp
// Channel 只跟所属的 Loop 说话
void Channel::enableReading() {
    ownerLoop_->updateChannel(this); 
}

// EventLoop 内部再指挥 Epoll
void EventLoop::updateChannel(Channel* channel) {
    poller_->updateChannel(channel); // poller_ 即是你的 Epoll 类
}

```

---

### Server 里的“终极解耦”

如果你把 `EventLoop` 当作大管家，`Server` 的逻辑会变得极其清爽。`Server` 不需要知道 `Epoll` 的存在，它只需要：

1. 创建一个 `EventLoop`。
2. 把这个 `Loop` 传给它创建的每一个 `Channel`。

```cpp
void Server::listenPort(int port) {
    // 这里的 loop_ 是 Server 的成员
    Channel* listenChannel = new Channel(listenFd, loop_); 
    listenChannel->setReadCallback(std::bind(&Server::handleNewConnection, this));
    listenChannel->enableReading(); 
}

```

---

### 为什么要多绕这一圈？

看似多写了一个 `EventLoop::updateChannel` 函数，但它带来了巨大的好处：

* **线程安全控制**：这是最关键的。如果你的 `Server` 在线程 A，而 `Epoll` 在线程 B，直接调用 `epoll_ctl` 会出事。有了 `EventLoop`，你可以实现“如果当前线程不是 Loop 所在的线程，就将更新任务放入任务队列”的逻辑。
* **多 Loop 支持**：当你想要实现 **Main-Slave Reactor**（主从模式）时，你只需要创建多个 `EventLoop` 实例。`Channel` 只要抱紧它所属的那个 `Loop` 的大腿，就能自动关联到正确的 `Epoll` 上。

### 总结建议

不要让 `Channel` 直接去碰 `Epoll`。把 `Epoll` 藏在 `EventLoop` 的肚子里，让 `Channel` 只和 `EventLoop` 交互。

这样你的 `Server` 就解脱了：它只需要管理一个 `EventLoop` 指针，剩下的“脏活”都由 `EventLoop` 在内部调度完成。

你现在的 `EventLoop` 类里，是不是已经有一个 `Epoll` 成员了？如果是，直接把更新接口挪过去试试。