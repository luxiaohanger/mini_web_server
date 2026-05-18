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
    channel->setReadCallBack([this]() { this->handleReadCallBack(); });
    channel->setWriteCallBack([this]() { this->handleWriteCallBack(); });
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
        } else if (read_byte == 0) {
            // EOF，客户端断开连接
            handleDead();
            break;
        }
    }
}

void Connection::handleReadCallBack() { Echo(); }

void Connection::Echo() {
    // 提前建立 self ，否则 IO 结束就立即销毁
    auto self = shared_from_this();
    readFromSck();

    std::cout << "msg from client " << sck->getFd() << " : "
              << readBuffer->peek() << '\n';

    // 调用线程池任务线程处理业务逻辑
    // 捕获 self，保证 conn 生命周期在此期间延续
    int n = readBuffer->readable();
    std::string data;
    // data 的数据区空间不足，先扩容再填充
    data.resize(n);
    readBuffer->dataOut(data.data(), n);
    // 注意异步线程执行不能按引用捕获，因为使用时栈空间可能已经释放
    // 可以使用移动语义，按值捕获
    process([self, data = std::move(data)]() {
        // 这里是任务线程在执行
        std::string res;
        self->processEcho(data, res);

        // 处理完成后向eloop善后队列注入任务
        // 注入函数同时完成了唤醒功能
        self->eloop->enqueueTask([self, res = std::move(res)]() {
            // 这里是 own subReactor 线程在执行善后任务
            self->writeBuffer->dataIn(res.data(), res.size());
            self->trySendToSck();
        });
    });
}

void Connection::processEcho(const std::string& data, std::string& res) {
    res = data;
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
                handleDead();
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
            handleDead();
            break;
        }
    }
}

void Connection::handleDead() {
    std::cout << "client " << sck->getFd() << " connection break\n";
    removeConnectionCallBack(sck);
    return;
}

void Connection::stop() {
    // 先禁用 channel，拒绝外界连接
    channel->disableAll();
}