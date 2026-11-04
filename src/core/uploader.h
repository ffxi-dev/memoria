#pragma once

#include "core/http.h"
#include "core/tus.h"

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
    std::filesystem::path     config; // its token file authorizes uploads and a paused file there stops them, for every client sharing the install
    std::chrono::milliseconds every{ 5000 };
    std::chrono::milliseconds idle{ 30000 }; // a file unchanged this long gets a close
    std::function<bool()>     mayRun;        // clients sharing an install upload through one of them
};

struct UploadStatus
{
    std::string name;
    uint64_t    sent{};
    uint64_t    size{};
    bool        refused{}; // Mogsafe refused it for good; error says why
    bool        ended{};   // Mogsafe cut its upload off: bytes past sent stay only on this PC
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

    [[nodiscard]] auto status() const -> std::vector<UploadStatus>;  // every capture in the folder, whichever client sends it
    [[nodiscard]] auto uploading() const -> bool;                    // false while another client sharing the folder sends its captures
    [[nodiscard]] auto saveError() const -> std::string;             // why the last save of upload progress failed, if it did
    [[nodiscard]] auto unauthorized() -> bool;                       // true once each time Mogsafe refuses the token on disk, which it then deletes
    [[nodiscard]] auto ended(const std::string& name) const -> bool; // Mogsafe ended this file's upload before it closed: its bytes never go again

private:
    struct File
    {
        std::string                           url;
        bool                                  closed{};
        bool                                  ended{};   // Mogsafe answered 410: cut off or refused
        bool                                  cutOff{};  // ended as cut off: Vault keeps the first sent bytes
        uint64_t                              refused{}; // size at the last refused close
        std::string                           reason;    // why Mogsafe refused it for good, kept in the state file
        uint64_t                              size{};
        uint64_t                              sent{};
        std::chrono::steady_clock::time_point seen{}; // when size last changed
        std::string                           error;
    };

    auto run() -> void;
    auto step(const std::filesystem::path& path, File& f) -> std::optional<std::string>;
    auto send(const std::filesystem::path& path, File& f, uint64_t offset, uint64_t size) -> std::optional<std::string>;
    auto close(const std::filesystem::path& path, File& f, uint64_t size) -> std::optional<std::string>;
    auto end(File& f, const HttpResponse& res) -> void;
    auto allowed() -> bool;
    auto request(const std::string& method, const std::string& url, HttpHeaders headers, std::span<const uint8_t> body = {}) -> std::expected<HttpResponse, std::string>;
    auto load() -> void;
    auto save() -> std::optional<std::string>;
    auto saved() const -> std::map<std::string, File>;

    UploadOptions         options_;
    std::unique_ptr<Http> http_;
    Tus                   tus_; // upload thread only, but for unauthorized()

    mutable std::mutex          mutex_;
    std::map<std::string, File> files_; // by file name
    std::string                 saveError_;
    bool                        dirty_{}; // upload thread only: memory holds what the state file lacks
    std::atomic<bool>           loaded_{ false };

    std::thread             thread_;
    std::mutex              wakeMutex_;
    std::condition_variable wake_;
    std::atomic<bool>       stopping_{ false };
    std::atomic<bool>       uploading_{ false };
};

} // namespace memoria
