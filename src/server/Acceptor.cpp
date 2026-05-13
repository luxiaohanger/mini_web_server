#include "Acceptor.h"

#include "Channel.h"
#include "Socket.h"
#include "error_solve.h"

Acceptor::Acceptor(EventLoop* eloop, int port) : eloop(eloop), port(port) {
    listenSck = new Socket(port);
    // 创建 channel 并挂载
    // 先挂载、再监听，防止空隙发生事件
    // LT 模式不会发生以上问题
    listenFd = listenSck->getFd();
    acceptChannel = new Channel(eloop, listenFd);
    acceptChannel->setReadCallBack(std::bind(&Acceptor::handleRead, this));
    acceptChannel->enableListen();
}

Acceptor::~Acceptor() {
    if (listenSck) delete listenSck;
    listenSck = nullptr;
    if (acceptChannel) delete acceptChannel;
    acceptChannel = nullptr;
}

void Acceptor::startListen() {
    errif(listen(listenFd, 128) == -1, "socket listen error");
}

void Acceptor::handleRead() {
    struct sockaddr_in clnt_addr{};
    socklen_t clnt_addr_len = sizeof(clnt_addr);
    int clnt_sockfd = accept(listenFd, (sockaddr*)&clnt_addr, &clnt_addr_len);
    errif(clnt_sockfd == -1, "socket accept error");
    Socket* sck = new Socket(clnt_sockfd, port, clnt_addr);
    newConnectionCallBack(sck);
}