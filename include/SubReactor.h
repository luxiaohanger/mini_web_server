#pragma once

#include <functional>
#include <memory>
#include <thread>
#include <unordered_map>

class EventLoop;
class Connection;
class Socket;

class SubReactor {
   private:
    std::thread eloopThread;
    std::unique_ptr<EventLoop> eloop;
    std::unordered_map<Socket*, std::shared_ptr<Connection>> Connections;

    // 无需 delete ，直接移除指针
    // 引用计数减少
    // Callback 也调用此函数
    void removeConnection(Socket* sck);

   public:
    SubReactor();
    ~SubReactor();

    // 将添加连接投递eloop任务队列，在线程loop处理
    // 如果直接处理，就变成在主线程操作了
    // 为了让向 conn 注入线程调度接口，作为参数进行传递
    void addConnection(Socket* sck,
                       std::function<void(std::function<void()>)> taskSubmit);

    void start();
    // 主线程向sub线程发送停止信号
    void stop();
};
