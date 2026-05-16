# 多线程、单一 Reactor 架构 Web Server 设计

在这一版本，我们引入了线程池，负责业务逻辑的处理，真正开始接触高并发

总体上看，主线程处理 epoll 触发，判断事件类型，完成 readbuffer 的准备，分配任务线程；任务线程则需要处理业务逻辑：解析 readbuffer 并将结果写入 writebuffer，并通知主线程再次和 sck 交互；主线程完成数据发送

buffer 就是主线程和任务线程的功能分界线。

其中实现的重点有：

- 任务线程的分发：connection 如何和 server 内部的线程池通信，进行任务函数的传递

- 任务完成的通知：我们引入了 eventfd ，当任务线程任务完成后，将善后函数打包发送到 eventloop 善后队列，由 eventloop 进行调用，处理 IO

- IO: 任务线程将 writebuffer 填充后，主线程先尝试发送数据，如果发送成功则结束，如果发送失败，例如内核缓冲区已满，则设置 EPOLLOUT，由channel 回调再次尝试 IO

- 完善 Channel 和 Connection : 增加 enableWriting( ) 和 handleWriting（ ），实现对输出的 epoll 控制 


## Q & A
1.为什么用 buffer 分割主线程和任务线程的功能界限？

- 当前阶段，主架构依旧是单一 Reactor ,epoll 和 eventloop 只有一个，主线程负责 buffer 和 sck 的 IO 交互，任务线程负责处理 readbuffer 内容并写入到 writebuffer ，也即单纯的业务处理，完成后通知主线程并退出

- 如果让任务线程接触和 sck 的 IO 交互：多线程会竞争 epoll，导致主线程的 epoll_wait 产生严重的上下文切换和锁等待开销；破坏了非阻塞写的“优雅兜底”机制；“连接销毁竞态”：客户端意外断开连接，主线程复用 fd ，任务线程可能还在执行对原来 fd 对 IO

- epoll 相关的所有操作应当由主线程进行，单一 epoll 应当禁止 epoll 相关调用的并发

--------
2.为什么业务函数由 connection 持有，线程池由 server 持有？如何实现业务函数由任务线程执行？

- server 控制全局，管理线程池、connection、acceptort；根据单一职责原则，业务逻辑应该和 server 解耦

- 为了实现 server 对线程池的控制和 conncetion 对业务函数的封装，双方使用了一次双向打包：server 提供一个可以塞入业务函数的线程调用接口，本质还是 bind 到 server 的回调函数，只不过这个函数执行时会触发异步的线程并发；connection 持有这个来自 server 的函数，并传入自己的业务函数作为参数后执行；和上帝类的区别就在于回调函数不再是传递 connection 指针让 server 处理业务逻辑，而是传递业务处理函数

-----
3.为什么构造函数需要使用 reserve ？

- reserve 是预留空间,但是依旧需要emplace_back，resize 是直接改变元素个数，可以下标使用；
- vector 的扩容会涉及旧元素的移动，对于 thread 而言有性能损耗
- 预留空间，避免扩容

-------
4.为什么任务线程的 lambda 函数退出后，线程就销毁了？

- C++ 使用 lambda 函数包装线程执行，其底层和 linux 交互实际上是调用了一个代理执行函数，代理函数中调用了 lambda 函数，因此 lambda 函数退出后，代理函数执行到末尾，线程销毁，等待主线程 join 回收资源



--------
5.为什么能使用 while 就不使用递归？

- 不增加调用栈，避免极端条件下出现大量递归导致栈溢出