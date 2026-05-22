#pragma once
#include <functional>
#include <memory>
#include <thread>
#include <unordered_map>

class EventLoop;
class Socket;
class Acceptor;

class MainReactor {
   private:
    std::thread eloopThread;
    std::unique_ptr<EventLoop> eloop;
    std::unordered_map<int, std::unique_ptr<Acceptor>> Acceptors;
    std::function<void(Socket*)> newConnectionCallback;

   public:
    MainReactor();
    ~MainReactor();
    void setNewConnectionCallback(std::function<void(Socket*)> cb) {
        newConnectionCallback = std::move(cb);
    }
    // 向 server 控制线程提供监听端口调用
    // equeueTask 线程间通信
    void listenPort(int port);
    void start();
    void stop();
};
