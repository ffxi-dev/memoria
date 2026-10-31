#pragma once

#include "core/event_queue.h"

#include <xicap/writer.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace memoria
{

struct RecorderOptions
{
    std::filesystem::path     dir;
    xicap::SecretSeed         seed{};
    std::string               captureAgent;
    std::chrono::milliseconds flushEvery{ 5000 };
    std::chrono::milliseconds drainEvery{ 50 };
    size_t                    queueBytes = 64 * 1024 * 1024;
};

struct RecorderStatus
{
    std::filesystem::path capture;
    std::filesystem::path session;
    uint64_t              packets{};
    uint64_t              dropped{};
    std::string           error;
    xicap::PublicKey      publicKey{};
};

// Writes queued events to xicap captures on a thread of its own. A login of another character starts a new capture; the same character logging back in continues it. A session is a second capture of the same packets, written to a path of the caller's choosing until sessionStop.
class Recorder
{
public:
    explicit Recorder(RecorderOptions options);
    ~Recorder();

    Recorder(const Recorder&)                    = delete;
    auto operator=(const Recorder&) -> Recorder& = delete;

    // These only queue: safe on the game thread.
    auto packet(Event event) -> void;
    auto login(uint64_t timestamp, Login who) -> void;
    auto logout(uint64_t timestamp) -> void;
    auto note(uint64_t timestamp, std::string text) -> void;
    auto sessionStart(uint64_t timestamp, const std::filesystem::path& path) -> void;
    auto sessionStop(uint64_t timestamp) -> void;

    [[nodiscard]] auto status() const -> RecorderStatus;

    auto stop() -> void; // writes what is queued and closes the capture

private:
    auto run() -> void;
    auto handle(const Event& event) -> void;
    auto config(const Login& who, xicap::StartContext start, const xicap::CaptureId& previous) const -> xicap::WriterConfig;
    auto open(uint64_t timestamp, const Login& who) -> void;
    auto write(const Event& event) -> void;
    auto mark(xicap::MarkerKind kind, std::string_view text = {}) -> void;
    auto openSession(const std::filesystem::path& path) -> void;
    auto closeSession() -> void;
    auto close() -> void;
    auto fail(const std::exception& error) -> void;
    auto broken(const std::exception& error) -> void;
    auto saveState(bool open) -> void;

    RecorderOptions options_;
    EventQueue      queue_;

    std::thread             thread_;
    std::mutex              wakeMutex_;
    std::condition_variable wake_;
    bool                    stopping_{ false };

    // Writer thread only.
    std::unique_ptr<xicap::Writer>        writer_;
    std::unique_ptr<xicap::Writer>        session_;
    std::optional<Login>                  login_; // kept after a write error, to reopen
    std::chrono::steady_clock::time_point reopenAt_{};
    xicap::CaptureId                      previous_{};
    std::chrono::steady_clock::time_point lastFlush_{};

    mutable std::mutex statusMutex_;
    RecorderStatus     status_;
};

} // namespace memoria
