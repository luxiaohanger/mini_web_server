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
    std::function<void(Socket*)> deleteConnectionCallBack;
    void handleReadCallBack();

   public:
    Connection(EventLoop* eloop, Socket* sck);
    ~Connection();
    void setDeleteConnectionCallBack(std::function<void(Socket*)> cb) {
        deleteConnectionCallBack = std::move(cb);
    }

    void startConnect();
};
