#pragma once

#include "core/http.h"

#include <atomic>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace memoria
{

// What capture uploads and captain imports share: the install's token and pause files, and Mogsafe's tus requests.
class Tus
{
public:
    static constexpr uint64_t CHUNK_SIZE = 1 << 20;

    struct Appended
    {
        uint64_t                    offset{}; // where sending stopped
        std::optional<HttpResponse> answer;   // the response other than 204 that stopped it
    };

    // config holds the token file that authorizes uploads and the paused file that stops them, for every client sharing the install.
    Tus(std::string server, std::filesystem::path config, Http& http);

    // Read before each step and chunk: another client sharing the install may have paused, unlinked, or linked again.
    auto allowed() -> bool;

    // Adds the tus version and the token, and sends to the server's origin whatever origin url names, or a bare path; a 401
    // deletes the token and is an error.
    auto request(const std::string& method, const std::string& url, HttpHeaders headers, std::span<const uint8_t> body = {}) -> std::expected<HttpResponse, std::string>;

    // The upload's URL from a create's Location, on the server's origin.
    [[nodiscard]] auto location(const HttpResponse& res) const -> std::string;

    // Sends path's bytes from offset to size in 1 MiB appends, while go(offset) holds.
    auto append(const std::filesystem::path& path, const std::string& url, uint64_t offset, uint64_t size, const std::function<bool(uint64_t)>& go) -> std::expected<Appended, std::string>;

    [[nodiscard]] auto unauthorized() -> bool; // true once each time Mogsafe refuses the token on disk, which it then deletes

    [[nodiscard]] auto token() const -> const std::string&; // as the last allowed() read it

private:
    [[nodiscard]] auto onServer(std::string_view url) const -> std::string;

    std::string           server_;
    std::filesystem::path config_;
    Http&                 http_;
    std::string           token_;
    std::atomic<bool>     unauthorized_{ false };
};

} // namespace memoria
