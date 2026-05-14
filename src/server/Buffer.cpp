#include "Buffer.h"

#include <string.h>
#include <sys/uio.h>

#include "Socket.h"
#include "error_solve.h"
#define BUFFER_SIZE 1024

Buffer::Buffer() : buf(BUFFER_SIZE), readIdx(0), writeIdx(0) {}

Buffer::~Buffer() {}

void Buffer::dataIn(const void* s, size_t count) {
    if (count <= this->writable()) {
        memcpy(&buf[writeIdx], s, count);
        writeIdx += count;
    } else if (count <= this->writable() + readIdx) {
        // 平移复用数据死区
        std::copy(buf.begin() + readIdx, buf.begin() + writeIdx, buf.begin());
        auto n = this->readable();
        readIdx = 0;
        writeIdx = n;
        this->dataIn(s, count);
    } else {
        // 需要扩容
        buf.resize(writeIdx + count);
        this->dataIn(s, count);
    }
}

void Buffer::dataOut(void* s, size_t count) {
    errif(count > this->readable(), "Not have enough data");
    memcpy(s, &buf[readIdx], count);
    readIdx += count;
    if (readIdx == writeIdx) {
        readIdx = 0;
        writeIdx = 0;
    }
}

// 总是优先尝试“零拷贝”（直接入 Buffer）
// 只有在 Buffer 实在放不下时
// 才动用“二等公民”栈空间作为临时避难所
ssize_t Buffer::sckToBuffer(Socket* sck) {
    char extrabuf[65536];  // 64KB 栈空间
    struct iovec vec[2];
    const size_t writable = this->writable();

    vec[0].iov_base = &buf[writeIdx];
    vec[0].iov_len = writable;
    vec[1].iov_base = extrabuf;
    vec[1].iov_len = sizeof(extrabuf);

    // 使用 readv 一次性读取
    const ssize_t n = readv(sck->getFd(), vec, 2);
    if (n < 0) {
        // 发生异常，需要退出到业务逻辑根据 errno 处理
        return n;
    }
    if (n <= writable) {
        writeIdx += n;
    } else {
        writeIdx = buf.size();
        this->dataIn(extrabuf, n - writable);  // 剩余部分自动扩容并存入
    }
    return n;
}

ssize_t Buffer::bufferToSck(Socket* sck, size_t count) {
    errif(count > this->readable(), "Not have enough data");
    auto res = sck->sckWrite(&buf[readIdx], count);
    readIdx += count;
    if (readIdx == writeIdx) {
        readIdx = 0;
        writeIdx = 0;
    }
    return res;
}

void Buffer::bufToBuf(Buffer* peer, size_t count) {
    errif(count > this->readable(), "Not have enough data");
    if (count <= peer->writable()) {
        std::copy(buf.begin() + readIdx, buf.begin() + writeIdx,
                  peer->buf.begin() + peer->writeIdx);
        readIdx += count;
        if (readIdx == writeIdx) {
            readIdx = 0;
            writeIdx = 0;
        }
        peer->writeIdx += count;
    } else if (count <= peer->writable() + peer->readIdx) {
        std::copy(peer->buf.begin() + peer->readIdx,
                  peer->buf.begin() + peer->writeIdx, peer->buf.begin());
        auto n = peer->readable();
        peer->readIdx = 0;
        peer->writeIdx = n;
        this->bufToBuf(peer, count);
    } else {
        peer->buf.resize(peer->writeIdx + count);
        this->bufToBuf(peer, count);
    }
}
