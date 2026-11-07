#include "core/cap/calls.h"

#include "core/install.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <span>
#include <utility>

namespace memoria::cap
{

Calls::Calls(CallOptions options, std::unique_ptr<Http> http)
: options_{ std::move(options) }
, http_{ std::move(http) }
, thread_{ [this](const std::stop_token& stop)
           {
               run(stop);
           } }
{
}

Calls::~Calls()
{
    thread_.request_stop();
    http_->cancel();
    thread_.join();
}

auto Calls::started(const Cap& cap, const uint32_t characterId) -> void
{
    {
        const std::scoped_lock lock{ mutex_ };
        pending_.push_back({ .cap = cap, .characterId = characterId });
    }
    wake_.notify_all();
}

// A stop whose start was dropped still goes: Mogsafe may have made the capture from its marker by then.
auto Calls::stopped(const Cap& cap, const uint64_t endMs) -> void
{
    {
        const std::scoped_lock lock{ mutex_ };
        if (auto* call = find(cap))
        {
            call->endMs = endMs;
        }
        else
        {
            pending_.push_back({ .cap = cap, .opened = true, .endMs = endMs });
        }
    }
    wake_.notify_all();
}

auto Calls::ended(const Cap& cap) -> void
{
    {
        const std::scoped_lock lock{ mutex_ };
        if (auto* call = find(cap))
        {
            call->cut = true;
        }
    }
    wake_.notify_all();
}

auto Calls::find(const Cap& cap) -> Pending*
{
    const auto it = std::ranges::find(pending_, cap.id, [](const Pending& p)
                                      {
                                          return p.cap.id;
                                      });
    return it == pending_.end() ? nullptr : &*it;
}

auto Calls::ready() const -> bool
{
    if (pending_.empty())
    {
        return false;
    }
    const auto& front = pending_.front();
    return !front.opened || front.endMs || front.cut;
}

// Only this thread pops, so front stays put while the call is out: the others push to the back or set fields under the lock.
auto Calls::run(const std::stop_token& stop) -> void
{
    std::unique_lock lock{ mutex_ };
    while (wake_.wait(lock, stop, [this]
                      {
                          return ready();
                      }) &&
           !stop.stop_requested())
    {
        auto& front = pending_.front();
        if (front.opened && front.cut)
        {
            pending_.pop_front();
            continue;
        }
        const auto call = front;
        lock.unlock();
        auto outcome{ Outcome::Drop };
        try
        {
            outcome = send(call);
        }
        catch (const std::exception&) // NOLINT(bugprone-empty-catch): the markers cover a call that never went
        {
        }
        lock.lock();
        if (outcome == Outcome::Retry && ++front.tries < options_.tries)
        {
            wake_.wait_for(lock, stop, options_.every, []
                           {
                               return false;
                           });
            continue;
        }
        if (outcome == Outcome::Done && !front.opened)
        {
            front.opened = true;
            front.tries  = 0;
            continue;
        }
        pending_.pop_front();
    }
}

auto Calls::send(const Pending& call) -> Outcome
{
    if (paused(options_.config))
    {
        return Outcome::Retry;
    }
    const auto link = loadLink(options_.config);
    if (!link)
    {
        return Outcome::Drop;
    }
    std::string    url;
    nlohmann::json body;
    if (!call.opened)
    {
        if (call.cap.recording.empty())
        {
            return Outcome::Drop;
        }
        const auto recording = options_.recordingId(call.cap.recording.filename().string());
        if (recording.empty())
        {
            return Outcome::Retry;
        }
        url  = options_.server + "/v1/caps";
        body = { { "id", call.cap.id }, { "name", call.cap.name }, { "recording", recording }, { "character_id", call.characterId }, { "start_ms", call.cap.startMs } };
    }
    else
    {
        url  = std::format("{}/v1/caps/{}/stop", options_.server, call.cap.id);
        body = { { "end_ms", call.endMs.value_or(0) } };
    }
    const auto text = body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    const auto res  = http_->request("POST", url, { { "Authorization", "Bearer " + link->token }, { "Content-Type", "application/json" } }, std::span{ reinterpret_cast<const uint8_t*>(text.data()), text.size() });
    if (!res || res->status == 404 || res->status == 429 || res->status >= 500)
    {
        return Outcome::Retry;
    }
    return res->status / 100 == 2 ? Outcome::Done : Outcome::Drop;
}

} // namespace memoria::cap
