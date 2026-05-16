#include "Connection.h"

#include <errno.h>
#include <string.h>

#include <functional>
#include <iostream>

#include "Buffer.h"
#include "Channel.h"
#include "EventLoop.h"
#include "Socket.h"

Connection::Connection(EventLoop* eloop, Socket* sck) : eloop(eloop), sck(sck) {
    channel = new Channel(eloop, sck->getFd());
    channel->setReadCallBack(std::bind(&Connection::handleReadCallBack, this));
    channel->setWriteCallBack(
        std::bind(&Connection::handleWriteCallBack, this));
    readBuffer = new Buffer();
    writeBuffer = new Buffer();
}

void Connection::startConnect() { channel->enableReading(); }

Connection::~Connection() {
    delete channel;
    delete sck;
    delete readBuffer;
    delete writeBuffer;
}

void Connection::readFromSck() {
    while (true) {
        ssize_t read_byte = readBuffer->sckToBuffer(sck);
        if (read_byte > 0) {
        } else if (read_byte == -1 && errno == EINTR) {
            // 被信号中断，继续读取
            continue;
        } else if (read_byte == -1 &&
                   ((errno == EAGAIN) || (errno == EWOULDBLOCK))) {
            // 非阻塞IO中，这个条件表示数据全部读取完毕
            break;
        } else if (read_byte == 0) {  // EOF，客户端断开连接
            // 通知上层（Server）销毁这个 Connection 对象。
            deleteConnectionCallBack(sck);
            break;
        }
    }
}

void Connection::handleReadCallBack() { Echo(); }

void Connection::Echo() {
    readFromSck();
    std::cout << "msg from client " << sck->getFd() << " : "
              << readBuffer->peek() << '\n';

    // 调用线程池任务线程处理业务逻辑
    process([this]() {
        this->processEcho();

        // 处理完成后向eloop善后队列注入任务
        // 注入函数同时完成了唤醒功能
        eloop->enqueueTask([this]() { this->trySendToSck(); });
    });
}

void Connection::processEcho() {
    readBuffer->bufToBuf(writeBuffer, readBuffer->readable());
}

void Connection::trySendToSck() {
    if (writeBuffer->readable() == 0) return;
    // 专门为连续中断设计的循环
    while (writeBuffer->readable() != 0) {
        auto send_bytes =
            writeBuffer->bufferToSck(sck, writeBuffer->readable());
        if (send_bytes > 0) {
            if (writeBuffer->readable() == 0) {
                channel->disableWriting();
                return;
            } else {
                channel->enableWriting();
                break;
            }
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                channel->enableWriting();
                break;
            } else if (errno == EINTR) {
                continue;
            } else {
                // 真正的错误（如 EPIPE 客户端断开），进行异常处理
                deleteConnectionCallBack(sck);
                break;
            }
        }
    }
}

void Connection::handleWriteCallBack() {
    // EPOLLOUT 触发事件
    if (writeBuffer->readable() == 0) {
        channel->disableWriting();
        return;
    }

    while (writeBuffer->readable() > 0) {
        auto n = writeBuffer->bufferToSck(sck, writeBuffer->readable());
        if (n > 0) {
            if (writeBuffer->readable() == 0) {
                // 彻底发完，立刻注销写事件
                channel->disableWriting();
                break;
            }
        } else if (n == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 内核缓冲区又满了，保持写事件监听，等待下一次 Epoll 唤醒
                break;
            }
            if (errno == EINTR) continue;  // 信号中断，继续写

            // 其他致命错误
            deleteConnectionCallBack(sck);
            break;
        }
    }
}