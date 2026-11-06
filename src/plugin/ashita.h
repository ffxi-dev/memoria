#pragma once

#include "plugin/session.h"

#include <Ashita.h>

#include <memory>

namespace memoria::plugin
{

class Plugin final : public IPlugin
{
public:
    [[nodiscard]] auto GetName() const -> const char* override;
    [[nodiscard]] auto GetAuthor() const -> const char* override;
    [[nodiscard]] auto GetDescription() const -> const char* override;
    [[nodiscard]] auto GetLink() const -> const char* override;
    [[nodiscard]] auto GetVersion() const -> double override;
    [[nodiscard]] auto GetPriority() const -> int32_t override;
    [[nodiscard]] auto GetFlags() const -> uint32_t override;

    auto Initialize(IAshitaCore* core, ILogManager* logger, uint32_t id) -> bool override;
    auto Release() -> void override;

    auto HandleCommand(int32_t mode, const char* command, bool injected) -> bool override;
    auto HandleIncomingPacket(uint16_t id, uint32_t size, const uint8_t* data, uint8_t* modified, uint32_t sizeChunk, const uint8_t* dataChunk, bool injected, bool blocked) -> bool override;
    auto HandleOutgoingPacket(uint16_t id, uint32_t size, const uint8_t* data, uint8_t* modified, uint32_t sizeChunk, const uint8_t* dataChunk, bool injected, bool blocked) -> bool override;
    auto Direct3DInitialize(IDirect3DDevice8* device) -> bool override;
    auto Direct3DBeginScene(bool isRenderingBackBuffer) -> void override;

private:
    std::unique_ptr<Session> session_;
};

} // namespace memoria::plugin
