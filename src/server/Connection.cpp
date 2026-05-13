#include "Connection.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <functional>
#include <iostream>

#include "Channel.h"
#include "Socket.h"
#define READ_BUFFER 1024

Connection::Connection(EventLoop* eloop, Socket* sck) : eloop(eloop), sck(sck) {
    channel = new Channel(eloop, sck->getFd());
    channel->setReadCallBack(std::bind(&Connection::handleReadCallBack, this));
}

void Connection::startConnect() { channel->enableReading(); }

Connection::~Connection() {
    if (sck) {
        delete sck;
        sck = nullptr;
    }
    if (channel) {
        delete channel;
        channel = nullptr;
    }
}

void Connection::handleReadCallBack() {
    char buf[READ_BUFFER];
    int clnt_sockfd = sck->getFd();
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
            // 通知上层（Server）销毁这个 Connection 对象。
            deleteConnectionCallBack(clnt_sockfd);
            break;
        }
    }
}