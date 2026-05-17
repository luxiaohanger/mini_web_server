#pragma once
#include <cstdint>
#include <functional>

class EventLoop;

// 描述 fd 状态，但不持有 fd
class Channel {
   private:
    EventLoop* eloop;
    int fd;

    // 希望监听这个文件描述符的哪些事件
    uint32_t events;

    // 在epoll返回该Channel时文件描述符正在发生的事件
    uint32_t revents;

    // 区别新fd和旧fd
    bool inEpoll;

    std::function<void()> readCallBack;
    std::function<void()> writeCallBack;

    void remove();

   public:
    Channel(EventLoop* eloop, int fd);
    ~Channel();

    // 设置/创建 监听文件可读、ET触发
    // （新channel）挂载到 epoll
    void enableReading();

    void enableWriting();
    void disableWriting();

    // 设置文件可监听，并且为 LT 模式
    // 仅对新创建的未挂载 channel 可用
    // 如果设置 ET，一次 acceptor 的 handleRead 就要无限循环
    void enableListen();

    void disableAll();

    int getFd() { return fd; }
    uint32_t getEvents() { return events; }
    bool getInEpoll() { return inEpoll; }

    // 把wait返回新事件写入拷贝channel，准备处理
    void setRevents(uint32_t revents) { this->revents = revents; }
    void changeInEpoll(bool status) { inEpoll = status; }

    // 根据 revents 处理事件
    void handle();

    // 提供给上层类（acceptor、connection）
    // 用于注册channel时设置回调函数
    // 根据不同的设置者实现伪多态
    // 从而channel本身无需关心自己的来源
    void setReadCallBack(std::function<void()> cb) {
        this->readCallBack = std::move(cb);
    }

    void setWriteCallBack(std::function<void()> cb) {
        this->writeCallBack = std::move(cb);
    }
};