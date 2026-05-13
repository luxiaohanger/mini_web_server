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