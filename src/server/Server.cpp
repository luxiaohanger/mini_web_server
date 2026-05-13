#include "Server.h"

#include <functional>

#include "Acceptor.h"
#include "Connection.h"
#include "EventLoop.h"
#include "Socket.h"
#include "error_solve.h"

Server::Server() { eloop = new EventLoop(); }

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
    Connections[sck->getFd()] = connection;
    connection->setDeleteConnectionCallBack(std::bind(
        &Server::handleDeleteConnection, this, std::placeholders::_1));
    connection->startConnect();
}

void Server::handleDeleteConnection(int fd) {
    auto connection = Connections[fd];
    Connections.erase(fd);
    delete connection;
}
