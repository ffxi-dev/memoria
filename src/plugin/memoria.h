#pragma once

#include "core/clock.h"
#include "core/recorder.h"
#include "core/uploader.h"

#include <Ashita.h>

#include <array>
#include <memory>
#include <optional>

namespace memoria
{

// The Ashita plugin: hands every game packet and every login and logout to the
// recorder. Its hooks run after every other plugin's and only copy.
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
    auto Direct3DInitialize(IDirect3DDevice8* device) -> bool override;
    auto Direct3DBeginScene(bool isRenderingBackBuffer) -> void override;
    auto HandleOutgoingPacket(uint16_t id, uint32_t size, const uint8_t* data, uint8_t* modified, uint32_t sizeChunk, const uint8_t* dataChunk, bool injected, bool blocked) -> bool override;

private:
    auto record(bool clientToServer, uint16_t id, uint32_t size, const uint8_t* data, const uint8_t* modified, uint32_t sizeChunk, const uint8_t* dataChunk, bool injected, bool blocked) -> void;
    auto network() -> xicap::Network;
    auto takePol() -> void;

public:
    auto recordSearch(bool clientToServer, const uint8_t* data, uint32_t size) -> void;

private:
    auto say(const std::string& text) -> void;
    auto startUploads(const std::filesystem::path& install) -> void;

    IAshitaCore*              core_{ nullptr };
    ILogManager*              log_{ nullptr };
    Clock                     clock_;
    std::unique_ptr<Recorder> recorder_;
    std::unique_ptr<Uploader> uploader_;
    std::string               uploadsOff_; // why uploads are off, when they are
    HANDLE                    uploadMutex_{ nullptr };
    std::string               searchError_; // why search is not recorded, when it is not
    std::string               lobbyError_;  // why the client version cannot be read
    std::wstring              polModule_;   // polplugins/Memoria.dll
    bool                      polFound_{ false };
    uint64_t                  polTaken_{ 0 };
    std::string               startError_;
    bool                      loggedIn_{ false };
    uint32_t                  loggedInAs_{ 0 }; // the character of the last login
    // Per direction: the chunk the last packet came in, to tell when a new
    // chunk starts.
    struct ChunkKey
    {
        uintptr_t               pointer;
        uint32_t                size;
        std::array<uint8_t, 16> head;
    };
    std::optional<ChunkKey> lastChunk_[2];
};

} // namespace memoria
