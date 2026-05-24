#include "SubReactor.h"

#include "Connection.h"
#include "EventLoop.h"
#include "Socket.h"
#include "error_solve.h"

SubReactor::SubReactor(bool f) : useThreadPool(f) {
    eloop = std::make_unique<EventLoop>();
}

SubReactor::~SubReactor() {}

void SubReactor::addConnection(
    Socket* sck, std::function<void(std::function<void()>)> taskSubmit) {
    eloop->enqueueTask([this, sck, taskSubmit]() {
        auto conn = std::make_shared<Connection>(
            eloop.get(), std::unique_ptr<Socket>(sck), useThreadPool);
        conn->setProcess(taskSubmit);
        conn->setRemoveConnectionCallBack(
            [this](Socket* sc) { Connections.erase(sc); });
        conn->setAddTimerCallBack([this, conn]() {
            return eloop->addTimer([conn]() { conn->handleDead(); });
        });
        conn->setDeleteTimerCallBack(
            [this](int id) { eloop->deleteTimer(id); });
        Connections[sck] = conn;
        conn->startConnect();
    });
}

void SubReactor::addConnection(Socket* sck) {
    eloop->enqueueTask([this, sck]() {
        auto conn = std::make_shared<Connection>(
            eloop.get(), std::unique_ptr<Socket>(sck), useThreadPool);
        conn->setRemoveConnectionCallBack(
            [this](Socket* sc) { Connections.erase(sc); });
        conn->setAddTimerCallBack([this, conn]() {
            return eloop->addTimer([conn]() { conn->handleDead(); });
        });
        conn->setDeleteTimerCallBack(
            [this](int id) { eloop->deleteTimer(id); });
        Connections[sck] = conn;
        conn->startConnect();
    });
}

void SubReactor::start() {
    // 所有在 loop 函数中处理的事件都运行在子线程上
    eloopThread = std::thread([this]() { eloop->loop(); });
}

void SubReactor::stop() {
    // conn 移除 channel，进入 dead state
    // 防止 working 回归后误 IO
    // 不保证善后函数全部完成
    // 注册循环退出信号
    // conn 资源回收由智能指针自动完成
    eloop->enqueueTask([this]() {
        for (auto it : Connections) {
            it.second->stop();
        }
        eloop->stopLoop();
    });

    // 等待循环退出，回收 sub 线程
    if (eloopThread.joinable()) eloopThread.join();
}