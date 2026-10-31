#pragma once

#include "core/event_queue.h"

#include <xicap/writer.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace memoria
{

struct RecorderOptions
{
    std::filesystem::path     dir; // captures/memoria in the Ashita install
    xicap::SecretSeed         seed{};
    std::string               captureAgent;
    std::string               clientVersion;
    std::chrono::milliseconds flushEvery{ 5000 };
    std::chrono::milliseconds drainEvery{ 50 };
    size_t                    queueBytes = 64 * 1024 * 1024;
};

// Who logged in, as the plugin read it at the zone-in after character select.
struct Login
{
    xicap::CharacterIdentity character{};
    xicap::Network           network    = xicap::Network::Unknown;
    bool                     midSession = false; // the plugin loaded after the login
    std::array<char, 32>     clientVersion{};    // as the client told the lobby; empty: RecorderOptions'
};

struct RecorderStatus
{
    std::filesystem::path capture; // empty when nothing is open
    std::filesystem::path session; // empty when no session is open
    uint64_t              packets{};
    uint64_t              dropped{};
    std::string           error;       // the last write error, if any
    xicap::PublicKey      publicKey{}; // the install's, once a capture has opened
};

// Writes what the game thread queues to xicap captures, on a thread of its
// own. A capture starts at a login of a character other than the open
// capture's; the same character logging back in continues the file with a
// logout and a login marker. Nothing but a login of another character, a
// stop, or a write error ends a capture. See xicap's FORMAT.md, Writer rules.
class Recorder
{
public:
    explicit Recorder(RecorderOptions options);
    ~Recorder();

    Recorder(const Recorder&)                    = delete;
    auto operator=(const Recorder&) -> Recorder& = delete;

    // Game thread: these only queue, and never wait on the writer.
    auto packet(Event event) -> void;
    auto login(uint64_t timestamp, const Login& who) -> void;
    auto logout(uint64_t timestamp) -> void;
    auto note(uint64_t timestamp, std::string_view text) -> void;

    // A session: a second capture of the same packets, from now until
    // sessionStop, written to path. Captain's /cap start and /cap stop put
    // one in each session folder. Only while a capture is open; a new
    // character's capture ends it.
    auto sessionStart(uint64_t timestamp, const std::filesystem::path& path) -> void;
    auto sessionStop(uint64_t timestamp) -> void;

    [[nodiscard]] auto status() const -> RecorderStatus;

    // Writes what is queued, closes the capture, and stops the thread.
    auto stop() -> void;

private:
    auto run() -> void;
    auto handle(const Event& event) -> void;
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
    std::optional<Login>                  login_; // who the open capture, or the broken one, belongs to
    std::chrono::steady_clock::time_point reopenAt_{};
    xicap::CaptureId                      previous_{};
    std::chrono::steady_clock::time_point lastFlush_{};

    mutable std::mutex statusMutex_;
    RecorderStatus     status_;
};

} // namespace memoria
