#include <iostream>
// Linux 系统编程基础
#include <fcntl.h>  // 提供 fcntl() (用于设置非阻塞 IO)
#include <sys/epoll.h>
#include <sys/socket.h>  // 提供 socket(), bind(), listen(), accept(), setsockopt()
#include <sys/types.h>
#include <unistd.h>  // 提供 close(), read(), write()
// 网络地址转换
#include <arpa/inet.h>   // 提供 inet_ntop(), inet_pton(), htons()
#include <netinet/in.h>  // 提供 struct sockaddr_in

// 错误处理与日志
#include <errno.h>   // 提供 errno 变量
#include <string.h>  // 提供 strerror()

// include/头文件
#include "Epoll.h"
#include "Server.h"
#include "error_solve.h"

Server::Server() {
    std::cout << "Server construct!\n";
    ep = new Epoll();
}

Server::~Server() {
    for (auto it = listen_set.begin(); it != listen_set.end(); ++it) {
        if (is_valid_fd(*it)) close(*it);
    }
    delete ep;
}

void Server::mylisten(int port) {
    int listen_fd = socket(PF_INET, SOCK_STREAM, 0);
    errif(listen_fd == -1, "socket create error");

    listen_set.insert(listen_fd);

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;          // 地址族
    addr.sin_addr.s_addr = INADDR_ANY;  // 监听所有网卡（0.0.0.0）
    addr.sin_port = htons(port);        // htons 是为了处理大端/小端字节序转换

    errif(bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) == -1,
          "socket bind error");

    errif(listen(listen_fd, 128) == -1, "socket listen error");
    ep->addEvent(listen_fd);
}

void Server::startIO() { ep->solveEvent(*this); }
