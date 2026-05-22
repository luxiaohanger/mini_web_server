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

    std::function<void(Socket*)> removeConnectionCallBack;
    std::function<void(std::function<void()>)> process;

    int timerId;
    std::function<int()> addTimerCallBack;
    std::function<void(int)> deleteTimerCallBack;
    // peer 产生交互，刷新 timer
    void refreshTimer();

    void handleReadCallBack();
    void handleWriteCallBack();
    void readFromSck();

    // 尝试将 writebuffer 写入 sck
    // 失败则添加 EPOLLOUT
    void trySendToSck();

    void onHttp();
    void sendHttpOnLoop(const std::string& resp, bool keepAlive);
    void checkEmptyReadAfterEof();

   public:
    Connection(EventLoop* eloop, std::unique_ptr<Socket> sck);
    ~Connection();

    void setProcess(std::function<void(std::function<void()>)> process) {
        this->process = std::move(process);
    }

    void setRemoveConnectionCallBack(std::function<void(Socket*)> cb) {
        removeConnectionCallBack = std::move(cb);
    }

    void setAddTimerCallBack(std::function<int()> cb) {
        addTimerCallBack = std::move(cb);
    }

    void setDeleteTimerCallBack(std::function<void(int)> cb) {
        deleteTimerCallBack = std::move(cb);
    }

    void startConnect();
    // 断开连接,修改状态
    // 用于线程关闭，独立于 handleDead
    void stop();

    // 不再单独使用 remove 回调，而是统一使用dead处理
    // 内部注册延迟自毁任务
    // 并对外 public
    // 保证连接延迟关闭语义统一
    void handleDead();
};
