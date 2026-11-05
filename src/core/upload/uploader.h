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
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
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

struct UploadStatus
{
    std::string name;
    uint64_t    sent{};
    uint64_t    size{};
    bool        refused{}; // Mogsafe refused it for good; error says why
    bool        cutOff{};  // Mogsafe cut its upload off: bytes past sent stay only on this PC
    bool        foreign{}; // uploaded under another account: not sent while this install keeps its token
    std::string error;
};

// What /memoria status says about uploads, added up over every capture.
struct UploadTotals
{
    uint64_t    behind{};  // bytes Mogsafe lacks, in captures still sending
    uint64_t    kept{};    // bytes only on this PC, in recordings Mogsafe cut off
    int         cut{};     // recordings that keep such bytes
    int         foreign{}; // recordings uploaded under another account
    std::string error;     // the first sending capture's error, if one failed
};

// Refused captures count nowhere: status lists them on their own.
auto totals(const std::vector<UploadStatus>& files) -> UploadTotals;

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
    // At unload, once the recorder closed names: sends their last bytes and closes their uploads, waiting at most limit in
    // all. Sends nothing after.
    auto               finish(std::vector<std::string> names, std::chrono::milliseconds limit) -> void;
    [[nodiscard]] auto uploadAll() -> std::optional<std::string>; // one pass over the folder; the first error, if any

    [[nodiscard]] auto status() const -> std::vector<UploadStatus>;               // every capture in the folder, whichever client sends it
    [[nodiscard]] auto saveError() const -> std::string;                          // why the last save of upload progress failed, if it did
    [[nodiscard]] auto takeUnauthorized() -> bool;                                // true once each time Mogsafe refuses the token on disk, which it then deletes
    [[nodiscard]] auto ended(const std::string& name) const -> bool;              // Mogsafe ended this file's upload before it closed: its bytes never go again
    [[nodiscard]] auto recordingId(const std::string& name) const -> std::string; // Mogsafe's id for this file, the end of its upload's URL; empty before it has one

private:
    auto               run() -> void;
    auto               step(const std::filesystem::path& path, UploadFile& f, std::chrono::milliseconds idle) -> std::optional<std::string>;
    auto               sendLast(const std::string& name) -> void;
    auto               send(const std::filesystem::path& path, UploadFile& f, uint64_t offset, uint64_t size) -> std::optional<std::string>;
    auto               close(const std::filesystem::path& path, UploadFile& f, uint64_t size) -> std::optional<std::string>;
    auto               applyEnded(UploadFile& f, const HttpResponse& res) -> void;
    auto               writeRest(const std::filesystem::path& path, UploadFile& f) -> std::optional<std::string>;
    auto               load() -> void;
    [[nodiscard]] auto save() -> std::optional<std::string>; // why uploads.txt was not replaced, if it was not
    auto               saved() const -> UploadFiles;
    auto               snapshot() const -> std::pair<UploadFiles, bool>;
    auto               file(const std::string& name) const -> UploadFile; // from memory while this client sends, else from the state file

    UploadOptions         options_;
    std::unique_ptr<Http> http_;
    Tus                   tus_; // upload thread only, but for takeUnauthorized()

    mutable std::mutex mutex_;
    UploadFiles        files_;
    std::string        saveError_;
    bool               dirty_{}; // upload thread only: memory holds what the state file lacks
    bool               last_{};  // upload thread only: finish() is sending the last bytes
    std::atomic<bool>  loaded_{ false };

    std::thread              thread_;
    std::mutex               wakeMutex_;
    std::condition_variable  wake_;
    std::stop_source         stop_;
    std::atomic<bool>        finishing_{ false };
    std::vector<std::string> finish_;     // the files finish() names, under wakeMutex_
    bool                     finished_{}; // under wakeMutex_
    std::atomic<bool>        uploading_{ false };
};

} // namespace memoria::upload
