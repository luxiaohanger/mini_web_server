#include "HttpProcess.h"

#include <algorithm>
#include <cctype>
#include <string_view>

#include "Buffer.h"

namespace {

constexpr const char kBody[] = "Hello, World!";
constexpr size_t kBodyLen = 13;

bool isHttpVersion(std::string_view v) {
    return v == "HTTP/1.1" || v == "HTTP/1.0";
}

// 按单个空格切分，限制最多 3 段（path 中可含空格则本阶段不支持）
bool splitThree(std::string_view line, std::string& a, std::string& b,
                std::string& c) {
    auto p1 = line.find(' ');
    if (p1 == std::string_view::npos) return false;
    auto p2 = line.find(' ', p1 + 1);
    if (p2 == std::string_view::npos) return false;
    if (line.find(' ', p2 + 1) != std::string_view::npos) return false;
    a.assign(line.substr(0, p1));
    b.assign(line.substr(p1 + 1, p2 - p1 - 1));
    c.assign(line.substr(p2 + 1));
    return true;
}

}  // namespace

std::string HttpProcess::toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool HttpProcess::retrieveLine(Buffer* readBuf, std::string& line,
                               bool& lineTooLong) {
    lineTooLong = false;
    const size_t crlf = readBuf->findCRLF();
    if (crlf == std::string::npos) return false;
    if (crlf > kMaxLineLen) {
        lineTooLong = true;
        return true;
    }
    line.assign(readBuf->peek().data(), crlf);
    readBuf->retrieve(crlf + 2);
    return true;
}

bool HttpProcess::parseRequestLine(const std::string& line, HttpRequest& out) {
    std::string method, path, version;
    if (!splitThree(line, method, path, version)) return false;
    if (method != "GET") return false;
    if (path.empty() || path[0] != '/') return false;
    if (!isHttpVersion(version)) return false;

    out.method = std::move(method);
    out.path = std::move(path);
    out.version = std::move(version);
    if (out.version == "HTTP/1.0") out.keepAlive = false;
    return true;
}

void HttpProcess::updateKeepAlive(HttpRequest& req,
                                  const std::string& connValue) {
    const std::string v = toLower(connValue);
    if (v.find("close") != std::string::npos)
        req.keepAlive = false;
    else if (v.find("keep-alive") != std::string::npos)
        req.keepAlive = true;
}

bool HttpProcess::parseHeaderLine(const std::string& line, HttpRequest& req,
                                  size_t& contentLength) {
    const auto colon = line.find(':');
    if (colon == std::string::npos) return false;

    std::string key = toLower(line.substr(0, colon));
    std::string value = line.substr(colon + 1);
    // trim leading spaces on value
    const auto start = value.find_first_not_of(' ');
    if (start == std::string::npos)
        value.clear();
    else
        value = value.substr(start);

    if (key == "host") {
        req.host = value;
    } else if (key == "connection") {
        updateKeepAlive(req, value);
    } else if (key == "content-length") {
        if (value.empty()) return false;
        for (char c : value)
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        contentLength = static_cast<size_t>(std::stoul(value));
    }
    return true;
}

ParseResult HttpProcess::parse(Buffer* readBuf) {
    if (state_ == State::kExpectRequestLine) {
        std::string line;
        bool tooLong = false;
        if (!retrieveLine(readBuf, line, tooLong))
            return ParseResult::kNeedMore;
        if (tooLong) return ParseResult::kLineTooLong;

        if (!parseRequestLine(line, current_)) return ParseResult::kError;

        contentLength_ = 0;
        headerLineCount_ = 0;
        state_ = State::kExpectHeaders;
    }

    while (state_ == State::kExpectHeaders) {
        std::string line;
        bool tooLong = false;
        if (!retrieveLine(readBuf, line, tooLong))
            return ParseResult::kNeedMore;
        if (tooLong) return ParseResult::kLineTooLong;

        if (line.empty()) {
            if (contentLength_ > 0) return ParseResult::kError;
            state_ = State::kExpectRequestLine;
            return ParseResult::kComplete;
        }

        ++headerLineCount_;
        if (headerLineCount_ > 100) return ParseResult::kLineTooLong;

        if (!parseHeaderLine(line, current_, contentLength_))
            return ParseResult::kError;
    }

    return ParseResult::kNeedMore;
}

HttpRequest HttpProcess::releaseRequest() {
    HttpRequest req = std::move(current_);
    current_ = HttpRequest{};
    contentLength_ = 0;
    headerLineCount_ = 0;
    state_ = State::kExpectRequestLine;
    return req;
}

void HttpProcess::reset() {
    current_ = HttpRequest{};
    contentLength_ = 0;
    headerLineCount_ = 0;
    state_ = State::kExpectRequestLine;
}

std::string HttpProcess::buildResponse(const HttpRequest& req) {
    std::string out;
    out.reserve(128);
    out += "HTTP/1.1 200 OK\r\n";
    out += "Content-Length: ";
    out += std::to_string(kBodyLen);
    out += "\r\n";
    out +=
        req.keepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    out += "\r\n";
    out += kBody;
    return out;
}

std::string HttpProcess::build400() {
    return "HTTP/1.1 400 Bad Request\r\n"
           "Content-Length: 0\r\n"
           "Connection: close\r\n"
           "\r\n";
}