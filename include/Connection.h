#pragma once
#include <functional>
class Socket;
class EventLoop;
class Channel;

class Connection {
   private:
    EventLoop* eloop;
    Socket* sck;
    Channel* channel;
    std::function<void(int)> deleteConnectionCallBack;
    void handleReadCallBack();

   public:
    Connection(EventLoop* eloop, Socket* sck);
    ~Connection();
    void setDeleteConnectionCallBack(std::function<void(int)> cb) {
        deleteConnectionCallBack = std::move(cb);
    }

    void startConnect();
};
