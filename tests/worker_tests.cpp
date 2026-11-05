#include "plugin/interruptible.h"
#include "plugin/worker.h"
#include "support.h"

#include <future>

using namespace memoria;
using namespace memoria::test;

TEST_CASE("a stopping worker drops the jobs still queued")
{
    std::promise<void> started;
    std::atomic<int>   ran{ 0 };
    {
        plugin::Worker worker;
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
        plugin::Worker worker;
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

namespace
{

// Handles as WinHTTP's: closing one ends the call waiting on it.
struct Handles
{
    std::mutex              mutex;
    std::condition_variable wake;
    std::set<void*>         closed;
    std::map<void*, int>    closes;

    auto close(void* h) -> void
    {
        {
            const std::scoped_lock lock(mutex);
            closed.insert(h);
            ++closes[h];
        }
        wake.notify_all();
    }

    // A call that waits until its handle closes, as a receive on a quiet socket does.
    auto waitOn(void* h) -> bool
    {
        std::unique_lock lock(mutex);
        wake.wait(lock, [&]
                  {
                      return closed.contains(h);
                  });
        return false;
    }
};

} // namespace

TEST_CASE("an abort ends the calls in flight by closing their handles, and refuses every call after it")
{
    Handles               handles;
    plugin::Interruptible calls([&](void* h)
                                {
                                    handles.close(h);
                                });
    int                   ws{};
    int                   idle{};
    void* const           socket = &ws;
    void* const           other  = &idle;
    std::latch            waiting(2);
    auto                  receive = std::async(std::launch::async, [&]
                                               {
                                  return calls.run(socket, [&]
                                                   {
                                                       waiting.count_down();
                                                       return handles.waitOn(socket);
                                                   });
                                               });
    auto                  send    = std::async(std::launch::async, [&]
                                               {
                               return calls.run(socket, [&]
                                                {
                                                    waiting.count_down();
                                                    return handles.waitOn(socket);
                                                });
                                               });
    waiting.wait();
    CHECK(calls.run(other, []
                    {
                        return true; // a call that ends on its own leaves its handle open
                    }));
    calls.abort();
    CHECK(receive.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    CHECK(send.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    CHECK(handles.closes[socket] == 1); // two calls on one handle: closed once
    CHECK(!handles.closed.contains(other));
    CHECK(calls.closed(socket));
    CHECK(!calls.closed(other));
    CHECK(calls.aborted());

    bool ran{ false };
    CHECK(!calls.run(other, [&]
                     {
                         ran = true;
                         return true;
                     }));
    CHECK(!ran); // no call starts after an abort, so no closed handle is ever used again
}
