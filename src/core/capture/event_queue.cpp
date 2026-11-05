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
    const size_t    size{ sizeof(Event) + event.data.size() + event.text.size() };
    std::lock_guard lock(mutex_);
    if (event.kind == EventKind::Packet && bytes_ + size > maxBytes_)
    {
        dropped_++;
        return false;
    }
    bytes_ += size;
    events_.push_back(std::move(event));
    return true;
}

// out is the batch the writer took last time: its bytes count until this call frees them, so the queue and the batch
// in hand together stay under maxBytes.
auto EventQueue::drain(std::vector<Event>& out) -> void
{
    out.clear();
    std::lock_guard lock(mutex_);
    out.swap(events_);
    bytes_ -= std::exchange(taken_, bytes_ - taken_);
}

auto EventQueue::takeDropped() -> uint64_t
{
    std::lock_guard lock(mutex_);
    return std::exchange(dropped_, 0);
}

} // namespace memoria::capture
