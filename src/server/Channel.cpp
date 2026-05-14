#include "Channel.h"

#include <sys/epoll.h>

#include <iostream>

#include "EventLoop.h"
#include "error_solve.h"

#define READ_BUFFER 1024

Channel::Channel(EventLoop* eloop, int fd)
    : eloop(eloop), fd(fd), events(0), revents(0), inEpoll(false) {}

void Channel::enableReading() {
    events |= EPOLLIN | EPOLLET;
    eloop->updateChannel(this);
}

void Channel::enableListen() {
    errif(inEpoll, "set inEpoll channel to be listen");
    events = EPOLLIN;
    eloop->updateChannel(this);
}

Channel::~Channel() {}

void Channel::handle() {
    if (this->revents & EPOLLIN) {
        readCallBack();
    } else {
        //
    }
}