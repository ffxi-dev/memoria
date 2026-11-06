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

class Tus
{
public:
    static constexpr uint64_t CHUNK_SIZE = 1 << 20;

    struct Appended
    {
        uint64_t                    offset{}; // where sending stopped
        std::optional<HttpResponse> answer;   // the non-204 response that stopped it
    };

    Tus(std::string server, std::filesystem::path config, Http& http);

    // Rereads the pause and token: another client sharing the install may have changed them.
    auto allowed() -> bool;

    // Sends url's path to the server's origin, whatever origin url names. A 401 deletes the token and returns REFUSED.
    auto request(const std::string& method, const std::string& url, HttpHeaders headers, std::span<const uint8_t> body = {}) -> std::expected<HttpResponse, std::string>;

    [[nodiscard]] auto location(const HttpResponse& res) const -> std::string;

    // Sends bytes offset to size in CHUNK_SIZE appends while go(offset) holds.
    auto append(const std::filesystem::path& path, const std::string& url, uint64_t offset, uint64_t size, const std::function<bool(uint64_t)>& go) -> std::expected<Appended, std::string>;

    [[nodiscard]] auto takeUnauthorized() -> bool; // true once per 401 that deleted the token

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
