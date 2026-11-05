#include "plugin/interruptible.h"

#include <utility>

namespace memoria::plugin
{

Interruptible::Interruptible(std::function<void(void*)> close)
: close_(std::move(close))
{
}

auto Interruptible::run(void* const handle, const std::function<bool()>& f) -> bool
{
    {
        const std::scoped_lock lock(mutex_);
        if (aborted_)
        {
            return false;
        }
        ++waiting_[handle];
    }
    const bool             ok{ f() };
    const std::scoped_lock lock(mutex_);
    if (const auto it = waiting_.find(handle); it != waiting_.end() && --it->second == 0)
    {
        waiting_.erase(it);
    }
    return ok;
}

auto Interruptible::abort() -> void
{
    const std::scoped_lock lock(mutex_);
    aborted_ = true;
    for (const auto& [handle, calls] : waiting_)
    {
        if (closed_.insert(handle).second)
        {
            close_(handle);
        }
    }
}

auto Interruptible::aborted() const -> bool
{
    const std::scoped_lock lock(mutex_);
    return aborted_;
}

auto Interruptible::closed(void* const handle) const -> bool
{
    const std::scoped_lock lock(mutex_);
    return closed_.contains(handle);
}

} // namespace memoria::plugin
