#include "EventLoop.h"

#include "Channel.h"
#include "Epoll.h"

EventLoop::EventLoop(Epoll* ep) : ep(ep) {}
EventLoop::~EventLoop() {}

void EventLoop::loop() {
    while (true) {
        auto channels = ep->poll();
        for (auto channel : channels) {
            channel->handle();
        }
    }
}