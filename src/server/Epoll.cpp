#include <iostream>

// 提供 memset()
#include <string.h>

// include/头文件
#include "Epoll.h"
#include "Server.h"
#include "error_solve.h"

Epoll::Epoll() {
    std::cout << "Epoll construct!\n";
    this->epfd = epoll_create1(0);
    errif(this->epfd == -1, "epoll create error");
    memset(events, 0, sizeof(events));
}

Epoll::~Epoll() {}

void Epoll::updateChannel(Channel* channel) {
    int fd = channel->getFd();
    memset(&ev, 0, sizeof(ev));
    ev.data.ptr = channel;
    ev.events = channel->getEvents();
    setnonblocking(fd);
    if (!channel->getInEpoll()) {
        errif(epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) == -1, "epoll add error");
        channel->setInEpoll();
    } else {
        errif(epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev) == -1,
              "epoll modify error");
    }
}

std::vector<Channel*> Epoll::poll() {
    std::vector<Channel*> ans;
    int nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);
    errif(nfds == -1, "epoll wait error");
    for (int i = 0; i < nfds; ++i) {
        Channel* channel = static_cast<Channel*>(events[i].data.ptr);
        channel->setRevents(events[i].events);
        ans.push_back(channel);
    }
    return ans;
}