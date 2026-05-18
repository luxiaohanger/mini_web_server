#include "Acceptor.h"

#include "Channel.h"
#include "Socket.h"
#include "error_solve.h"

Acceptor::Acceptor(EventLoop* eloop, int port) : eloop(eloop), port(port) {
    listenSck = new Socket(port);
    // 创建 channel 并挂载
    // 先挂载、再监听，防止空隙发生事件
    // LT 模式不会发生以上问题
    acceptChannel = new Channel(eloop, listenSck->getFd());
    acceptChannel->setReadCallBack(std::bind(&Acceptor::handleRead, this));
    acceptChannel->enableListen();
}

Acceptor::~Acceptor() {
    delete acceptChannel;
    delete listenSck;
}

void Acceptor::startListen() { listenSck->startListen(); }

void Acceptor::handleRead() {
    auto res = listenSck->acceptConnection();
    if (res) newConnectionCallBack(res);
}