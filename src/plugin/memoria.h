#pragma once

#include "core/clock.h"
#include "core/link.h"
#include "core/recorder.h"
#include "core/uploader.h"

#include <Ashita.h>

#include <array>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace memoria
{

class Plugin final : public IPlugin
{
public:
    auto GetName() const -> const char* override;
    auto GetAuthor() const -> const char* override;
    auto GetDescription() const -> const char* override;
    auto GetLink() const -> const char* override;
    auto GetVersion() const -> double override;
    auto GetPriority() const -> int32_t override;
    auto GetFlags() const -> uint32_t override;

    auto Initialize(IAshitaCore* core, ILogManager* logger, uint32_t id) -> bool override;
    auto Release() -> void override;

    auto HandleCommand(int32_t mode, const char* command, bool injected) -> bool override;
    auto HandleIncomingPacket(uint16_t id, uint32_t size, const uint8_t* data, uint8_t* modified, uint32_t sizeChunk, const uint8_t* dataChunk, bool injected, bool blocked) -> bool override;
    auto HandleOutgoingPacket(uint16_t id, uint32_t size, const uint8_t* data, uint8_t* modified, uint32_t sizeChunk, const uint8_t* dataChunk, bool injected, bool blocked) -> bool override;
    auto Direct3DInitialize(IDirect3DDevice8* device) -> bool override;
    auto Direct3DBeginScene(bool isRenderingBackBuffer) -> void override;

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
    auto network() -> xicap::Network;
    auto say(const std::string& text) -> void;
    auto post(std::string text) -> void; // say, from any thread: printed on the next frame
    auto status() -> void;
    auto startUploads() -> void;
    auto link() -> void;
    auto unlink(const std::string& why) -> void;
    auto remind() -> void;

    IAshitaCore*              core_{ nullptr };
    std::filesystem::path     install_;
    xicap::SecretSeed         seed_{};
    std::string               server_ = "https://mogsafe.sruon.dev";
    std::optional<Linked>     link_;
    std::mutex                inboxMutex_;
    std::vector<std::string>  inbox_;
    std::optional<Linked>     arrived_; // from the link thread
    ILogManager*              log_{ nullptr };
    Clock                     clock_;
    std::unique_ptr<Recorder> recorder_;
    std::unique_ptr<Uploader> uploader_;
    HANDLE                    uploadMutex_{ nullptr };
    std::string               startError_;
    std::string               uploadsOff_;
    std::string               searchError_;
    std::string               lobbyError_;
    bool                      loggedIn_{ false };
    uint32_t                  loggedInAs_{ 0 };
    std::optional<ChunkKey>   lastChunk_[2]; // per direction
    std::jthread              linker_;       // last: destroyed first, before what its thread uses
};

} // namespace memoria
