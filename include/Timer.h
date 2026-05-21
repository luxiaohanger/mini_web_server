#pragma once
#include <functional>

// 逻辑计时器，仅供 TimerQueue 调用
// 和 timerId 一一对应
class Timer {
   private:
    int timerId;

    // 基于单调时钟的绝对过期时间 ts
    struct timespec expiration;

    // 计时器触发回调
    std::function<void()> callBack;

   public:
    Timer(int id, struct timespec ex, std::function<void()> cb);
    ~Timer();
    struct timespec getExpiration() { return expiration; }
    void runCallBack() { callBack(); }
};
