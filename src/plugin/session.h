#pragma once

#include "core/chat.h"
#include "core/clock.h"
#include "core/importer.h"
#include "core/link.h"
#include "core/recorder.h"
#include "core/uploader.h"
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

namespace memoria
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

    auto start(std::optional<LateLogin> late) -> void;

    auto incoming(uint16_t id, uint32_t size, const uint8_t* data, const uint8_t* modified, uint32_t sizeChunk, const uint8_t* dataChunk, bool injected, bool blocked) -> void;
    auto outgoing(uint32_t size, const uint8_t* data, const uint8_t* modified, uint32_t sizeChunk, const uint8_t* dataChunk, bool injected, bool blocked) -> void;
    auto frame() -> void;

    // args[0] is the command's name; line is the command as typed, for a path that holds spaces.
    auto command(const std::vector<std::string>& args, std::string_view line) -> void;

    auto recordSearch(bool clientToServer, const uint8_t* data, uint32_t size) -> void;

private:
    struct ChunkKey
    {
        uintptr_t               pointer;
        uint32_t                size;
        std::array<uint8_t, 16> head;

        auto operator==(const ChunkKey&) const -> bool = default;
    };

    auto record(bool clientToServer, uint32_t size, const uint8_t* data, const uint8_t* modified, uint32_t sizeChunk, const uint8_t* dataChunk, bool injected, bool blocked) -> void;
    auto say(const std::string& text) -> void;
    auto post(std::string text) -> void; // like say, from any thread; the line prints on the next frame
    auto status() -> void;
    auto startUploads() -> void;
    auto link() -> void;
    auto unlink(const std::string& why) -> void;
    auto linked() const -> std::optional<Linked>; // read each time: clients sharing the install share its token
    auto greet() -> void;
    auto remind() -> void;
    auto offerImport() -> void;
    auto startImport(bool asked) -> void;
    auto importLine() const -> std::string;
    auto hello() -> std::string;
    auto stopLive() -> void;
    auto refreshChat() -> void; // asks Mogsafe whether the linked account saves chat, off the game thread

    Host                               host_;
    std::string                        server_ = "https://mogsafe.sruon.dev";
    std::mutex                         inboxMutex_;
    std::vector<std::string>           inbox_;
    std::optional<Linked>              arrived_; // from the link thread
    Clock                              clock_;
    std::unique_ptr<Recorder>          recorder_;
    std::unique_ptr<Uploader>          uploader_;
    std::unique_ptr<Live>              live_; // after recorder_, whose feed it reads
    std::vector<std::unique_ptr<Live>> closing_;
    HANDLE                             uploadMutex_{ nullptr };
    std::unique_ptr<Importer>          importer_;
    std::atomic<bool>                  reminding_{ false };
    std::string                        startError_;
    std::string                        searchError_;
    std::string                        characterError_; // why the content id and world are missing
    xicap::Character                   character_;
    bool                               loggedIn_{ false };
    bool                               privacyShown_{ false }; // the first /memoria link of a session shows the policy's URL
    std::optional<uint64_t>            sessionInVault_;        // a /cap session's start while linked: Vault keeps it as a range of the recording, with no file here
    uint32_t                           loggedInAs_{ 0 };
    std::optional<ChunkKey>            lastChunk_[2];      // the last chunk seen in each direction
    std::atomic<bool>                  saveChat_{ false }; // for the next capture: the linked account's choice
    std::atomic<bool>                  fetchingChat_{ false };
    std::jthread                       chatFetch_;
    std::jthread                       reminder_;
    std::jthread                       linker_; // last: destroyed first, before what its thread uses
};

} // namespace memoria
