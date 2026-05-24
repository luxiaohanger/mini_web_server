#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <thread>

#include "Server.h"

namespace {

// 仅由信号处理器写入；main 线程读取
std::atomic<bool> g_stop_requested{false};

void onStopSignal(int /*signo*/) {
    // 信号处理器内只做 async-signal-safe 操作
    g_stop_requested.store(true, std::memory_order_release);
}

}  // namespace

int main(int argc, char* argv[]) {
    // Ctrl+C → SIGINT；kill 默认 → SIGTERM
    std::signal(SIGINT, onStopSignal);
    std::signal(SIGTERM, onStopSignal);

    bool threadpool = false;  // 默认 off

    if (argc == 1) {
        // 无参数，用默认
    } else if (argc == 3 && std::strcmp(argv[1], "-t") == 0) {
        if (std::strcmp(argv[2], "on") == 0) {
            threadpool = true;
        } else if (std::strcmp(argv[2], "off") == 0) {
            threadpool = false;
        } else {
            std::cerr << "用法: " << argv[0] << " [-t on|off]\n";
            return 1;
        }
    } else {
        std::cerr << "用法: " << argv[0] << " [-t on|off]\n";
        return 1;
    }

    Server s(threadpool);
    s.listenPort(8888);

    // 启动 MainReactor / SubReactor I/O 线程，立即返回
    std::cout << "Server is starting!\n";
    s.start();

    // 控制线程：等待外部停服信号
    while (!g_stop_requested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // 接受外部终止信号，按语义退出
    s.stop();

    std::cout << "Server stop safely!\n";

    return 0;
}