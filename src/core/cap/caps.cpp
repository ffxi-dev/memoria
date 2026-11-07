#include "core/cap/caps.h"

#include <format>
#include <utility>

namespace memoria::cap
{

Caps::Caps(std::filesystem::path dir, std::string captainStop)
: dir_{ std::move(dir) }
, captainStop_{ std::move(captainStop) }
{
}

auto Caps::start(capture::Recorder& recorder, const std::string_view args, const uint64_t nowMs, const std::string_view character) -> std::expected<Cap, std::string>
{
    if (running_)
    {
        return std::unexpected(std::format("A capture is running: {}. {} first.", running_->name, running_->stopWith));
    }
    auto [name, path] = parseStart(args);
    Cap cap{
        .id        = newId(),
        .name      = name.empty() ? defaultName(nowMs) : std::move(name),
        .stopWith  = path.empty() || captainStop_.empty() ? "/memoria cap stop" : captainStop_,
        .startMs   = nowMs,
        .recording = recorder.status().capture,
    };
    const auto file = path.empty() ? freeFile(dir_, cap.name, character, nowMs) : fileIn(std::filesystem::path{ std::u8string{ path.begin(), path.end() } });
    recorder.sessionStart(nowMs, file);
    recorder.note(nowMs, startMarker(cap));
    running_ = cap;
    return cap;
}

auto Caps::stop(capture::Recorder& recorder, const uint64_t nowMs) -> std::optional<Cap>
{
    auto cap = std::exchange(running_, std::nullopt);
    if (cap)
    {
        recorder.note(nowMs, stopMarker(*cap));
        recorder.sessionStop(nowMs);
    }
    return cap;
}

auto Caps::end(capture::Recorder& recorder, const uint64_t nowMs) -> std::optional<Cap>
{
    auto cap = std::exchange(running_, std::nullopt);
    if (cap)
    {
        recorder.sessionStop(nowMs);
    }
    return cap;
}

auto Caps::running() const -> const std::optional<Cap>&
{
    return running_;
}

} // namespace memoria::cap
