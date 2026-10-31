#pragma once

#include "core/http.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace memoria
{

struct UploadOptions
{
    std::filesystem::path     dir;         // captures/memoria
    std::string               server;      // e.g. https://mogsafe.sruon.dev
    HttpHeaders               credentials; // sent with every request, never shown
    std::chrono::milliseconds every{ 5000 };
    std::chrono::milliseconds idle{ 30000 }; // try to close a file unchanged for this long
    // Clients sharing an install share the captures folder; only the one this
    // says yes to uploads. Empty: always yes.
    std::function<bool()> mayRun;
};

struct UploadStatus
{
    std::string name;
    uint64_t    sent{};
    uint64_t    size{};
    bool        closed{};
    std::string error; // the last error or refusal, if any
};

// Sends growing .xicap files to Mogsafe as they are written, resuming from the
// server's offset after any outage or restart. A port of Mogsafe's
// upload/upload.go: it knows nothing about the format, and the server decides
// when a capture is finished.
class Uploader
{
public:
    Uploader(UploadOptions options, std::unique_ptr<Http> http);
    ~Uploader();

    Uploader(const Uploader&)                    = delete;
    auto operator=(const Uploader&) -> Uploader& = delete;

    // Passes on a thread of its own, waiting longer after each failed pass.
    auto start() -> void;

    // Looks at every capture once; returns the first error.
    auto pass() -> std::optional<std::string>;

    [[nodiscard]] auto status() const -> std::vector<UploadStatus>;

private:
    struct File
    {
        std::string                           url;
        bool                                  closed{};
        uint64_t                              refused{}; // size at the last refused close
        uint64_t                              size{};
        uint64_t                              sent{};
        std::chrono::steady_clock::time_point seen{}; // when size last changed
        std::string                           error;
    };

    auto step(const std::filesystem::path& path, File& f) -> std::optional<std::string>;
    auto send(const std::filesystem::path& path, File& f, uint64_t offset, uint64_t size) -> std::optional<std::string>;
    auto close(const std::filesystem::path& path, File& f, uint64_t size) -> std::optional<std::string>;
    auto request(const std::string& method, const std::string& url, HttpHeaders headers, std::span<const uint8_t> body = {}) -> std::expected<HttpResponse, std::string>;
    auto load() -> void;
    auto save() -> void;

    UploadOptions         options_;
    std::unique_ptr<Http> http_;

    mutable std::mutex          mutex_; // guards files_ for status()
    std::map<std::string, File> files_; // by file name
    bool                        loaded_{ false };

    std::thread             thread_;
    std::mutex              wakeMutex_;
    std::condition_variable wake_;
    std::atomic<bool>       stopping_{ false };
};

} // namespace memoria
