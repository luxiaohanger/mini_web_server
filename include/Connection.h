#pragma once
#include <functional>
class Socket;
class EventLoop;
class Channel;
class Buffer;

class Connection {
   private:
    EventLoop* eloop;
    Socket* sck;
    Channel* channel;
    Buffer* readBuffer;
    Buffer* writeBuffer;
    bool alive;
    std::function<void(Socket*)> deleteConnectionCallBack;
    std::function<void(std::function<void()>)> process;
    void handleReadCallBack();
    void handleWriteCallBack();
    void Echo();
    void processEcho();
    void readFromSck();

    // 尝试将 writebuffer 写入 sck
    // 失败则添加 EPOLLOUT
    void trySendToSck();

   public:
    Connection(EventLoop* eloop, Socket* sck);
    ~Connection();

    void setDeleteConnectionCallBack(std::function<void(Socket*)> cb) {
        deleteConnectionCallBack = std::move(cb);
    }

    void setProcess(std::function<void(std::function<void()>)> process) {
        this->process = std::move(process);
    }

    void startConnect();
};
