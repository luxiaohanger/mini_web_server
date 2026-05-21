#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>

class TimerQueue;
class Epoll;
class Channel;
class EventLoop {
   private:
    std::unique_ptr<Epoll> ep;
    // 负责通知loop的fd
    int eloopFd;
    // 包装eloop_fd
    std::unique_ptr<Channel> eloopChannel;
    std::unique_ptr<TimerQueue> timerQueue;
    std::mutex queue_mtx;

    // 等待被调用的善后函数
    std::queue<std::function<void()>> tasks;

    // worker 注册善后函数时发生竞态
    std::atomic<bool> stop;

    // 必须设置读回调并读取fd缓冲区全部数据
    // ET触发的原理不是缓冲区数据有变化
    // 而是缓冲区数据有无的状态切换
    void readCallback();
    bool isEloopChannel(Channel* c);

   public:
    EventLoop();
    ~EventLoop();
    void loop();
    void updateChannel(Channel* channel);
    void enqueueTask(std::function<void()> func);
    void removeChannel(Channel* channel);
    void stopLoop();
    int addTimer(std::function<void()> cb);
    void deleteTimer(int id);
};
