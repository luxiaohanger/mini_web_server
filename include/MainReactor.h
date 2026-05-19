#pragma once
#include <functional>
#include <memory>
#include <unordered_map>

class EventLoop;
class Socket;
class Acceptor;

class MainReactor {
   private:
    std::unique_ptr<EventLoop> eloop;
    std::unordered_map<int, std::unique_ptr<Acceptor>> Acceptors;

    std::function<void(Socket*)> newConnectionCallback;

   public:
    MainReactor();
    ~MainReactor();
    void setNewConnectionCallback(std::function<void(Socket*)> cb) {
        newConnectionCallback = std::move(cb);
    }
    void listenPort(int port);
    void start();
};
