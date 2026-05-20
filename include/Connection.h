#pragma once
#include <atomic>
#include <functional>
#include <memory>

class Socket;
class EventLoop;
class Channel;
class Buffer;
class HttpProcess;

enum class ConnState { connected, peerClose, dead };

class Connection : public std::enable_shared_from_this<Connection> {
   private:
    EventLoop* eloop;
    std::unique_ptr<Socket> sck;
    std::unique_ptr<Channel> channel;
    std::unique_ptr<Buffer> readBuffer;
    std::unique_ptr<Buffer> writeBuffer;
    std::unique_ptr<HttpProcess> httpProcess_;

    // 由于conn 和 sck 生命周期绑定，因此不会出现 fd 复用导致输出错误
    // 根据严格的tcp语义和socket特性，读到EOF不代表不能写
    // 我们实现的语义为：
    // EOF之前保证所有读入数据正确处理并填入buffer
    // 尝试写入 fd 直到写错误
    // 记录 conn 状态 ：连接、peer 已关闭、完全断开
    // 状态为 dead 时触发 remove 回调
    ConnState state;
    // 计数工作线程
    int working;
    // state == ConnState::peerClose && working == 0
    // 表示不会有新worker且worker全部结束

    // 使用共享指针，无需 delete，而是移除在此处的指针
    std::function<void(Socket*)> removeConnectionCallBack;
    std::function<void(std::function<void()>)> process;

    void handleReadCallBack();
    void handleWriteCallBack();
    void readFromSck();

    // 尝试将 writebuffer 写入 sck
    // 失败则添加 EPOLLOUT
    void trySendToSck();

    // 写回sck时检测到写出错
    // 连接完全断开
    // 从 subreactor 移除
    void handleDead();

    void onHttp();
    void sendHttpOnLoop(const std::string& resp, bool keepAlive);
    void checkEmptyReadAfterEof();

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
