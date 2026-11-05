#pragma once

#include "core/http.h"
#include "core/tus.h"
#include "core/upload/upload_state.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace memoria::upload
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

// xicap's header, which Mogsafe wants whole in a file's first append.
inline constexpr uint64_t CAPTURE_HEADER_SIZE = 160;

struct UploadStatus
{
    std::string name;
    uint64_t    sent{};
    uint64_t    size{};
    bool        refused{}; // Mogsafe refused it for good; error says why
    bool        ended{};   // Mogsafe cut its upload off: bytes past sent stay only on this PC
    std::string error;
};

// Sends what each .xicap in the folder gained since Mogsafe's offset, and closes its upload with the file's hash once the capture is complete.
class Uploader
{
public:
    Uploader(UploadOptions options, std::unique_ptr<Http> http);
    ~Uploader();

    Uploader(const Uploader&)                    = delete;
    Uploader(Uploader&&)                         = delete;
    auto operator=(const Uploader&) -> Uploader& = delete;
    auto operator=(Uploader&&) -> Uploader&      = delete;

    auto start() -> void;
    // At unload, once the recorder closed name: sends its last bytes and closes its upload, waiting at most limit. Sends nothing after.
    auto finish(const std::string& name, std::chrono::milliseconds limit) -> void;
    auto pass() -> std::optional<std::string>; // the first error, if any

    [[nodiscard]] auto status() const -> std::vector<UploadStatus>;               // every capture in the folder, whichever client sends it
    [[nodiscard]] auto saveError() const -> std::string;                          // why the last save of upload progress failed, if it did
    [[nodiscard]] auto unauthorized() -> bool;                                    // true once each time Mogsafe refuses the token on disk, which it then deletes
    [[nodiscard]] auto ended(const std::string& name) const -> bool;              // Mogsafe ended this file's upload before it closed: its bytes never go again
    [[nodiscard]] auto recordingId(const std::string& name) const -> std::string; // Mogsafe's id for this file, the end of its upload's URL; empty before it has one

private:
    auto run() -> void;
    auto step(const std::filesystem::path& path, UploadFile& f, std::chrono::milliseconds idle) -> std::optional<std::string>;
    auto last(const std::string& name) -> void;
    auto send(const std::filesystem::path& path, UploadFile& f, uint64_t offset, uint64_t size) -> std::optional<std::string>;
    auto close(const std::filesystem::path& path, UploadFile& f, uint64_t size) -> std::optional<std::string>;
    auto end(UploadFile& f, const HttpResponse& res) -> void;
    auto rest(const std::filesystem::path& path, UploadFile& f) -> std::optional<std::string>;
    auto load() -> void;
    auto save() -> std::optional<std::string>;
    auto saved() const -> UploadFiles;
    auto file(const std::string& name) const -> UploadFile; // from memory while this client sends, else from the state file

    UploadOptions         options_;
    std::unique_ptr<Http> http_;
    Tus                   tus_; // upload thread only, but for unauthorized()

    mutable std::mutex mutex_;
    UploadFiles        files_;
    std::string        saveError_;
    bool               dirty_{}; // upload thread only: memory holds what the state file lacks
    bool               last_{};  // upload thread only: finish() is sending the last bytes
    std::atomic<bool>  loaded_{ false };

    std::thread             thread_;
    std::mutex              wakeMutex_;
    std::condition_variable wake_;
    std::atomic<bool>       stopping_{ false };
    std::atomic<bool>       finishing_{ false };
    std::string             finish_;     // the file finish() names, under wakeMutex_
    bool                    finished_{}; // under wakeMutex_
    std::atomic<bool>       uploading_{ false };
};

} // namespace memoria::upload
