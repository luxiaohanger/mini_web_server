#pragma once
#include <cstdint>

class Epoll;

class Channel {
   private:
    Epoll* ep;
    int fd;

    // 希望监听这个文件描述符的哪些事件
    uint32_t events;

    // 在epoll返回该Channel时文件描述符正在发生的事件
    uint32_t revents;

    // 区别新fd和旧fd
    bool inEpoll;

    // 标识监听fd
    bool isListen;

   public:
    Channel(Epoll* ep, int fd);
    ~Channel();

    // 设置/创建 监听文件可读、ET触发
    // （新channel）挂载到 epoll
    void enableReading();

    // 新建监听 fd,监听 port 端口
    // 挂载到 epoll
    void addListen(int port);

    int getFd() { return fd; }
    uint32_t getEvents() { return events; }
    bool getInEpoll() { return inEpoll; }

    // 把wait返回新事件写入拷贝channel，准备处理
    void setRevents(uint32_t revents) { this->revents = revents; }
    void setInEpoll() { inEpoll = true; }

    // 根据 revents 处理事件
    void handle();
};