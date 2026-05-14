#pragma once
#include <sys/types.h>

#include <string_view>

#include "vector"
class Socket;

class Buffer {
   private:
    std::vector<char> buf;
    size_t readIdx;
    size_t writeIdx;

   public:
    Buffer();
    ~Buffer();

    // 可读缓存
    size_t readable() { return writeIdx - readIdx; }

    // 可写剩余空间
    size_t writable() { return buf.size() - writeIdx; }

    // 接口指针使用 void* ,符合系统函数参数
    // 业务逻辑按需转换
    void dataIn(const void* s, size_t count);
    void dataOut(void* s, size_t count);

    // buffer 和 socket 通信
    ssize_t sckToBuffer(Socket* sck);
    ssize_t bufferToSck(Socket* sck, size_t count);

    // buffer 间通信
    void bufToBuf(Buffer* peer, size_t count);

    std::string_view peek() {
        // 仅仅返回一个视图，零拷贝
        return std::string_view(&buf[readIdx], readable());
    }
};
