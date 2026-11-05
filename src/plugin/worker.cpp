#include "plugin/worker.h"

#include <tuple>
#include <utility>

namespace memoria::plugin
{

Worker::Worker()
: thread_([this](const std::stop_token& stop)
          {
              run(stop);
          })
{
}

Worker::~Worker()
{
    stopping_ = true;
    thread_.request_stop();
}

auto Worker::post(Job job) -> void
{
    {
        const std::scoped_lock lock(mutex_);
        jobs_.push_back(std::move(job));
    }
    wake_.notify_one();
}

auto Worker::stopping() const -> const std::atomic<bool>&
{
    return stopping_;
}

auto Worker::run(const std::stop_token& stop) -> void
{
    while (true)
    {
        Job job;
        {
            std::unique_lock lock(mutex_);
            std::ignore = wake_.wait(lock, stop, [this]
                                     {
                                         return !jobs_.empty();
                                     });
            if (stop.stop_requested())
            {
                jobs_.clear(); // dropped: a stopping worker starts nothing
                return;
            }
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        try
        {
            job(stop);
        }
        catch (const std::exception&) // NOLINT(bugprone-empty-catch): a job that fails ends alone; the next one still runs
        {
        }
    }
}

} // namespace memoria::plugin
