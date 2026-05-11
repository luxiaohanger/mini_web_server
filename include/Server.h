#pragma once
#include <unordered_set>

#include "Server.h"
class Epoll;

class Server {
   private:
    Epoll* ep;
    std::unordered_set<int> listen_set;

   public:
    Server();
    ~Server();

    friend class Epoll;

    // listen the target port , add to epoll
    void mylisten(int port);
    void startIO();
};