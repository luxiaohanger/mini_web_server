#pragma once
#include <functional>
#include <unordered_map>

class EventLoop;
class Socket;
class Acceptor;

class MainReactor {
   private:
    EventLoop* eloop;
    std::unordered_map<int, Acceptor*> Acceptors;

    std::function<void(Socket* sck)> newConnectionCallback;

   public:
    MainReactor();
    ~MainReactor();
    void setNewConnectionCallback(std::function<void(Socket* sck)> cb) {
        newConnectionCallback = std::move(cb);
    }
    void listenPort(int port);
    void start();
};
