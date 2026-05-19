#pragma once
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

class ThreadPool {
   private:
    // 工作线程数组
    std::vector<std::thread> workers;

    // 任务队列：存放包装后的通用任务（void()签名）
    std::queue<std::function<void()>> tasks;

    // 同步机制
    std::mutex queue_mtx;        // 保护任务队列的互斥锁
    std::condition_variable cv;  // 唤醒工作线程的条件变量

    // 状态标志，作为线程池即将消亡的停止信号
    // 也用于使被唤醒的任务线程顺利结束
    bool stop_;

   public:
    // 构造函数：启动指定数量的工作线程
    explicit ThreadPool(size_t n = 10);

    // 核心接口：向线程池投递任务
    // 运用了可变参数模板、右值引用与完美转发，支持任意函数签名
    template <class F, class... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<typename std::result_of<F(Args...)>::type>;

    // 析构函数：通知所有线程退出并安全释放资源
    ~ThreadPool();

    void stop();

    // 禁止拷贝构造和赋值操作（资源所有权唯一）
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
};

// 注意：由于 enqueue
// 是一个成员模板函数，它的实现必须放在头文件中（或者本文件末尾）

template <class F, class... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args)
    -> std::future<typename std::result_of<F(Args...)>::type> {
    // 1. 利用 result_of 拿到函数的具体返回值类型（比如 int 或 bool）
    using return_type = typename std::result_of<F(Args...)>::type;

    // 2. 将任务打包成智能指针管理、可异步获取结果的 packaged_task
    // 使用 std::make_shared 是为了方便后面把它封装成 std::function<void()>
    // 扔进队列
    auto task = std::make_shared<std::packaged_task<return_type()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...));

    // 3. 从打包好的任务中，提取出专属的 future 异步快递单，准备返回给主线程
    std::future<return_type> res = task->get_future();

    {  // === 进入临界区：安全地把任务塞入队列 ===
        std::unique_lock<std::mutex> lock(queue_mtx);

        // 如果线程池已经触发了 stop 停止信号，严禁继续提交新任务！
        if (stop_) {
            throw std::runtime_error("enqueue on stopped ThreadPool");
        }

        // 把 packaged_task 的执行通过 lambda 包装成 void()
        // 形式，塞入标准队列
        tasks.emplace([task]() { (*task)(); });
    }  // === 离开临界区，自动解锁 ===

    // 4. 唤醒一个正在 cv.wait 上深度冬眠的工作线程起来干活！
    cv.notify_one();

    // 5. 把快递单返回给主线程
    return res;
}