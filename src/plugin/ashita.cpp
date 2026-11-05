#include "plugin/ashita.h"

#include <Commands.h>

#include <climits>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace memoria::plugin
{

auto Plugin::GetName() const -> const char*
{
    return "Memoria";
}

auto Plugin::GetAuthor() const -> const char*
{
    return "sruon";
}

auto Plugin::GetDescription() const -> const char*
{
    return "Records every packet of a retail session to xicap and uploads it to Vault.";
}

auto Plugin::GetLink() const -> const char*
{
    return "https://github.com/ffxi-dev/memoria";
}

auto Plugin::GetVersion() const -> double
{
    return MEMORIA_VERSION;
}

// Last, so the blocked and modified flags see what every other plugin did.
auto Plugin::GetPriority() const -> int32_t
{
    return INT32_MAX;
}

auto Plugin::GetFlags() const -> uint32_t
{
    using enum Ashita::PluginFlags;
    return std::to_underlying(UsePackets) | std::to_underlying(UseCommands) | std::to_underlying(UseDirect3D);
}

auto Plugin::Initialize(IAshitaCore* core, ILogManager* logger, const uint32_t id) -> bool
{
    UNREFERENCED_PARAMETER(id);
    const std::filesystem::path install{ core->GetInstallPath() };
    session_ = std::make_unique<Session>(Host{
        .config   = install / "config" / "memoria",
        .captures = install / "captures" / "memoria",
        .captain  = install / "addons" / "captain",
        .say      = [core](const std::string& text)
        {
            core->GetChatManager()->Writef(1, false, "\x1E\x51[\x1E\x06Memoria\x1E\x51]\x1E\x01 %s", text.c_str());
        },
        .log = [logger](const std::string& text)
        {
            logger->Log(std::to_underlying(Ashita::LogLevel::Info), "Memoria", text.c_str());
        },
    });

    std::optional<LateLogin> late;
    auto*                    party = core->GetMemoryManager()->GetParty();
    if (core->GetMemoryManager()->GetPlayer()->GetLoginStatus() == 2 && party->GetMemberServerId(0) != 0)
    {
        late = LateLogin{ .characterId = party->GetMemberServerId(0), .name = party->GetMemberName(0) };
    }
    session_->start(late);
    return true;
}

auto Plugin::Release() -> void
{
    session_.reset();
}

auto Plugin::HandleIncomingPacket(const uint16_t id, const uint32_t size, const uint8_t* data, uint8_t* modified, const uint32_t sizeChunk, const uint8_t* dataChunk, const bool injected, const bool blocked) -> bool
{
    session_->incoming(id, size, data, modified, sizeChunk, dataChunk, injected, blocked);
    return false;
}

auto Plugin::HandleOutgoingPacket(const uint16_t id, const uint32_t size, const uint8_t* data, uint8_t* modified, const uint32_t sizeChunk, const uint8_t* dataChunk, const bool injected, const bool blocked) -> bool
{
    UNREFERENCED_PARAMETER(id);
    session_->outgoing(size, data, modified, sizeChunk, dataChunk, injected, blocked);
    return false;
}

// Ashita refuses a plugin that asks for Direct3D callbacks and turns the device down.
auto Plugin::Direct3DInitialize(IDirect3DDevice8* device) -> bool
{
    UNREFERENCED_PARAMETER(device);
    return true;
}

auto Plugin::Direct3DBeginScene(const bool isRenderingBackBuffer) -> void
{
    UNREFERENCED_PARAMETER(isRenderingBackBuffer);
    session_->frame();
}

auto Plugin::HandleCommand(const int32_t mode, const char* command, const bool injected) -> bool
{
    UNREFERENCED_PARAMETER(mode);
    UNREFERENCED_PARAMETER(injected);
    std::vector<std::string> args;
    Ashita::Commands::GetCommandArgs(command, &args);
    if (args.empty() || args[0] != "/memoria")
    {
        return false;
    }
    session_->command(args, command);
    return true;
}

} // namespace memoria::plugin

extern "C" auto __stdcall expCreatePlugin(const char* /*args*/) -> IPlugin*
{
    return new memoria::plugin::Plugin();
}

extern "C" auto __stdcall expDestroyPlugin(void* instance) -> void
{
    delete static_cast<memoria::plugin::Plugin*>(instance);
}

extern "C" auto __stdcall expGetInterfaceVersion() -> double
{
    return ASHITA_INTERFACE_VERSION;
}
