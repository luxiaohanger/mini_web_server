#pragma once
#include <time.h>

#include <functional>
#include <map>
#include <memory>

class EventLoop;
class Channel;
class Timer;

class TimerQueue {
   private:
    int timerId;
    EventLoop* eloop;
    int timeFd;
    std::unique_ptr<Channel> timeChannel;

    // 按id排序，id越大越晚触发
    std::map<int, std::unique_ptr<Timer>> timers;

    // 计时器被触发，说明可能有conn过期
    // 检查各timer的expiration
    // 对过期conn触发回调
    void handleRead();

    // 刷新定时器，以最近的timer为准
    // 用在 timermap 修改之后
    void refreshClock();

   public:
    TimerQueue(EventLoop* eloop);
    ~TimerQueue();
    void stop();

    // 给新连接添加计时器，callback 存放销毁回调
    // 返回新计时器序号
    int addTimer(std::function<void()> cb);

    // 删除旧 timer
    // 需要更新的，直接删除旧的，添加新的
    void deleteTimer(int id);
    // eloop用于判断channel类型
    bool isTimeChannel(Channel* c) { return timeChannel.get() == c; }
};
