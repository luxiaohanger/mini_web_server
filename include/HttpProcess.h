#pragma once

#include <cstddef>
#include <string>

class Buffer;

enum class ParseResult {
    kNeedMore,     // 半包，等下次 read
    kComplete,     // 合法 GET 完整
    kError,        // 非法 → 400
    kLineTooLong,  // 超长 → 关连接
};

struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
    std::string host;
    bool keepAlive{true};
};

// Thread safety:
//   parse / releaseRequest / reset  → owner EventLoop（I/O 线程）only
//   buildResponse / build400        → worker 线程（无状态）
class HttpProcess {
   public:
    HttpProcess() = default;

    // ---------- I/O 线程 ----------
    ParseResult parse(Buffer* readBuf);
    HttpRequest releaseRequest();
    void reset();

    // ---------- Worker 线程 ----------
    static std::string buildResponse(const HttpRequest& req);
    static std::string build400();

   private:
    enum class State { kExpectRequestLine, kExpectHeaders };

    static constexpr size_t kMaxLineLen = 8192;

    State state_{State::kExpectRequestLine};
    HttpRequest current_;
    size_t contentLength_{0};
    size_t headerLineCount_{0};

    // 从 readBuf 取一行（不含 \r\n）；false = 需要更多数据
    // lineTooLong = true → 调用方返回 kLineTooLong
    bool retrieveLine(Buffer* readBuf, std::string& line, bool& lineTooLong);

    static std::string toLower(std::string s);
    static bool parseRequestLine(const std::string& line, HttpRequest& out);
    static bool parseHeaderLine(const std::string& line, HttpRequest& req,
                                size_t& contentLength);
    static void updateKeepAlive(HttpRequest& req, const std::string& connValue);
};