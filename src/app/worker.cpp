#include "app/worker.h"

#include <utility>

namespace usb::app {

Worker::~Worker() { Stop(true); }

void Worker::Start() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    if (started_) return;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->stopping = false;
    }
    started_ = true;
    const std::shared_ptr<State> state = state_;
    thread_ = std::thread([state]() { Loop(state); });
}

void Worker::Stop(bool wait) {
    std::thread thread;
    {
        std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
        if (!started_) {
            // 清掉 Start 之前可能排队的任务。
            std::deque<std::function<void()>> discarded;
            std::lock_guard<std::mutex> lock(state_->mutex);
            discarded.swap(state_->tasks);
            return;
        }
        started_ = false;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->stopping = true;
        }
        state_->condition.notify_all();
        thread = std::move(thread_);
    }

    if (!thread.joinable()) return;
    if (wait) {
        thread.join();
    } else {
        thread.detach();
    }
}

void Worker::Post(std::function<void()> task) {
    if (!task) return;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->stopping) return;
        state_->tasks.push_back(std::move(task));
    }
    state_->condition.notify_one();
}

void Worker::Loop(std::shared_ptr<State> state) {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->condition.wait(
                lock, [&state]() { return state->stopping || !state->tasks.empty(); });
            if (state->stopping) return;
            task = std::move(state->tasks.front());
            state->tasks.pop_front();
        }
        task();
    }
}

}  // namespace usb::app
