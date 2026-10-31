#pragma once

#include "core/http.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
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
    std::filesystem::path     dir;
    std::string               server;
    HttpHeaders               credentials;
    std::chrono::milliseconds every{ 5000 };
    std::chrono::milliseconds idle{ 30000 }; // a file unchanged this long gets a close
    std::function<bool()>     mayRun;        // clients sharing an install upload through one of them
};

struct UploadStatus
{
    std::string name;
    uint64_t    sent{};
    uint64_t    size{};
    bool        closed{};
    std::string error;
};

// A port of Mogsafe's upload/upload.go: sends what each .xicap gained since the server's offset, and closes it with its hash once the server accepts it as complete.
class Uploader
{
public:
    Uploader(UploadOptions options, std::unique_ptr<Http> http);
    ~Uploader();

    Uploader(const Uploader&)                    = delete;
    auto operator=(const Uploader&) -> Uploader& = delete;

    auto start() -> void;
    auto pass() -> std::optional<std::string>; // the first error, if any

    [[nodiscard]] auto status() const -> std::vector<UploadStatus>;
    [[nodiscard]] auto unauthorized() const -> bool; // Mogsafe refused the credentials: stopped

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

    auto run() -> void;
    auto step(const std::filesystem::path& path, File& f) -> std::optional<std::string>;
    auto send(const std::filesystem::path& path, File& f, uint64_t offset, uint64_t size) -> std::optional<std::string>;
    auto close(const std::filesystem::path& path, File& f, uint64_t size) -> std::optional<std::string>;
    auto request(const std::string& method, const std::string& url, HttpHeaders headers, std::span<const uint8_t> body = {}) -> std::expected<HttpResponse, std::string>;
    auto load() -> void;
    auto save() -> void;

    UploadOptions         options_;
    std::unique_ptr<Http> http_;

    mutable std::mutex          mutex_;
    std::map<std::string, File> files_; // by file name
    bool                        loaded_{ false };

    std::thread             thread_;
    std::mutex              wakeMutex_;
    std::condition_variable wake_;
    std::atomic<bool>       stopping_{ false };
    std::atomic<bool>       unauthorized_{ false };
};

} // namespace memoria
