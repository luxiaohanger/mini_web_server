#include "Socket.h"

#include <fcntl.h>
#include <unistd.h>

#include "error_solve.h"

void setNonBlocking(int fd) {
    // file control
    // 先 file get flag ,位掩码修改后 file set flag
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

Socket::Socket(int port) : port(port) {
    fd = socket(PF_INET, SOCK_STREAM, 0);
    peerAddr = {};
    errif(fd == -1, "socket create error");
    // 创建时设置非阻塞，防止多次系统调用
    setNonBlocking(this->fd);
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;          // 地址族
    addr.sin_addr.s_addr = INADDR_ANY;  // 监听所有网卡（0.0.0.0）
    addr.sin_port = htons(port);        // htons 是为了处理大端/小端字节序转换

    errif(bind(fd, (sockaddr*)&addr, sizeof(addr)) == -1, "socket bind error");
}

Socket::Socket(int fd, int port, sockaddr_in peerAddr)
    : fd(fd), port(port), peerAddr(peerAddr) {
    setNonBlocking(this->fd);
}

Socket::~Socket() {
    if (fd != -1) {
        close(fd);
        fd = -1;
    }
}

Socket* Socket::acceptConnection() {
    struct sockaddr_in clnt_addr{};
    socklen_t clnt_addr_len = sizeof(clnt_addr);
    int clnt_sockfd = accept(fd, (sockaddr*)&clnt_addr, &clnt_addr_len);
    errif(clnt_sockfd == -1, "socket accept error");
    Socket* sck = new Socket(clnt_sockfd, port, clnt_addr);
    return sck;
}

void Socket::startListen() {
    errif(listen(fd, 128) == -1, "socket listen error");
}

ssize_t Socket::sckRead(void* buf, size_t count) {
    return read(fd, buf, count);
}

ssize_t Socket::sckWrite(const void* buf, size_t count) {
    return write(fd, buf, count);
}
