#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "error_solve.h"

namespace {

constexpr int kRecvTimeoutSec = 3;

struct TestCaseResult {
    std::string name;
    bool passed{true};
    std::string reason;
    std::string response;
};

int connectServer(const char* host, uint16_t port) {
    int fd = socket(PF_INET, SOCK_STREAM, 0);
    errif(fd == -1, "socket create error");

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    errif(inet_pton(AF_INET, host, &addr.sin_addr) != 1, "inet_pton error");

    errif(connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1,
          "connect error");
    return fd;
}

bool writeAll(int fd, std::string_view data) {
    while (!data.empty()) {
        ssize_t n = write(fd, data.data(), data.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        data.remove_prefix(static_cast<size_t>(n));
    }
    return true;
}

ssize_t readWithTimeout(int fd, char* buf, size_t cap, int timeoutSec) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    timeval tv{};
    tv.tv_sec = timeoutSec;
    tv.tv_usec = 0;

    int ret = select(fd + 1, &rfds, nullptr, nullptr, &tv);
    if (ret == 0) return 0;
    if (ret < 0) {
        if (errno == EINTR) return readWithTimeout(fd, buf, cap, timeoutSec);
        return -1;
    }
    return read(fd, buf, cap);
}

bool readHttpResponse(int fd, std::string& out) {
    out.clear();
    char buf[4096];

    while (out.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = readWithTimeout(fd, buf, sizeof(buf), kRecvTimeoutSec);
        if (n < 0) return false;
        if (n == 0) return !out.empty();
        out.append(buf, static_cast<size_t>(n));
        if (out.size() > 65536) return false;
    }

    const auto headerEnd = out.find("\r\n\r\n");
    const std::string headers = out.substr(0, headerEnd);

    size_t contentLength = 0;
    size_t pos = 0;
    while (pos < headers.size()) {
        auto lineEnd = headers.find("\r\n", pos);
        if (lineEnd == std::string::npos) lineEnd = headers.size();
        std::string line = headers.substr(pos, lineEnd - pos);
        pos = (lineEnd == headers.size()) ? lineEnd : lineEnd + 2;

        if (line.size() >= 15 && (line.rfind("Content-Length:", 0) == 0 ||
                                  line.rfind("content-length:", 0) == 0)) {
            contentLength = static_cast<size_t>(std::stoul(line.substr(15)));
        }
    }

    const size_t bodyStart = headerEnd + 4;
    while (out.size() - bodyStart < contentLength) {
        ssize_t n = readWithTimeout(fd, buf, sizeof(buf), kRecvTimeoutSec);
        if (n < 0) return false;
        if (n == 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    return true;
}

std::string buildGetRequest(const std::string& path, bool close) {
    std::string req;
    req += "GET ";
    req += path;
    req += " HTTP/1.1\r\n";
    req += "Host: localhost\r\n";
    req += close ? "Connection: close\r\n" : "Connection: keep-alive\r\n";
    req += "\r\n";
    return req;
}

std::string buildPostRequest() {
    return "POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n";
}

void failCase(TestCaseResult& r, const std::string& reason,
              const std::string& resp = {}) {
    r.passed = false;
    r.reason = reason;
    if (!resp.empty()) r.response = resp;
}

void printFailure(const TestCaseResult& r) {
    std::cerr << "[FAIL] " << r.name << "\n";
    std::cerr << "  " << r.reason << "\n";
    if (!r.response.empty()) {
        std::cerr << "  ----- 实际响应 -----\n";
        std::cerr << r.response;
        if (r.response.back() != '\n') std::cerr << '\n';
        std::cerr << "  ----- end -----\n";
    }
    std::cerr << '\n';
}

bool expectStatus(const std::string& resp, const char* statusLine) {
    return resp.rfind(statusLine, 0) == 0 ||
           resp.find(std::string("\r\n") + statusLine) != std::string::npos;
}

bool expectBodyContains(const std::string& resp, const char* body) {
    return resp.find(body) != std::string::npos;
}

void closeFd(int fd) {
    if (fd >= 0) close(fd);
}

TestCaseResult testSingleGet(const char* host, uint16_t port) {
    TestCaseResult r;
    r.name = "test1 单次 GET / → 200 + body 含 Hello, World!";
    int fd = connectServer(host, port);
    if (!writeAll(fd, buildGetRequest("/", false))) {
        failCase(r, "写入请求失败");
        closeFd(fd);
        return r;
    }

    std::string resp;
    if (!readHttpResponse(fd, resp)) {
        failCase(r, "读取响应失败（超时或连接异常）");
        closeFd(fd);
        return r;
    }
    if (!expectStatus(resp, "HTTP/1.1 200")) {
        failCase(r, "期望状态行 HTTP/1.1 200", resp);
    } else if (!expectBodyContains(resp, "Hello, World")) {
        failCase(r, "期望 body 含 Hello, World", resp);
    }
    closeFd(fd);
    return r;
}

TestCaseResult testKeepAliveOneGet(int fd, const std::string& path) {
    TestCaseResult r;
    r.name = "test2 同连接 GET " + path + " → 200 + body 含 Hello, World!";
    if (!writeAll(fd, buildGetRequest(path, false))) {
        failCase(r, "写入请求失败");
        return r;
    }
    std::string resp;
    if (!readHttpResponse(fd, resp)) {
        failCase(r, "读取响应失败（超时或连接异常）");
        return r;
    }
    if (!expectStatus(resp, "HTTP/1.1 200")) {
        failCase(r, "期望状态行 HTTP/1.1 200", resp);
    } else if (!expectBodyContains(resp, "Hello, World")) {
        failCase(r, "期望 body 含 Hello, World", resp);
    }
    return r;
}

std::vector<TestCaseResult> testKeepAliveTwoGets(const char* host,
                                                 uint16_t port) {
    std::vector<TestCaseResult> results;
    int fd = connectServer(host, port);
    results.push_back(testKeepAliveOneGet(fd, "/a"));
    results.push_back(testKeepAliveOneGet(fd, "/b"));
    closeFd(fd);
    return results;
}

TestCaseResult testConnectionClose(const char* host, uint16_t port) {
    TestCaseResult r;
    r.name =
        "test3 GET / + Connection: close → 200、响应头含 close、再 read 得 "
        "EOF(n=0)";
    int fd = connectServer(host, port);
    if (!writeAll(fd, buildGetRequest("/", true))) {
        failCase(r, "写入请求失败");
        closeFd(fd);
        return r;
    }

    std::string resp;
    if (!readHttpResponse(fd, resp)) {
        failCase(r, "读取响应失败（超时或连接异常）");
        closeFd(fd);
        return r;
    }
    if (!expectStatus(resp, "HTTP/1.1 200")) {
        failCase(r, "期望状态行 HTTP/1.1 200", resp);
        closeFd(fd);
        return r;
    }
    if (resp.find("Connection: close") == std::string::npos) {
        failCase(r, "期望响应头含 Connection: close", resp);
        closeFd(fd);
        return r;
    }

    char buf[64];
    ssize_t n = readWithTimeout(fd, buf, sizeof(buf), 1);
    if (n != 0) {
        failCase(r, "期望服务端关闭连接后 extra read 返回 0(EOF)，实际 n=" +
                        std::to_string(n));
    }
    closeFd(fd);
    return r;
}

TestCaseResult testPost400(const char* host, uint16_t port) {
    TestCaseResult r;
    r.name = "test4 POST / → 400 Bad Request";
    int fd = connectServer(host, port);
    if (!writeAll(fd, buildPostRequest())) {
        failCase(r, "写入请求失败");
        closeFd(fd);
        return r;
    }

    std::string resp;
    if (!readHttpResponse(fd, resp)) {
        failCase(r, "读取响应失败（超时或连接异常）");
        closeFd(fd);
        return r;
    }
    if (!expectStatus(resp, "HTTP/1.1 400")) {
        failCase(r, "期望状态行 HTTP/1.1 400", resp);
    }
    closeFd(fd);
    return r;
}

void printRoundSummary(int round, int totalRounds, const char* host,
                       uint16_t port,
                       const std::vector<TestCaseResult>& results) {
    int failed = 0;
    for (const auto& t : results) {
        if (!t.passed) ++failed;
    }
    const int passed = static_cast<int>(results.size()) - failed;

    if (failed == 0) {
        std::cout << "round " << round << "/" << totalRounds << " @ " << host
                  << ":" << port << " — 全部通过 (" << passed << "/"
                  << results.size() << ")\n";
        return;
    }

    std::cerr << "round " << round << "/" << totalRounds << " @ " << host << ":"
              << port << " — 失败 " << failed << "/" << results.size()
              << "\n\n";
    for (const auto& t : results) {
        if (!t.passed) printFailure(t);
    }
}

std::vector<TestCaseResult> runAll(const char* host, uint16_t port) {
    std::vector<TestCaseResult> all;
    all.push_back(testSingleGet(host, port));
    auto ka = testKeepAliveTwoGets(host, port);
    all.insert(all.end(), ka.begin(), ka.end());
    all.push_back(testConnectionClose(host, port));
    all.push_back(testPost400(host, port));
    return all;
}

void printExpectations() {
    std::cout << "HTTP 验收客户端（仅失败时打印详情）\n";
    std::cout << "期望：\n";
    std::cout << "  1. GET /              → 200, body 含 Hello, World!\n";
    std::cout << "  2. 同连接 GET /a、/b  → 各 200, body 含 Hello, World!\n";
    std::cout << "  3. Connection: close  → 200, 头含 close, 再 read EOF\n";
    std::cout << "  4. POST /             → 400 Bad Request\n\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    const char* host = "127.0.0.1";
    uint16_t port = 8888;
    int repeat = 1;

    if (argc >= 2) {
        repeat = std::atoi(argv[1]);
        if (repeat < 1) {
            std::cerr << "repeat count must be >= 1\n";
            return 1;
        }
    }
    if (argc >= 4) {
        host = argv[2];
        port = static_cast<uint16_t>(std::atoi(argv[3]));
    }
    if (argc > 4) {
        std::cerr << "usage: client [repeat] [host] [port]\n";
        return 1;
    }

    printExpectations();
    std::cout << "共 " << repeat << " 轮 → " << host << ":" << port << "\n\n";

    int totalFailed = 0;
    for (int i = 1; i <= repeat; ++i) {
        auto results = runAll(host, port);
        for (const auto& t : results) {
            if (!t.passed) ++totalFailed;
        }
        printRoundSummary(i, repeat, host, port, results);
        if (i < repeat) std::cout << '\n';
    }

    if (totalFailed == 0) {
        std::cout << "\n全部 " << repeat << " 轮通过。\n";
        return 0;
    }
    std::cerr << "\n合计失败用例数: " << totalFailed << "\n";
    return 1;
}
