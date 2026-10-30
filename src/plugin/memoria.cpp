#include "plugin/memoria.h"

#include <xicap/constants.h>

namespace memoria
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
    return 0.1;
}

auto Plugin::GetFlags() const -> uint32_t
{
    return static_cast<uint32_t>(Ashita::PluginFlags::None);
}

auto Plugin::Initialize(IAshitaCore* core, ILogManager* logger, const uint32_t id) -> bool
{
    UNREFERENCED_PARAMETER(core);
    UNREFERENCED_PARAMETER(id);
    log_ = logger;
    log_->Logf(static_cast<uint32_t>(Ashita::LogLevel::Info), "Memoria", "Memoria %.1f loaded, xicap format %d.%d", GetVersion(), xicap::VERSION_MAJOR, xicap::VERSION_MINOR);
    return true;
}

auto Plugin::Release() -> void
{
    if (log_)
    {
        log_->Logf(static_cast<uint32_t>(Ashita::LogLevel::Info), "Memoria", "Memoria unloaded");
    }
}

} // namespace memoria

extern "C" __declspec(dllexport) IPlugin* __stdcall expCreatePlugin(const char* args)
{
    UNREFERENCED_PARAMETER(args);
    return new memoria::Plugin();
}

extern "C" __declspec(dllexport) double __stdcall expGetInterfaceVersion()
{
    return ASHITA_INTERFACE_VERSION;
}
