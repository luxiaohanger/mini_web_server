#include "EventLoop.h"

#include <sys/eventfd.h>
#include <unistd.h>

#include "Channel.h"
#include "Epoll.h"
#include "error_solve.h"

EventLoop::EventLoop() : stop(false) {
    eloopFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    errif(eloopFd < 0, "eloopFd create error");
    ep = std::make_unique<Epoll>();
    eloopChannel = std::make_unique<Channel>(this, eloopFd);
    eloopChannel->setReadCallBack([this]() { this->readCallback(); });
    eloopChannel->enableReading();
}

EventLoop::~EventLoop() {
    eloopChannel.reset();
    ep.reset();
    ::close(eloopFd);
}

void EventLoop::loop() {
    while (!stop) {
        auto channels = ep->poll();
        for (auto channel : channels) {
            channel->handle();
        }

        // 为了避免在持有锁时调用善后函数，先用栈空间存储
        std::vector<std::function<void()>> funcs;
        {
            std::unique_lock<std::mutex> lock(queue_mtx);

            while (!tasks.empty()) {
                funcs.emplace_back(std::move(tasks.front()));
                tasks.pop();
            }
        }

        for (auto& it : funcs) it();
    }
}

void EventLoop::updateChannel(Channel* channel) { ep->updateChannel(channel); }

void EventLoop::readCallback() {
    uint64_t buf = 0;
    //  eloopChannel 是 ET 模式，必须用 while 循环榨干至 EAGAIN
    while (true) {
        auto n = ::read(eloopFd, &buf, sizeof(buf));
        if (n == sizeof(buf)) {
            break;  // 成功清除内核计数器，安全退出
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // 榨干了
            if (errno == EINTR) continue;  // 信号中断，继续
            errif(true, "eloopFd read error");
        }
    }
}

void EventLoop::enqueueTask(std::function<void()> func) {
    if (stop) return;
    {
        std::unique_lock<std::mutex> lock(queue_mtx);
        tasks.push(std::move(func));
    }
    uint64_t one = 1;
    auto res = ::write(eloopFd, &one, sizeof(one));
    errif(res < 0, "eloopFd write error");
}

void EventLoop::removeChannel(Channel* channel) { ep->removeChannel(channel); }