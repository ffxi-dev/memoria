#pragma once

#include "core/event_queue.h"
#include "core/live.h"

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

namespace memoria
{

struct RecorderOptions
{
    std::filesystem::path     dir;
    std::string               captureAgent;
    std::chrono::milliseconds flushEvery{ 5000 };
    std::chrono::milliseconds drainEvery{ 50 };
    size_t                    queueBytes = 64 * 1024 * 1024;
    // Asked on the writer thread as each capture opens: the policy for it, its session file, and its live frames. Empty: the default.
    std::function<xicap::ExclusionPolicy()> policy;
    // Asked at each flush, on the writer thread: true when the server ended the open capture's upload. The capture then
    // closes, and the next packet starts a new one that names it, as after a write error.
    std::function<bool(const std::filesystem::path& capture)> ended;
    // Called on the writer thread when the capture stops being written, once until a capture writes again.
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
    uint64_t              dropped{};    // the queue's, and those that came while no capture could be written
    std::string           error;        // why the capture stopped, until one writes again
    std::string           sessionError; // why the last session ended early or never started
};

// Writes queued events to xicap captures on a thread of its own. A login of another character starts a new capture; the same character logging back in continues it. A session is a second capture of the same packets, written to a path of the caller's choosing until sessionStop.
class Recorder
{
public:
    explicit Recorder(RecorderOptions options);
    ~Recorder();

    Recorder(const Recorder&)                    = delete;
    Recorder(Recorder&&)                         = delete;
    auto operator=(const Recorder&) -> Recorder& = delete;
    auto operator=(Recorder&&) -> Recorder&      = delete;

    // These only queue: safe on the game thread.
    auto packet(Event event) -> void;
    auto login(uint64_t timestamp, Login who) -> void;
    auto logout(uint64_t timestamp) -> void;
    auto note(uint64_t timestamp, std::string text) -> void;
    auto sessionStart(uint64_t timestamp, const std::filesystem::path& path) -> void;
    auto sessionStop(uint64_t timestamp) -> void;

    [[nodiscard]] auto status() const -> RecorderStatus;

    auto live() -> LiveFeed&
    {
        return live_;
    }

    auto stop() -> void; // writes what is queued and closes the capture

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
    auto sessionFailed(const std::exception& error) -> void;
    auto close() -> void;
    auto finish() -> void;
    auto fail(const std::exception& error) -> void;
    auto broken(const std::exception& error) -> void;
    auto rollover() -> void;
    auto saveState(bool open) -> void;

    RecorderOptions options_;
    EventQueue      queue_;
    LiveFeed        live_;

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
    xicap::ExclusionPolicy                policy_ = xicap::defaultExclusionPolicy(); // the open capture's
    std::chrono::steady_clock::time_point lastFlush_{};
    bool                                  failing_{}; // the player was told; cleared once a capture writes again

    mutable std::mutex statusMutex_;
    RecorderStatus     status_;
};

} // namespace memoria
