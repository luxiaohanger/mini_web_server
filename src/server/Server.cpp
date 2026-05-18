#include "Server.h"

#include "Acceptor.h"
#include "MainReactor.h"
#include "SubReactor.h"
#include "ThreadPool.h"
#include "error_solve.h"

Server::Server() : subIdx(0) {
    int n = std::thread::hardware_concurrency();
    if (n == 0) n = 4;
    threadpool = new ThreadPool(n);
    mainReactor = new MainReactor();
    mainReactor->setNewConnectionCallback(
        [this](Socket* sck) { handleNewConnection(sck); });
    SubReactors.reserve(n);
    for (int i = 0; i < n; ++i) {
        SubReactors.emplace_back(new SubReactor());
    }
}

Server::~Server() {
    delete mainReactor;
    for (auto it : SubReactors) it->stop();
    // 等待worker退出，防止注册函数访问越界
    delete threadpool;
    for (auto it : SubReactors) delete it;
}

void Server::listenPort(int port) { mainReactor->listenPort(port); }

void Server::start() {
    for (auto it : SubReactors) it->start();
    mainReactor->start();
}

void Server::handleNewConnection(Socket* sck) {
    SubReactors[subIdx]->addConnection(
        sck, [this](std::function<void()> f) { this->threadpool->enqueue(f); });
    subIdx = (subIdx + 1) % SubReactors.size();
}
