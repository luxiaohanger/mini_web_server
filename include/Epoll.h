#pragma once
#define MAX_EVENTS 100
#include "Epoll.h"
#include "Server.h"
class Epoll {
   private:
    int epfd;
    epoll_event events[MAX_EVENTS], ev;

   public:
    Epoll(/* args */);
    ~Epoll();
    void addEvent(int fd);
    void solveEvent(Server& s);
};