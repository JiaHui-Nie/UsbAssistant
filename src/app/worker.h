// 最简单的串行后台执行器：一个线程 + 任务队列。
// 任务自己负责把结果 PostMessage 回 UI 线程，Worker 不解释任务内容。
//
// 线程只持有 shared_ptr<State>，不持有 Worker 对象本身；因此退出时
// 即使任务还在运行，也可以安全地 detach 线程而不产生 use-after-free。
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace usb::app {

class Worker {
public:
    Worker() = default;
    ~Worker();
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    void Start();

    // wait=false 用于进程退出：丢弃排队任务、让运行中的任务自行结束，
    // 不阻塞 UI 退出。
    void Stop(bool wait = true);

    void Post(std::function<void()> task);

private:
    struct State {
        std::mutex mutex;
        std::condition_variable condition;
        std::deque<std::function<void()>> tasks;
        bool stopping = false;
    };

    static void Loop(std::shared_ptr<State> state);

    std::shared_ptr<State> state_ = std::make_shared<State>();
    std::mutex lifecycle_mutex_;
    std::thread thread_;
    bool started_ = false;
};

}  // namespace usb::app
