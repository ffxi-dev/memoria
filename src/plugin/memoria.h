#pragma once

#include <Ashita.h>

namespace memoria
{

// The Ashita plugin. For now it loads, says which version it is, and unloads;
// recording follows (vault openspec add-memoria, group 2).
class Plugin final : public IPlugin
{
public:
    auto GetName() const -> const char* override;
    auto GetAuthor() const -> const char* override;
    auto GetDescription() const -> const char* override;
    auto GetLink() const -> const char* override;
    auto GetVersion() const -> double override;
    auto GetFlags() const -> uint32_t override;

    auto Initialize(IAshitaCore* core, ILogManager* logger, uint32_t id) -> bool override;
    auto Release() -> void override;

private:
    ILogManager* log_{ nullptr };
};

} // namespace memoria
