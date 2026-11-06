#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>

namespace memoria
{

// Short jobs the game thread must not wait on: reminders, the chat choice, opening the browser.
class Worker
{
public:
    using Job = std::function<void(const std::stop_token& stop)>;

    Worker();
    ~Worker(); // stops the running job and drops queued ones

    Worker(const Worker&)                    = delete;
    Worker(Worker&&)                         = delete;
    auto operator=(const Worker&) -> Worker& = delete;
    auto operator=(Worker&&) -> Worker&      = delete;

    auto post(Job job) -> void;

private:
    auto run(const std::stop_token& stop) -> void;

    std::mutex                  mutex_;
    std::condition_variable_any wake_;
    std::deque<Job>             jobs_;
    std::jthread                thread_; // last: it starts using the members above
};

} // namespace memoria
