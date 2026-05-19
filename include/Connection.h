#pragma once
#include <atomic>
#include <functional>
#include <memory>

class Socket;
class EventLoop;
class Channel;
class Buffer;

class Connection : public std::enable_shared_from_this<Connection> {
   private:
    EventLoop* eloop;
    std::unique_ptr<Socket> sck;
    std::unique_ptr<Channel> channel;
    std::unique_ptr<Buffer> readBuffer;
    std::unique_ptr<Buffer> writeBuffer;

    // 由于conn 和 sck 生命周期绑定，因此不会出现 fd 复用导致输出错误
    // 根据严格的tcp语义和socket特性，读到EOF不代表不能写
    // 因此不需要状态标记
    // 我们实现的语义为：
    // EOF之前保证所有读入数据正确处理并填入buffer
    // 尝试写入 fd 直到写错误
    // bool alive;

    // 使用共享指针，无需 delete，而是移除在此处的指针
    std::function<void(Socket*)> removeConnectionCallBack;
    std::function<void(std::function<void()>)> process;

    void handleReadCallBack();
    void handleWriteCallBack();
    void Echo();
    void processEcho(const std::string& data, std::string& res);
    void readFromSck();

    // 尝试将 writebuffer 写入 sck
    // 失败则添加 EPOLLOUT
    void trySendToSck();
    // 在 IO 中首次检测到连接断开
    // 播报通知，并回调移除 ptr
    // 后续只判断，不播报
    void handleDead();

   public:
    Connection(EventLoop* eloop, std::unique_ptr<Socket> sck);
    ~Connection();

    void setRemoveConnectionCallBack(std::function<void(Socket*)> cb) {
        removeConnectionCallBack = std::move(cb);
    }

    void setProcess(std::function<void(std::function<void()>)> process) {
        this->process = std::move(process);
    }

    void startConnect();
    // 通知断开连接
    void stop();
};
