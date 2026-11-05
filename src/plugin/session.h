#pragma once

#include "core/account/chat.h"
#include "core/account/link.h"
#include "core/captain/importer.h"
#include "core/capture/clock.h"
#include "core/capture/recorder.h"
#include "core/install.h"
#include "core/upload/uploader.h"
#include "core/worker.h"
#include "plugin/live.h"
#include "plugin/text.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace memoria::plugin
{

inline constexpr auto CAPTURE_AGENT = "Memoria " MEMORIA_VERSION_TEXT;

// What a framework gives the session: where its files go, and how it talks to the player.
struct Host
{
    std::filesystem::path                   config;   // the token and settings
    std::filesystem::path                   captures; // captures and upload state
    std::filesystem::path                   captain;  // captain's addon folder, for /memoria import
    std::function<void(const std::string&)> say;      // one chat line, on the game thread
    std::function<void(const std::string&)> log;      // one line to the framework's log
};

// The character already in game when Memoria loads, if the host can tell.
struct LateLogin
{
    uint32_t    characterId{};
    std::string name;
};

// Everything Memoria does, whichever framework loaded it. Every call comes from the game thread.
class Session final
{
public:
    explicit Session(Host host);
    ~Session(); // stops every thread and unhooks the client

    Session(const Session&)                    = delete;
    Session(Session&&)                         = delete;
    auto operator=(const Session&) -> Session& = delete;
    auto operator=(Session&&) -> Session&      = delete;

    auto start(const std::optional<LateLogin>& late) -> void;

    auto incoming(uint16_t id, uint32_t size, const uint8_t* data, const uint8_t* modified, uint32_t sizeChunk, const uint8_t* dataChunk, bool injected, bool blocked) -> void;
    auto outgoing(uint32_t size, const uint8_t* data, const uint8_t* modified, uint32_t sizeChunk, const uint8_t* dataChunk, bool injected, bool blocked) -> void;
    auto frame() -> void;

    // args[0] is the command's name; line is the command as typed, for a path that holds spaces.
    auto command(const std::vector<std::string>& args, std::string_view line) -> void;

    auto recordSearch(bool clientToServer, const uint8_t* data, uint32_t size) -> void;

private:
    // A /memoria session started while linked: Vault keeps it as a range of the recording, with no file here.
    struct VaultSession
    {
        uint64_t              fromMs{};
        std::filesystem::path capture; // the recording it started in; empty before the first capture opened
    };

    struct ChunkKey
    {
        uintptr_t               pointer;
        uint32_t                size;
        std::array<uint8_t, 16> head;

        auto operator==(const ChunkKey&) const -> bool = default;
    };

    auto enter(capture::Login who) -> void;
    auto record(xicap::PacketDirection direction, uint32_t size, const uint8_t* data, const uint8_t* modified, uint32_t sizeChunk, const uint8_t* dataChunk, bool injected, bool blocked) -> void;
    auto say(const std::string& text) -> void;
    auto post(std::string text) -> void; // like say, from any thread; the line prints on the next frame
    auto sayStatus() -> void;
    auto sayUploads() -> void;
    auto startSession(std::string_view path) -> void;
    auto stopSession() -> void;
    auto importCaptain() -> void;
    auto goLive() -> void;
    auto unlinkInstall() -> void;
    auto setUploads(bool on) -> void;
    auto startUploads() -> void;
    auto link() -> void;
    auto waitForLink(const std::stop_token& stop) -> void;
    auto cancelLink() -> bool;                     // true when a link was being made
    auto unlinked(const std::string& why) -> void; // the token is gone: stops live and says so
    auto linked() const -> std::optional<Linked>;  // read each time: clients sharing the install share its token
    auto greet() -> void;
    auto remind() -> void;
    auto offerImport(const std::stop_token& stop) -> void;
    auto startImport(bool asked) -> void;
    auto importLine() const -> std::string;
    auto hello() -> std::string;
    auto stopLive() -> void;
    auto refreshChat() -> void; // asks Mogsafe whether the linked account saves chat, off the game thread

    Host                                   host_;
    std::string                            server_{ "https://mogsafe.sruon.dev" };
    std::mutex                             inboxMutex_;
    std::vector<std::string>               inbox_;
    std::optional<Linked>                  arrived_;   // from the link thread
    std::mutex                             linkMutex_; // the link thread saves a token only while no unlink cancels it
    std::atomic<bool>                      linking_{ false };
    capture::Clock                         clock_;
    std::unique_ptr<capture::Recorder>     recorder_;
    std::unique_ptr<upload::Uploader>      uploader_;
    std::unique_ptr<Live>                  live_; // after recorder_, whose feed it reads
    std::vector<std::unique_ptr<Live>>     closing_;
    HANDLE                                 uploadMutex_{ nullptr };
    HANDLE                                 importMutex_{ nullptr };
    std::unique_ptr<captain::Importer>     importer_;
    std::atomic<bool>                      reminding_{ false };
    std::string                            startError_;
    std::string                            searchError_;
    std::string                            characterError_; // why the content id and world are missing
    xicap::Character                       character_;
    bool                                   loggedIn_{ false };
    bool                                   privacyShown_{ false }; // the first /memoria link of a session shows the policy's URL
    std::optional<VaultSession>            sessionInVault_;
    uint32_t                               loggedInAs_{ 0 };
    std::array<std::optional<ChunkKey>, 2> lastChunk_; // the last chunk seen in each direction
    account::ChatChoice                    chatChoice_;
    std::atomic<bool>                      chatQueued_{ false }; // a refresh waits on the worker
    Worker                                 worker_;              // after what its jobs use
    std::jthread                           linker_;              // last: destroyed first, before what its thread uses
};

} // namespace memoria::plugin
