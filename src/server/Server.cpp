#include <sys/socket.h>  // 提供 socket(), bind(), listen(), accept(), setsockopt()
#include <unistd.h>      // 提供 close(), read(), write()

#include <iostream>

// 错误处理与日志
#include <errno.h>   // 提供 errno 变量
#include <string.h>  // 提供 strerror()

// include/头文件
#include "Channel.h"
#include "Epoll.h"
#include "EventLoop.h"
#include "Server.h"
#include "error_solve.h"

Server::Server() {
    std::cout << "Server construct!\n";
    ep = new Epoll();
    eloop = new EventLoop(ep);
}

Server::~Server() {
    for (auto it = listen_set.begin(); it != listen_set.end(); ++it) {
        if (is_valid_fd(*it)) close(*it);
    }
    if (ep) delete ep;
    ep = nullptr;
    if (eloop) delete eloop;
    eloop = nullptr;
}

void Server::listenPort(int port) {
    Channel* channel = new Channel(ep, 0);
    channel->addListen(port);
}

void Server::startLoop() { eloop->loop(); }
