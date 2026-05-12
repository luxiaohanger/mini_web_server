#include "Channel.h"

#include <sys/socket.h>  // 提供 socket(), bind(), listen(), accept(), setsockopt()
#include <sys/types.h>
#include <unistd.h>  // 提供 close(), read(), write()

#include <iostream>

#include "Epoll.h"
#include "error_solve.h"
// 网络地址转换
#include <arpa/inet.h>   // 提供 inet_ntop(), inet_pton(), htons()
#include <netinet/in.h>  // 提供 struct sockaddr_in

// 错误处理与日志
#include <errno.h>   // 提供 errno 变量
#include <string.h>  // 提供 strerror()
#define READ_BUFFER 1024
Channel::Channel(Epoll* ep, int fd)
    : ep(ep), fd(fd), events(0), revents(0), inEpoll(false), isListen(false) {}

void Channel::enableReading() {
    events = EPOLLIN | EPOLLET;
    ep->updateChannel(this);
}

void Channel::addListen(int port) {
    isListen = true;
    int listen_fd = socket(PF_INET, SOCK_STREAM, 0);
    errif(listen_fd == -1, "socket create error");

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;          // 地址族
    addr.sin_addr.s_addr = INADDR_ANY;  // 监听所有网卡（0.0.0.0）
    addr.sin_port = htons(port);        // htons 是为了处理大端/小端字节序转换

    errif(bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) == -1,
          "socket bind error");

    errif(listen(listen_fd, 128) == -1, "socket listen error");

    this->fd = listen_fd;
    events = EPOLLIN | EPOLLET;
    ep->updateChannel(this);
}

void Channel::handle() {
    if (isListen) {
        // 新连接
        // 加载 clnt_fd
        struct sockaddr_in clnt_addr;
        socklen_t clnt_addr_len = sizeof(clnt_addr);
        bzero(&clnt_addr, sizeof(clnt_addr));
        int clnt_sockfd =
            accept(this->fd, (sockaddr*)&clnt_addr, &clnt_addr_len);
        errif(clnt_sockfd == -1, "socket accept error");
        std::cout << "new client accept!\n";

        // 加载新的channel，使用 enableReading 挂载
        Channel* channel = new Channel(this->ep, clnt_sockfd);
        channel->enableReading();
    } else if (this->revents & EPOLLIN) {
        // 可读事件
        // 在 epoll 的 ET
        // 模式下，内核只会在缓冲区状态发生“变化”时通知你一次。
        // 通过“死循环”确保将内核接收缓冲区中的数据彻底抽干 (Drain)
        char buf[READ_BUFFER];
        int clnt_sockfd = this->fd;
        while (true) {
            memset(buf, 0, sizeof(buf));
            ssize_t read_byte = read(clnt_sockfd, buf, sizeof(buf));
            if (read_byte > 0) {
                std::cout << "msg from client " << clnt_sockfd << " : " << buf
                          << '\n';
                write(clnt_sockfd, buf, read_byte);
            } else if (read_byte == -1 && errno == EINTR) {
                // 被信号中断，继续读取
                std::cout << "interrupted! continue.\n";
                continue;
            } else if (read_byte == -1 &&
                       ((errno == EAGAIN) || (errno == EWOULDBLOCK))) {
                // 非阻塞IO中，这个条件表示数据全部读取完毕
                std::cout << "read OK!\n";
                break;
            } else if (read_byte == 0) {  // EOF，客户端断开连接
                std::cout << "EOF\n";
                // 关闭socket会自动将文件描述符从epoll树上移除
                close(clnt_sockfd);
                break;
            }
        }
    } else {
        //
    }
}