#include "MainReactor.h"

#include "Acceptor.h"
#include "EventLoop.h"
#include "error_solve.h"

MainReactor::MainReactor() { eloop = std::make_unique<EventLoop>(); }

MainReactor::~MainReactor() { eloop->stopLoop(); }

void MainReactor::listenPort(int port) {
    if (Acceptors.find(port) != Acceptors.end()) return;
    auto acceptor = std::make_unique<Acceptor>(eloop.get(), port);
    acceptor->setNewConnectionCallBack(
        [this](Socket* sck) { newConnectionCallback(sck); });
    Acceptors[port] = std::move(acceptor);
    Acceptors[port]->startListen();
}

void MainReactor::start() { eloop->loop(); }