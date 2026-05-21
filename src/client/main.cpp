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
// 服务端 idle 默认 60s，多 5s 余量
constexpr int kIdleWaitSec = 65;

struct TestCaseResult {
    std::string name;
    bool passed{true};
    std::string reason;
    std::string response;
};

struct ClientConfig {
    const char* host{"127.0.0.1"};
    uint16_t port{8888};
    int repeat{1};
    bool idleTest{false};
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

// 在同一 fd 上发 Keep-Alive GET 并读完整响应；失败表示连接已不可用
bool keepAliveGetOnFd(int fd, const std::string& path, std::string& resp) {
    if (!writeAll(fd, buildGetRequest(path, false))) return false;
    return readHttpResponse(fd, resp);
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
    std::string resp;
    if (!keepAliveGetOnFd(fd, path, resp)) {
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

// -t：单连接发一轮 Keep-Alive GET，静置 kIdleWaitSec 后再请求；
// 若同 fd 失败且重连后可继续，则 idle 超时生效。
TestCaseResult testIdleTimeout(const char* host, uint16_t port) {
    TestCaseResult r;
    r.name = "idle-timeout：同连接 idle " + std::to_string(kIdleWaitSec) +
             "s 后应需重连";

    int fd = connectServer(host, port);
    auto round1 = testKeepAliveOneGet(fd, "/");
    if (!round1.passed) {
        failCase(r, "第一轮失败: " + round1.reason, round1.response);
        closeFd(fd);
        return r;
    }
    for (const char* path : {"/a", "/b"}) {
        auto t = testKeepAliveOneGet(fd, path);
        if (!t.passed) {
            failCase(r, "第一轮失败: " + t.reason, t.response);
            closeFd(fd);
            return r;
        }
    }
    std::cout << "第一轮（同连接 GET /、/a、/b）通过，等待 " << kIdleWaitSec
              << "s（服务端 idle 约 60s）...\n";
    sleep(kIdleWaitSec);

    std::string resp;
    const bool stillAlive = keepAliveGetOnFd(fd, "/idle-check", resp);
    closeFd(fd);

    if (stillAlive) {
        failCase(r, "静置后同连接仍可 GET 200，idle 未关闭连接（未触发重连）",
                 resp);
        return r;
    }

    std::cout << "同连接第二次 GET 失败，判定需重连；正在验证新连接可用...\n";
    int fd2 = connectServer(host, port);
    if (!keepAliveGetOnFd(fd2, "/", resp) ||
        !expectStatus(resp, "HTTP/1.1 200")) {
        failCase(r, "重连后 GET / 失败，服务端可能异常", resp);
        closeFd(fd2);
        return r;
    }
    closeFd(fd2);
    std::cout << "重连后 GET / 成功，idle 超时验收通过。\n";
    return r;
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

void printExpectations() {
    std::cout << "HTTP 验收客户端（仅失败时打印详情）\n";
    std::cout << "期望：\n";
    std::cout << "  1. GET /              → 200, body 含 Hello, World!\n";
    std::cout << "  2. 同连接 GET /a、/b  → 各 200, body 含 Hello, World!\n";
    std::cout << "  3. Connection: close  → 200, 头含 close, 再 read EOF\n";
    std::cout << "  4. POST /             → 400 Bad Request\n\n";
}

void printIdleExpectations() {
    std::cout << "idle 超时验收（-t）\n";
    std::cout << "  单连接 Keep-Alive GET /、/a、/b → 等待 " << kIdleWaitSec
              << "s → 同连接再 GET\n";
    std::cout << "  期望：第二次失败，需重连；重连后 GET / 成功\n\n";
}

void printUsage(const char* prog) {
    std::cerr << "usage:\n"
              << "  " << prog
              << " -n <repeat> [host] [port]   # 多轮 HTTP 四用例\n"
              << "  " << prog
              << " -t [host] [port]            # idle 超时（65s 后需重连）\n"
              << "  " << prog
              << " <repeat> [host] [port]      # 同 -n（兼容旧用法）\n";
}

bool parseConfig(int argc, char* argv[], ClientConfig& cfg) {
    int i = 1;
    bool gotMode = false;

    while (i < argc) {
        if (std::strcmp(argv[i], "-t") == 0) {
            if (gotMode) return false;
            cfg.idleTest = true;
            cfg.repeat = 1;
            gotMode = true;
            ++i;
            continue;
        }
        if (std::strcmp(argv[i], "-n") == 0) {
            if (gotMode) return false;
            if (i + 1 >= argc) return false;
            cfg.repeat = std::atoi(argv[i + 1]);
            if (cfg.repeat < 1) return false;
            gotMode = true;
            i += 2;
            continue;
        }
        break;
    }

    if (!gotMode && i < argc && argv[i][0] >= '0' && argv[i][0] <= '9') {
        cfg.repeat = std::atoi(argv[i++]);
        if (cfg.repeat < 1) return false;
    }

    if (i < argc) {
        cfg.host = argv[i++];
    }
    if (i < argc) {
        cfg.port = static_cast<uint16_t>(std::atoi(argv[i++]));
    }
    if (i < argc) return false;
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    ClientConfig cfg;
    if (!parseConfig(argc, argv, cfg)) {
        printUsage(argv[0]);
        return 1;
    }

    if (cfg.idleTest) {
        printIdleExpectations();
        std::cout << "目标 " << cfg.host << ":" << cfg.port << "\n\n";
        auto r = testIdleTimeout(cfg.host, cfg.port);
        if (r.passed) {
            std::cout << "\n[PASS] " << r.name << "\n";
            return 0;
        }
        printFailure(r);
        return 1;
    }

    printExpectations();
    std::cout << "共 " << cfg.repeat << " 轮 → " << cfg.host << ":" << cfg.port
              << "\n\n";

    int totalFailed = 0;
    for (int round = 1; round <= cfg.repeat; ++round) {
        auto results = runAll(cfg.host, cfg.port);
        for (const auto& t : results) {
            if (!t.passed) ++totalFailed;
        }
        printRoundSummary(round, cfg.repeat, cfg.host, cfg.port, results);
        if (round < cfg.repeat) std::cout << '\n';
    }

    if (totalFailed == 0) {
        std::cout << "\n全部 " << cfg.repeat << " 轮通过。\n";
        return 0;
    }
    std::cerr << "\n合计失败用例数: " << totalFailed << "\n";
    return 1;
}
