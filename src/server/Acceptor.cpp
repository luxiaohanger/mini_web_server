#include "Acceptor.h"

#include "Channel.h"
#include "Socket.h"
#include "error_solve.h"

Acceptor::Acceptor(EventLoop* eloop, int port) : eloop(eloop), port(port) {
    listenSck = std::make_unique<Socket>(port);
    // 创建 channel 并挂载
    // 先挂载、再监听，防止空隙发生事件
    // LT 模式不会发生以上问题
    acceptChannel = std::make_unique<Channel>(eloop, listenSck->getFd());
    acceptChannel->setReadCallBack([this]() { this->handleRead(); });
    acceptChannel->enableListen();
}

Acceptor::~Acceptor() {}

void Acceptor::startListen() { listenSck->startListen(); }

void Acceptor::handleRead() {
    auto res = listenSck->acceptConnection();
    if (res) newConnectionCallBack(res);
}