#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <set>

namespace memoria::plugin
{

// Blocking calls on handles that another thread must be able to end at once, as WinHTTP's are: closing a handle ends
// the calls waiting on it. abort() closes every handle a call is waiting on and refuses each call after it, so a
// handle it closed is never passed to a call again.
class Interruptible
{
public:
    explicit Interruptible(std::function<void(void*)> close);

    // f's answer, or false without running f once aborted.
    auto run(void* handle, const std::function<bool()>& f) -> bool;
    auto abort() -> void;

    [[nodiscard]] auto aborted() const -> bool;
    [[nodiscard]] auto closed(void* handle) const -> bool; // abort() closed it: its owner releases it, never closes it

private:
    std::function<void(void*)> close_;
    mutable std::mutex         mutex_;
    std::map<void*, int>       waiting_; // calls in flight, by handle
    std::set<void*>            closed_;
    bool                       aborted_{ false };
};

} // namespace memoria::plugin
