#include "MainReactor.h"

#include "Acceptor.h"
#include "EventLoop.h"
#include "error_solve.h"

MainReactor::MainReactor() { eloop = std::make_unique<EventLoop>(); }

MainReactor::~MainReactor() {}

void MainReactor::listenPort(int port) {
    eloop->enqueueTask([this, port]() {
        if (Acceptors.find(port) != Acceptors.end()) return;
        auto acceptor = std::make_unique<Acceptor>(eloop.get(), port);
        acceptor->setNewConnectionCallBack(
            [this](Socket* sck) { newConnectionCallback(sck); });
        Acceptors[port] = std::move(acceptor);
        Acceptors[port]->startListen();
    });
}

void MainReactor::start() {
    eloopThread = std::thread([this]() { eloop->loop(); });
}

void MainReactor::stop() {
    eloop->enqueueTask([this]() {
        while (!Acceptors.empty()) {
            auto it = Acceptors.begin();
            Acceptors.erase(it->first);
        }
        eloop->stopLoop();
    });

    // 等待循环退出，回收 sub 线程
    if (eloopThread.joinable()) eloopThread.join();
}