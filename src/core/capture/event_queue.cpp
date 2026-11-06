#include "core/capture/event_queue.h"

#include <utility>

namespace memoria::capture
{

EventQueue::EventQueue(const size_t maxBytes)
: maxBytes_(maxBytes)
{
}

auto EventQueue::push(Event event) -> bool
{
    const size_t           size{ sizeof(Event) + event.data.size() + event.text.size() };
    const std::scoped_lock lock(mutex_);
    if (event.kind == EventKind::Packet && bytes_ + size > maxBytes_)
    {
        dropped_++;
        return false;
    }
    bytes_ += size;
    events_.push_back(std::move(event));
    return true;
}

// out holds the last batch: its bytes count until this call frees them.
auto EventQueue::drain(std::vector<Event>& out) -> void
{
    out.clear();
    const std::scoped_lock lock(mutex_);
    out.swap(events_);
    bytes_ -= taken_;
    taken_ = bytes_;
}

auto EventQueue::takeDropped() -> uint64_t
{
    const std::scoped_lock lock(mutex_);
    return std::exchange(dropped_, 0);
}

} // namespace memoria::capture
