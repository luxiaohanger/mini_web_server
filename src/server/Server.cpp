#include "Server.h"

#include "Acceptor.h"
#include "MainReactor.h"
#include "SubReactor.h"
#include "ThreadPool.h"
#include "error_solve.h"

Server::Server(bool f) : useThreadPool(f), subIdx(0) {
    int n = std::thread::hardware_concurrency();
    if (n == 0) n = 4;
    if (useThreadPool) threadpool = std::make_unique<ThreadPool>(n);
    mainReactor = std::make_unique<MainReactor>();
    mainReactor->setNewConnectionCallback(
        [this](Socket* sck) { handleNewConnection(sck); });
    SubReactors.reserve(n);
    for (int i = 0; i < n; ++i) {
        SubReactors.emplace_back(std::make_unique<SubReactor>(useThreadPool));
    }
}

Server::~Server() {}

void Server::stop() {
    mainReactor->stop();
    for (int i = 0; i < SubReactors.size(); ++i) SubReactors[i]->stop();
    if (useThreadPool) threadpool->stop();
}

void Server::listenPort(int port) { mainReactor->listenPort(port); }

void Server::start() {
    for (int i = 0; i < SubReactors.size(); ++i) SubReactors[i]->start();
    mainReactor->start();
}

void Server::handleNewConnection(Socket* sck) {
    if (useThreadPool)
        SubReactors[subIdx]->addConnection(
            sck,
            [this](std::function<void()> f) { this->threadpool->enqueue(f); });
    else
        SubReactors[subIdx]->addConnection(sck);
    subIdx = (subIdx + 1) % SubReactors.size();
}
