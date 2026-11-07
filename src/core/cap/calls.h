#pragma once

#include "core/cap/cap.h"
#include "core/cap/enums/outcome.h"
#include "core/http.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

namespace memoria::cap
{

struct CallOptions
{
    std::string                                         server;
    std::filesystem::path                               config;
    std::function<std::string(const std::string& file)> recordingId; // Mogsafe's id for a recording file, empty until it has one
    std::chrono::milliseconds                           every{ 10000 };
    int                                                 tries{ 30 };
};

// Tells Mogsafe when each capture starts and stops, in order, on a thread of its own. A call that keeps failing is
// dropped: Mogsafe makes or closes the capture from the markers when it reads the recording.
class Calls
{
public:
    Calls(CallOptions options, std::unique_ptr<Http> http);
    ~Calls();

    Calls(const Calls&)                    = delete;
    Calls(Calls&&)                         = delete;
    auto operator=(const Calls&) -> Calls& = delete;
    auto operator=(Calls&&) -> Calls&      = delete;

    auto started(const Cap& cap, uint32_t characterId) -> void;
    auto stopped(const Cap& cap, uint64_t endMs) -> void;
    auto ended(const Cap& cap) -> void; // cut short: never stopped

private:
    struct Pending
    {
        Cap                     cap;
        uint32_t                characterId{};
        bool                    opened{}; // Mogsafe has the row
        bool                    cut{};
        std::optional<uint64_t> endMs;
        int                     tries{};
    };

    auto               run(const std::stop_token& stop) -> void;
    auto               send(const Pending& call) -> Outcome;
    auto               find(const Cap& cap) -> Pending*; // under mutex_
    [[nodiscard]] auto ready() const -> bool;            // under mutex_

    CallOptions                 options_;
    std::unique_ptr<Http>       http_;
    std::mutex                  mutex_;
    std::condition_variable_any wake_;
    std::deque<Pending>         pending_;
    std::jthread                thread_; // last: it starts using the members above
};

} // namespace memoria::cap
