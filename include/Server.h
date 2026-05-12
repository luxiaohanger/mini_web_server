#pragma once
#include <unordered_set>

#include "Server.h"
class EventLoop;
class Epoll;

class Server {
   private:
    std::unordered_set<int> listen_set;
    EventLoop* eloop;
    Epoll* ep;

   public:
    Server();
    ~Server();

    // listen the target port
    // add channel to epoll
    void listenPort(int port);
    void startLoop();
};