#pragma once

#include "core/capture/event_queue.h"
#include "core/live/live.h"

#include <xicap/writer.h>

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

namespace memoria::capture
{

struct RecorderOptions
{
    std::filesystem::path     dir;
    std::string               captureAgent;
    std::chrono::milliseconds flushEvery{ 5000 };
    std::chrono::milliseconds drainEvery{ 50 };
    size_t                    queueBytes{ size_t{ 64 } * 1024 * 1024 };
    // Asked on the writer thread as each capture opens, for its session and live frames too. Empty redacts.
    std::function<xicap::Chat()> chat;
    // Asked on the writer thread at each flush. True closes the capture, and the next packet starts one that names it.
    std::function<bool(const std::filesystem::path& capture)> ended;
    // Called on the writer thread when writing fails, once until a capture writes again.
    std::function<void(const std::string& why)> failed;
};

struct RecorderStatus
{
    std::filesystem::path capture;
    std::filesystem::path session;
    uint64_t              packets{}; // in the open capture, as are the counts below
    uint64_t              bytes{};
    uint64_t              search{};
    uint64_t              firstMs{};
    uint64_t              lastMs{};
    uint64_t              dropped{};                   // queue overflow, short game packets, and packets with no writable capture
    xicap::Chat           chat{ xicap::Chat::Redact }; // the open capture's
    std::string           error;                       // why the capture stopped, until one writes again
    std::string           sessionError;                // why the last session ended early or never started
};

// Writes queued events on its own thread. A login by another character starts a new capture; the same character
// continues it. A session writes the same packets to a second file until sessionStop.
class Recorder
{
public:
    explicit Recorder(RecorderOptions options);
    ~Recorder();

    Recorder(const Recorder&)                    = delete;
    Recorder(Recorder&&)                         = delete;
    auto operator=(const Recorder&) -> Recorder& = delete;
    auto operator=(Recorder&&) -> Recorder&      = delete;

    // For the game thread: these queue and return.
    auto packet(Event event) -> void;
    auto login(uint64_t timestamp, Login who) -> void;
    auto logout(uint64_t timestamp) -> void;
    auto note(uint64_t timestamp, std::string text) -> void;
    auto sessionStart(uint64_t timestamp, const std::filesystem::path& path) -> void;
    auto sessionStop(uint64_t timestamp) -> void;

    [[nodiscard]] auto status() const -> RecorderStatus;

    auto live() -> live::Feed&
    {
        return live_;
    }

    // Writes what is queued and closes the capture. Returns every capture closed after the stop request, including one a
    // queued login rotated out, so unload can send their last bytes.
    auto stop() -> std::vector<std::filesystem::path>;

private:
    auto run() -> void;
    auto handle(const Event& event) -> void;
    auto metadata(const Login& who, const xicap::CaptureId& previous) const -> xicap::Metadata;
    auto open(uint64_t timestamp, const Login& who) -> void;
    auto write(const Event& event) -> void;
    auto mark(xicap::MarkerKind kind, std::string_view text = {}) -> void;
    auto openSession(const std::filesystem::path& path) -> void;
    auto closeSession() -> void;
    template <typename Op>
    auto onSession(Op op) -> void;
    auto sessionFailed(const std::string& why) -> void;
    auto close() -> void;
    auto finish() -> void;
    auto setError(const std::exception& error) -> void;    // shown in status until a capture writes again
    auto writeFailed(const std::exception& error) -> void; // drops the writer, as a crash would
    auto clearError() -> void;
    auto rollover() -> void;
    auto saveState(bool open) -> void;

    RecorderOptions options_;
    EventQueue      queue_;
    live::Feed      live_;

    std::thread                        thread_;
    std::mutex                         wakeMutex_;
    std::condition_variable            wake_;
    bool                               stopping_{ false };
    std::vector<std::filesystem::path> closedAtStop_; // under wakeMutex_

    // Writer thread only.
    std::unique_ptr<xicap::Writer>        writer_;
    std::filesystem::path                 capture_; // the open capture's path; status_ keeps a copy for readers
    std::unique_ptr<xicap::Writer>        session_;
    std::optional<Login>                  login_; // kept after a write error, to reopen
    std::chrono::steady_clock::time_point reopenAt_{};
    xicap::CaptureId                      previous_{};
    xicap::Chat                           chat_{ xicap::Chat::Redact }; // the open capture's
    std::chrono::steady_clock::time_point lastFlush_{};
    bool                                  failing_{}; // the player was told; cleared once a capture writes again

    mutable std::mutex statusMutex_;
    RecorderStatus     status_;
};

} // namespace memoria::capture
