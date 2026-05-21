#include "Connection.h"

#include <errno.h>
#include <string.h>

#include <functional>
#include <iostream>

#include "Buffer.h"
#include "Channel.h"
#include "EventLoop.h"
#include "HttpProcess.h"
#include "Socket.h"
#include "error_solve.h"

Connection::Connection(EventLoop* eloop, std::unique_ptr<Socket> sock)
    : eloop(eloop),
      sck(std::move(sock)),
      state(ConnState::connected),
      working(0),
      timerId(-1) {
    errif(this->sck.get() == nullptr, "conn get nullptr ");
    channel = std::make_unique<Channel>(eloop, sck->getFd());
    channel->setReadCallBack([this]() { this->handleReadCallBack(); });
    channel->setWriteCallBack([this]() { this->handleWriteCallBack(); });
    readBuffer = std::make_unique<Buffer>();
    writeBuffer = std::make_unique<Buffer>();
    httpProcess_ = std::make_unique<HttpProcess>();
}

void Connection::startConnect() {
    channel->enableReading();
    timerId = addTimerCallBack();
}

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

void Connection::refreshTimer() {
    deleteTimerCallBack(timerId);
    timerId = addTimerCallBack();
}

void Connection::handleReadCallBack() {
    refreshTimer();
    onHttp();
}

void Connection::checkEmptyReadAfterEof() {
    if (state == ConnState::peerClose && working == 0 &&
        writeBuffer->readable() == 0) {
        handleDead();
    }
}

void Connection::sendHttpOnLoop(const std::string& resp, bool keepAlive) {
    writeBuffer->dataIn(resp.data(), resp.size());
    trySendToSck();
    if (!keepAlive) {
        channel->disableReading();
        state = ConnState::peerClose;
    }
}

void Connection::onHttp() {
    auto self = shared_from_this();
    readFromSck();

    if (readBuffer->readable() == 0) {
        checkEmptyReadAfterEof();
        return;
    }

    while (true) {
        ParseResult r = httpProcess_->parse(readBuffer.get());

        if (r == ParseResult::kNeedMore) break;

        if (r == ParseResult::kLineTooLong) {
            // 拒绝此连接
            channel->disableReading();
            state = ConnState::peerClose;
            checkEmptyReadAfterEof();
            return;
        }

        if (r == ParseResult::kError) {
            sendHttpOnLoop(HttpProcess::build400(), false);
            httpProcess_->reset();
            checkEmptyReadAfterEof();
            return;
        }

        // kComplete — 合法 GET
        HttpRequest req = httpProcess_->releaseRequest();
        working++;
        process([self, req = std::move(req)]() {
            std::string resp = HttpProcess::buildResponse(req);
            const bool keepAlive = req.keepAlive;
            self->eloop->enqueueTask(
                [self, resp = std::move(resp), keepAlive]() {
                    self->working--;
                    self->sendHttpOnLoop(resp, keepAlive);
                    self->checkEmptyReadAfterEof();
                });
        });
        // 粘包：继续 while，不在此 break
    }
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
                handleDead();
                break;
            }
        }
    }
}

void Connection::handleWriteCallBack() {
    refreshTimer();
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
            handleDead();
            break;
        }
    }
}

void Connection::handleDead() {
    auto self = shared_from_this();
    state = ConnState::dead;
    channel->disableAll();
    std::cout << "client " << sck->getFd() << " connection break\n";
    // 注入任务队列，防止本次channel没有响应完成就析构
    eloop->enqueueTask(
        [self]() { self->removeConnectionCallBack(self->sck.get()); });
    return;
}

void Connection::stop() {
    // 先禁用 channel，拒绝外界连接
    channel->disableAll();
}