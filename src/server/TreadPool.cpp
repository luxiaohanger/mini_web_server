#include "ThreadPool.h"

ThreadPool::ThreadPool(size_t n) {
    workers.reserve(n);
    for (int i = 0; i < n; ++i) {
        // 用 lambda 表达式作为thread构造函数参数
        workers.emplace_back([this]() {
            while (true) {
                // 声明一个任务，尝试从任务队列获取任务
                std::function<void()> task;

                {  // === 临界区开始：从队列取任务 ===
                    std::unique_lock<std::mutex> lock(this->queue_mtx);

                    // 等待条件：线程池停止，或者任务队列不为空
                    this->cv.wait(lock, [this]() {
                        return this->stop || !this->tasks.empty();
                    });

                    // 如果线程池停止了，且队列里的任务都干完了，线程安全退出
                    if (this->stop && this->tasks.empty()) {
                        return;  // 结束 Lambda 函数，意味着线程销毁
                    }

                    // 移走队列头部的任务（使用 std::move 避免拷贝开销）
                    task = std::move(this->tasks.front());
                    this->tasks.pop();
                }  // === 临界区结束：锁在这里自动释放 ===

                // 【核心细节】在锁的外边执行任务，否则多线程就变成串行了！
                if (task) {
                    task();
                }
            }
        });
    }
}

ThreadPool::~ThreadPool() {
    {
        // 加锁修改，保证 cpu 可见一致性
        std::unique_lock<std::mutex> lock(this->queue_mtx);
        stop = true;
    }

    // 唤醒所有线程退出
    cv.notify_all();

    for (auto& it : workers) {
        it.join();
    }
}