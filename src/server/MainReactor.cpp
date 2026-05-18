#include "MainReactor.h"

#include "Acceptor.h"
#include "EventLoop.h"
#include "error_solve.h"

MainReactor::MainReactor() { eloop = new EventLoop(); }

MainReactor::~MainReactor() {
    for (auto it : Acceptors) delete it.second;
    Acceptors.clear();
    eloop->stopLoop();
    delete eloop;
}

void MainReactor::listenPort(int port) {
    if (Acceptors.find(port) != Acceptors.end()) return;
    Acceptor* acceptor = new Acceptor(eloop, port);
    acceptor->setNewConnectionCallBack(
        [this](Socket* sck) { newConnectionCallback(sck); });
    Acceptors[port] = acceptor;
    acceptor->startListen();
}

void MainReactor::start() { eloop->loop(); }