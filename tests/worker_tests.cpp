#include "core/worker.h"
#include "support.h"

#include <future>

using namespace memoria;
using namespace memoria::test;

TEST_CASE("a stopping worker drops the jobs still queued")
{
    std::promise<void> started;
    std::atomic<int>   ran{ 0 };
    {
        Worker worker;
        worker.post([&](const std::stop_token& stop)
                    {
                        std::mutex                  mutex;
                        std::condition_variable_any wake;
                        std::unique_lock            lock(mutex);
                        started.set_value();
                        std::ignore = wake.wait(lock, stop, []
                                                {
                                                    return false;
                                                });
                    });
        started.get_future().wait();
        worker.post([&](const std::stop_token&)
                    {
                        ++ran;
                    });
    }
    CHECK(ran == 0);
}

TEST_CASE("a worker runs its jobs in order, and one that throws does not stop the next")
{
    std::vector<int>   order;
    std::promise<void> done;
    {
        Worker worker;
        worker.post([&](const std::stop_token&)
                    {
                        order.push_back(1);
                        throw std::runtime_error("no disk");
                    });
        worker.post([&](const std::stop_token&)
                    {
                        order.push_back(2);
                        done.set_value();
                    });
        done.get_future().wait();
    }
    CHECK(order == std::vector{ 1, 2 });
}
