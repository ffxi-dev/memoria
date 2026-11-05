#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>

namespace memoria::plugin
{

// One thread for the session's short jobs: reminders, the chat choice, opening the browser. A packet hook only queues a
// job, and never joins a thread. At unload the worker stops: a job sees its stop token, and stopping() for a scan.
class Worker
{
public:
    using Job = std::function<void(const std::stop_token& stop)>;

    Worker();
    ~Worker(); // stops the job running, drops the rest, and joins

    Worker(const Worker&)                    = delete;
    Worker(Worker&&)                         = delete;
    auto operator=(const Worker&) -> Worker& = delete;
    auto operator=(Worker&&) -> Worker&      = delete;

    auto post(Job job) -> void;

    [[nodiscard]] auto stopping() const -> const std::atomic<bool>&;

private:
    auto run(const std::stop_token& stop) -> void;

    std::mutex                  mutex_;
    std::condition_variable_any wake_;
    std::deque<Job>             jobs_;
    std::atomic<bool>           stopping_{ false };
    std::jthread                thread_; // last: started once the rest is ready
};

} // namespace memoria::plugin
