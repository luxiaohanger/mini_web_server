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
    bool useThreadPool;
    std::thread eloopThread;
    std::unique_ptr<EventLoop> eloop;
    std::unordered_map<Socket*, std::shared_ptr<Connection>> Connections;

   public:
    SubReactor(bool f);
    ~SubReactor();

    // 新连接添加接口
    // 使用 enqueueTask 线程间通信
    void addConnection(Socket* sck,
                       std::function<void(std::function<void()>)> taskSubmit);

    void addConnection(Socket*);

    void start();
    // 控制线程向sub线程发送停止信号
    void stop();
};
