#pragma once
#include <arpa/inet.h>

class Socket {
   private:
    int fd;
    int port;  // local port
    struct sockaddr_in peerAddr;

   public:
    // 创建监听 socket , 绑定监听端口
    Socket(int port);

    // 创建 tcp socket ， 完善 peer 信息
    Socket(int fd, int port, struct sockaddr_in peerAddr);

    ~Socket();
    int getFd() { return fd; }
    int getPort() { return port; }
    auto getPeerAddr() { return peerAddr; }
};
