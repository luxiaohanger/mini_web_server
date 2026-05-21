#include "TimerQueue.h"

#include <Channel.h>
#include <error_solve.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <vector>

#include "EventLoop.h"
#include "Timer.h"

// 返回绝对时间结构体 ts
struct timespec now() {
    struct timespec ts;
    errif(clock_gettime(CLOCK_MONOTONIC, &ts) < 0, "time get error");
    return ts;
}

TimerQueue::TimerQueue(EventLoop* eloop) : eloop(eloop), timerId(0) {
    timeFd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    errif(timeFd < 0, "timeFd create error");
    timeChannel = std::make_unique<Channel>(eloop, timeFd);
    timeChannel->setReadCallBack([this]() { this->handleRead(); });
    timeChannel->enableReading();
}

TimerQueue::~TimerQueue() { ::close(timeFd); }

void TimerQueue::handleRead() {
    // 先清空 fd 的内容
    uint64_t buf = 0;
    //  同 eloopFd 一致，ET 模式，必须用 while 循环榨干至 EAGAIN
    while (true) {
        auto n = ::read(timeFd, &buf, sizeof(buf));
        if (n == sizeof(buf)) {
            break;  // 成功清除内核计数器，进入处理逻辑
        }
        if (n < 0) {
            // 此处和 eloopFd 不太一样，代表没有到时间，直接退出
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR) continue;  // 信号中断，继续
            errif(true, "eloopFd read error");
        }
    }

    auto t = now();
    std::vector<int> dyingTimers;

    for (const auto& it : timers) {
        if (it.second->getExpiration().tv_sec < t.tv_sec ||
            (it.second->getExpiration().tv_sec == t.tv_sec &&
             it.second->getExpiration().tv_nsec < t.tv_nsec))
            dyingTimers.push_back(it.first);
        else
            break;
    }

    for (auto id : dyingTimers) {
        timers[id]->runCallBack();
        timers.erase(id);
    }

    refreshClock();
}

void TimerQueue::refreshClock() {
    if (timers.empty()) {
        errif(timerfd_settime(timeFd, TFD_TIMER_ABSTIME, nullptr, nullptr) < 0,
              "timeFd refresh error");
        return;
    }
    const auto& it = timers.begin()->second->getExpiration();
    struct itimerspec its;
    struct timespec ts{};
    its.it_interval = ts;
    ts = it;
    its.it_value = ts;

    errif(timerfd_settime(timeFd, TFD_TIMER_ABSTIME, &its, nullptr) < 0,
          "timeFd refresh error");
}

void TimerQueue::stop() { timeChannel->disableAll(); }

int TimerQueue::addTimer(std::function<void()> cb) {
    auto ex = now();
    ex.tv_sec += 60;  // 定时 60 s
    timerId++;
    auto timer = std::make_unique<Timer>(timerId, ex, std::move(cb));
    timers[timerId] = std::move(timer);
    refreshClock();
    return timerId;
}

void TimerQueue::deleteTimer(int id) {
    if (timers.find(id) == timers.end()) return;
    timers.erase(id);
    refreshClock();
    return;
}
