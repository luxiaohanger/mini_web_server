#include "Connection.h"

#include <errno.h>
#include <string.h>

#include <functional>
#include <iostream>

#include "Buffer.h"
#include "Channel.h"
#include "EventLoop.h"
#include "Socket.h"
#include "error_solve.h"

Connection::Connection(EventLoop* eloop, std::unique_ptr<Socket> sock)
    : eloop(eloop),
      sck(std::move(sock)),
      state(ConnState::connected),
      working(0) {
    errif(this->sck.get() == nullptr, "conn get nullptr ");
    channel = std::make_unique<Channel>(eloop, sck->getFd());
    channel->setReadCallBack([this]() { this->handleReadCallBack(); });
    channel->setWriteCallBack([this]() { this->handleWriteCallBack(); });
    readBuffer = std::make_unique<Buffer>();
    writeBuffer = std::make_unique<Buffer>();
}

void Connection::startConnect() { channel->enableReading(); }

Connection::~Connection() {}

void Connection::readFromSck() {
    while (true) {
        ssize_t read_byte = readBuffer->sckToBuffer(sck.get());
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
            state = ConnState::peerClose;
            break;
        }
    }
}

void Connection::handleReadCallBack() { Echo(); }

void Connection::Echo() {
    auto self = shared_from_this();
    readFromSck();

    int n = readBuffer->readable();
    if (n != 0)
        std::cout << "msg from client " << sck->getFd() << " : "
                  << readBuffer->peek() << '\n';
    else {
        // 此时说明 peer 发送 EOF 包
        // 不调用 working 处理空包
        // 若处理完毕，结束 conn
        if (state == ConnState::peerClose && working == 0 &&
            writeBuffer->readable() == 0) {
            state = ConnState::dead;
            handleDead();
        }
        // 说明没有处理完毕
        // 不从此处结束conn,直接退出即可
        // 由后续写失败结束conn
        return;
    }

    std::string data;
    // data 的数据区空间不足，先扩容再填充
    data.resize(n);
    readBuffer->dataOut(data.data(), n);
    // 注意异步线程执行不能按引用捕获，因为使用时栈空间可能已经释放
    // 可以使用移动语义，按值捕获
    working++;
    process([self, data = std::move(data)]() {
        // 这里是任务线程在执行
        std::string res;
        self->processEcho(data, res);

        // 处理完成后向eloop善后队列注入任务
        // 注入函数同时完成了唤醒功能
        self->eloop->enqueueTask([self, res = std::move(res)]() {
            // 这里是 own subReactor 线程在执行善后任务
            // 先改计数器
            self->working--;
            self->writeBuffer->dataIn(res.data(), res.size());
            self->trySendToSck();
        });
    });
}

void Connection::processEcho(const std::string& data, std::string& res) {
    res = data;
}

void Connection::trySendToSck() {
    // 专门为连续中断设计的循环
    while (writeBuffer->readable() != 0) {
        auto send_bytes =
            writeBuffer->bufferToSck(sck.get(), writeBuffer->readable());
        if (send_bytes > 0) {
            if (writeBuffer->readable() == 0) {
                channel->disableWriting();
                // 写排空，检查连接状态
                if (state == ConnState::peerClose && working == 0) {
                    state = ConnState::dead;
                    handleDead();
                }
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
                state = ConnState::dead;
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
        auto n = writeBuffer->bufferToSck(sck.get(), writeBuffer->readable());
        if (n > 0) {
            if (writeBuffer->readable() == 0) {
                // 彻底发完，立刻注销写事件
                channel->disableWriting();
                // 写排空，检查连接状态
                if (state == ConnState::peerClose && working == 0) {
                    state = ConnState::dead;
                    handleDead();
                }
                break;
            }
        } else if (n == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 内核缓冲区又满了，保持写事件监听，等待下一次 Epoll 唤醒
                break;
            }
            if (errno == EINTR) continue;  // 信号中断，继续写

            // 其他致命错误
            state = ConnState::dead;
            handleDead();
            break;
        }
    }
}

void Connection::handleDead() {
    std::cout << "client " << sck->getFd() << " connection break\n";
    removeConnectionCallBack(sck.get());
    return;
}

void Connection::stop() {
    // 先禁用 channel，拒绝外界连接
    channel->disableAll();
}