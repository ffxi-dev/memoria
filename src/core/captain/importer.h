#pragma once

#include "core/captain/captain.h"
#include "core/captain/enums/outcome.h"
#include "core/http.h"
#include "core/tus.h"

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
#include <stop_token>
#include <string>
#include <thread>

namespace memoria::captain
{

struct ImportOptions
{
    std::filesystem::path            captures; // <Ashita>/addons/captain/captures
    std::filesystem::path            uploaded; // <Ashita>/addons/captain/captures-uploaded
    std::filesystem::path            state;    // <Ashita>/captures/memoria: imports.txt and the staged logs
    std::string                      server;
    std::filesystem::path            config;
    std::function<void(std::string)> say;
    std::chrono::milliseconds        retry{ 5000 };
    std::chrono::seconds             quiet{ std::chrono::minutes(10) };
    std::string                      mutex; // Windows: a named mutex held for the run, so clients sharing the install import one at a time
};

struct ImportProgress
{
    size_t      index{}; // counts from 1; 0 while the scan runs
    size_t      count{};
    std::string character;
    uint64_t    sent{};
    uint64_t    size{}; // of the staged log; 0 until staged
};

// /memoria import: uploads captain's captures to Mogsafe as captain imports, one at a time, and moves each one Mogsafe holds to captures-uploaded.
class Importer
{
public:
    Importer(ImportOptions options, std::unique_ptr<Http> http);
    ~Importer(); // cancels the request in flight and joins

    Importer(const Importer&)                    = delete;
    Importer(Importer&&)                         = delete;
    auto operator=(const Importer&) -> Importer& = delete;
    auto operator=(Importer&&) -> Importer&      = delete;

    // An import was asked for and not finished.
    static auto pending(const std::filesystem::path& state) -> bool;

    auto start(bool asked = true) -> void; // run() on its own thread, below normal priority; a resume (not asked) is silent when another client holds the run
    auto run() -> void;

    [[nodiscard]] auto finished() const -> bool;
    [[nodiscard]] auto progress() const -> ImportProgress;
    [[nodiscard]] auto unauthorized() -> bool; // true once when Mogsafe refused the token, which is then deleted

private:
    struct Entry
    {
        std::string crc; // 8 hex digits
        uint64_t    size{};
        std::string url;
    };

    auto upload(const CaptainCapture& c, const std::string& prefix) -> Outcome;
    auto moved(const CaptainCapture& c, const std::string& prefix, const std::string& done, Outcome outcome) -> Outcome;
    auto ready() -> bool;
    auto wait(std::chrono::milliseconds time) -> void;
    auto say(const std::string& text) -> void;
    auto load() -> void;
    auto save() -> std::optional<std::string>; // why imports.txt was not replaced, if it was not

    ImportOptions                options_;
    std::unique_ptr<Http>        http_;
    Tus                          tus_;
    std::map<std::string, Entry> entries_; // by folder; importer thread only

    mutable std::mutex      mutex_;
    std::condition_variable wake_;
    std::string             character_;

    std::atomic<size_t>   index_{ 0 };
    std::atomic<size_t>   count_{ 0 };
    std::atomic<uint64_t> sent_{ 0 };
    std::atomic<uint64_t> size_{ 0 };
    std::stop_source      stop_;
    std::atomic<bool>     finished_{ false };
    std::thread           thread_;
};

} // namespace memoria::captain
