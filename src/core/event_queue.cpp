#include "core/event_queue.h"

#include <utility>

namespace memoria
{

EventQueue::EventQueue(size_t maxBytes)
: maxBytes_(maxBytes)
{
}

auto EventQueue::push(Event event) -> bool
{
    const size_t    size = sizeof(Event) + event.data.size();
    std::lock_guard lock(mutex_);
    if (event.kind == Event::Kind::Packet && bytes_ + size > maxBytes_)
    {
        dropped_++;
        return false;
    }
    bytes_ += size;
    events_.push_back(std::move(event));
    return true;
}

auto EventQueue::drain(std::vector<Event>& out) -> void
{
    out.clear();
    std::lock_guard lock(mutex_);
    out.swap(events_);
    bytes_ = 0;
}

auto EventQueue::takeDropped() -> uint64_t
{
    std::lock_guard lock(mutex_);
    return std::exchange(dropped_, 0);
}

} // namespace memoria
