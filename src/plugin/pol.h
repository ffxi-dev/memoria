#pragma once

#include <Ashita.h>

#include <atomic>
#include <cstdint>
#include <thread>

namespace memoria
{

// One PlayOnline record, handed from the POL plugin to the game plugin. Plain
// data: the two may be separate copies of this DLL, each with its own heap.
struct PolRecord
{
    uint64_t       timestamp;
    uint8_t        channel; // xicap channel 2 (IRC) or 3 (profile)
    uint8_t        clientToServer;
    uint16_t       opcode;
    uint32_t       size;
    const uint8_t* data;
};

using PolEach = void(__cdecl*)(void* context, const PolRecord* record);

// The POL plugin: Ashita loads it from polplugins/ before the PlayOnline
// login, which game plugins never see. It hooks polcore's sqCrypt64, through
// which every IRC line and profile-server object passes in plaintext, and
// queues what it sees until the game plugin, which owns the recorder, takes it.
// Credits: polsniff.
class PolPlugin final : public IPolPlugin
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
    std::atomic<bool> stop_{ false };
    std::thread       watcher_;
};

} // namespace memoria

// Hands every queued record to each, oldest first, and forgets them.
extern "C" __declspec(dllexport) void __cdecl memoriaPolTake(memoria::PolEach each, void* context);
