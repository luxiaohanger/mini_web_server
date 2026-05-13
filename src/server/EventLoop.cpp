#include "EventLoop.h"

#include "Channel.h"
#include "Epoll.h"

EventLoop::EventLoop() { ep = new Epoll(); }
EventLoop::~EventLoop() {
    if (ep) delete ep;
    ep = nullptr;
}

void EventLoop::loop() {
    while (true) {
        auto channels = ep->poll();
        for (auto channel : channels) {
            channel->handle();
        }
    }
}

void EventLoop::updateChannel(Channel* channel) { ep->updateChannel(channel); }