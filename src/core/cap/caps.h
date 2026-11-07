#pragma once

#include "core/cap/cap.h"
#include "core/capture/recorder.h"

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace memoria::cap
{

// One capture at a time: markers in the recording, and a copy of its own that never uploads.
class Caps
{
public:
    // captainStop stops a capture another addon started with a folder, such as captain's /cap stop; empty if none.
    Caps(std::filesystem::path dir, std::string captainStop);

    // args is what follows "cap start", in UTF-8. The error is the refusal to print.
    auto start(capture::Recorder& recorder, std::string_view args, uint64_t nowMs, std::string_view character) -> std::expected<Cap, std::string>;
    auto stop(capture::Recorder& recorder, uint64_t nowMs) -> std::optional<Cap>;
    // A logout or another character: the copy closes, and the recording gets no stop marker.
    auto end(capture::Recorder& recorder, uint64_t nowMs) -> std::optional<Cap>;

    [[nodiscard]] auto running() const -> const std::optional<Cap>&;

private:
    std::filesystem::path dir_;
    std::string           captainStop_;
    std::optional<Cap>    running_;
};

} // namespace memoria::cap
