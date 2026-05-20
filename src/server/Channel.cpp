#include "Channel.h"

#include <sys/epoll.h>

#include "EventLoop.h"
#include "error_solve.h"

Channel::Channel(EventLoop* eloop, int fd)
    : eloop(eloop), fd(fd), events(0), revents(0), inEpoll(false) {}

void Channel::enableReading() {
    events |= EPOLLIN | EPOLLET;
    eloop->updateChannel(this);
}

void Channel::disableReading() {
    events &= ~EPOLLIN;
    eloop->updateChannel(this);
}

void Channel::enableWriting() {
    events |= EPOLLOUT;
    eloop->updateChannel(this);
}

void Channel::disableWriting() {
    events &= ~EPOLLOUT;
    eloop->updateChannel(this);
}

void Channel::enableListen() {
    errif(inEpoll, "set inEpoll channel to be listen");
    events = EPOLLIN;
    eloop->updateChannel(this);
}

void Channel::disableAll() {
    events = 0;
    eloop->updateChannel(this);
}

void Channel::remove() { eloop->removeChannel(this); }

Channel::~Channel() { remove(); }

void Channel::handle() {
    // 每个事件都判断，可能同时存在

    if (this->revents & EPOLLIN) readCallBack();

    if (this->revents & EPOLLOUT) writeCallBack();
}