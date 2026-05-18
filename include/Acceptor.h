#pragma once
#include <functional>

class EventLoop;
class Channel;
class Socket;
class Acceptor {
   private:
    int port;
    Socket* listenSck;
    EventLoop* eloop;
    Channel* acceptChannel;  // 包装 ListenFD 的 Channel

    // 回调函数顺便传入对方地址，减少额外的系统调用
    std::function<void(Socket*)> newConnectionCallBack;
    // 当 Channel 发现 ListenFD 可读时
    // 在其中执行 callback
    void handleRead();

   public:
    Acceptor(EventLoop* loop, int port);
    ~Acceptor();

    // 关键：供上层 Server 调用，用来注入“新连接处理逻辑”
    void setNewConnectionCallBack(std::function<void(Socket*)> cb) {
        newConnectionCallBack = std::move(cb);
    }

    // 分离构造函数和启动监听
    // 在所有“插件”（回调函数）安装好之前，不要启动引擎
    // 避免server 创建 acceptor后、设置回调函数前，epoll 事件触发
    void startListen();
    int getPort() { return port; }
};