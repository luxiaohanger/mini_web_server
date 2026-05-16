#include "Server.h"

#include <functional>

#include "Acceptor.h"
#include "Connection.h"
#include "EventLoop.h"
#include "Socket.h"
#include "ThreadPool.h"
#include "error_solve.h"

Server::Server() {
    eloop = new EventLoop();
    threadpool = new ThreadPool();
}

Server::~Server() {
    delete eloop;
    for (int i = 0; i < Acceptors.size(); ++i) {
        delete Acceptors[i];
    }
    for (auto it = Connections.begin(); it != Connections.end(); ++it) {
        delete it->second;
    }
}

void Server::listenPort(int port) {
    for (int i = 0; i < Acceptors.size(); ++i) {
        if (port == Acceptors[i]->getPort()) return;
    }
    Acceptor* acceptor = new Acceptor(eloop, port);
    acceptor->setNewConnectionCallBack(
        std::bind(&Server::handleNewConnection, this, std::placeholders::_1));
    Acceptors.push_back(acceptor);
    acceptor->startListen();
}

void Server::startLoop() { eloop->loop(); }

void Server::handleNewConnection(Socket* sck) {
    Connection* connection = new Connection(eloop, sck);
    Connections[sck] = connection;
    connection->setProcess([this](std::function<void()> task) {
        this->threadpool->enqueue(task);
    });
    connection->setDeleteConnectionCallBack(std::bind(
        &Server::handleDeleteConnection, this, std::placeholders::_1));
    connection->startConnect();
}

void Server::handleDeleteConnection(Socket* sck) {
    auto connection = Connections[sck];
    Connections.erase(sck);
    delete connection;
}
