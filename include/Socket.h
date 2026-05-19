#pragma once
#include <arpa/inet.h>

#include <memory>

// fd 的持有者，封装裸露的 fd 和 socket 系统调用
// 上层类只持有 Socket 指针，并进行调用
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
    Socket(const Socket&) = delete;
    ~Socket();
    int getFd() { return fd; }
    int getPort() { return port; }
    auto getPeerAddr() { return peerAddr; }

    Socket* acceptConnection();
    void startListen();
    ssize_t sckRead(void* buf, size_t count);
    ssize_t sckWrite(const void* buf, size_t count);
};
