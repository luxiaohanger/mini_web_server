#pragma once
#define MAX_EVENTS 100
#include <sys/epoll.h>

#include <vector>

#include "Channel.h"
#include "Server.h"

class Epoll {
   private:
    int epfd;
    epoll_event events[MAX_EVENTS], ev;

   public:
    Epoll();
    ~Epoll();

    // 获取事件 channel 列表
    std::vector<Channel*> poll();

    // 同步 channel 的最新信息，更新epoll
    // 挂载新channel
    void updateChannel(Channel* channel);
};