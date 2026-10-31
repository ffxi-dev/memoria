#include "plugin/pol.h"

#include "core/clock.h"
#include "core/event_queue.h"

#include <intrin.h>

#include <array>
#include <cstring>
#include <optional>
#include <vector>

#pragma intrinsic(_ReturnAddress)

namespace memoria
{
namespace
{

constexpr uint8_t IRC     = 2;
constexpr uint8_t PROFILE = 3;

// The profile channel's opcode: what the object is.
enum class Kind : uint16_t
{
    Request      = 1,
    Answer       = 2,
    Object       = 3,
    Checksum     = 4,
    Record       = 5,
    FileData     = 6,
    FileChecksum = 7,
};

struct Site
{
    uint8_t  channel;
    bool     clientToServer;
    uint16_t opcode;
};

// sqCrypt64's call sites in the current retail polcore.dll, by return address
// relative to polcore's base. A polcore update can move them; records from a
// site not listed here are not recorded.
auto site(uint32_t rva) -> std::optional<Site>
{
    const auto profile = [](bool out, Kind kind)
    {
        return Site{ PROFILE, out, static_cast<uint16_t>(kind) };
    };
    switch (rva)
    {
        case 0x000138B5:
            return Site{ IRC, true, 0 };
        case 0x00015ECB:
            return Site{ IRC, false, 0 };
        case 0x0001F530:
            return profile(true, Kind::Request);
        case 0x0001F70D:
            return profile(false, Kind::Answer);
        case 0x0001F8B8:
            return profile(false, Kind::Object);
        case 0x0001F9F1:
            return profile(true, Kind::Object);
        case 0x0001FB1B:
            return profile(false, Kind::Checksum);
        case 0x000217A1:
        case 0x00021923:
            return profile(false, Kind::Record);
        case 0x000263C9:
            return profile(false, Kind::FileData);
        case 0x000264A9:
            return profile(false, Kind::FileChecksum);
        case 0x000269DF:
        case 0x00026FC9:
            return profile(true, Kind::FileData);
        case 0x00026AB0:
        case 0x00027088:
            return profile(true, Kind::FileChecksum);
        default:
            return std::nullopt;
    }
}

// sqCrypt64's prologue and what follows; the first 5 bytes (mov eax,[esp+14h];
// push ebp) are moved to the trampoline.
constexpr std::array<uint8_t, 25> SIGNATURE = {
    0x8B,
    0x44,
    0x24,
    0x14,
    0x55,
    0x56,
    0x57,
    0x85,
    0xC0,
    0x74,
    0x0E,
    0x8B,
    0x44,
    0x24,
    0x1C,
    0x33,
    0xF6,
    0x8B,
    0x78,
    0x58,
    0x8B,
    0x68,
    0x5C,
    0xEB,
    0x0D,
};
constexpr size_t  PATCH_SIZE = 5;
constexpr int32_t MAX_COPY   = 0x200000; // the largest object is a 1 MB settings-backup chunk

using Crypt = void(__cdecl*)(uint8_t* src, uint8_t* dst, int32_t length, void* context, int32_t reset);

Clock                           clock_;
EventQueue                      queue_(16 * 1024 * 1024);
uintptr_t                       base_{};
uintptr_t                       target_{};
uint8_t*                        trampoline_{};
std::array<uint8_t, PATCH_SIZE> original_{};

auto push(const Site& s, const uint8_t* data, int32_t length) -> void
{
    if (data == nullptr || length <= 0 || length > MAX_COPY)
    {
        return;
    }
    queue_.push(Event{
        .kind           = Event::Kind::Packet,
        .timestamp      = clock_.nowMs(),
        .clientToServer = s.clientToServer,
        .channel        = s.channel,
        .opcode         = s.opcode,
        .data           = { data, data + length },
    });
}

// Sends are plaintext going in, receives coming out.
void __cdecl hookedCrypt(uint8_t* src, uint8_t* dst, int32_t length, void* context, int32_t reset)
{
    const auto s = site(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress()) - base_));
    if (s && s->clientToServer)
    {
        push(*s, src, length);
    }
    reinterpret_cast<Crypt>(trampoline_)(src, dst, length, context, reset);
    if (s && !s->clientToServer)
    {
        push(*s, dst, length);
    }
}

auto write(uintptr_t address, const void* bytes, size_t size) -> bool
{
    DWORD protection = 0;
    if (!::VirtualProtect(reinterpret_cast<void*>(address), size, PAGE_EXECUTE_READWRITE, &protection))
    {
        return false;
    }
    std::memcpy(reinterpret_cast<void*>(address), bytes, size);
    ::VirtualProtect(reinterpret_cast<void*>(address), size, protection, &protection);
    ::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(address), size);
    return true;
}

auto jumpTo(uintptr_t from, uintptr_t to) -> std::array<uint8_t, PATCH_SIZE>
{
    std::array<uint8_t, PATCH_SIZE> jump{ 0xE9 };
    const auto                      relative = static_cast<int32_t>(to - (from + PATCH_SIZE));
    std::memcpy(jump.data() + 1, &relative, sizeof(relative));
    return jump;
}

// Committed, readable pages only: polcore unpacks itself while it loads.
auto findSignature(uintptr_t base) -> uintptr_t
{
    const auto* dos  = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt   = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    const auto  end  = base + nt->OptionalHeader.SizeOfImage;
    uintptr_t   addr = base;
    while (addr < end)
    {
        MEMORY_BASIC_INFORMATION info{};
        if (::VirtualQuery(reinterpret_cast<void*>(addr), &info, sizeof(info)) == 0)
        {
            break;
        }
        const auto regionEnd = (std::min)(reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize, end);
        const bool readable  = info.State == MEM_COMMIT && (info.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_READONLY | PAGE_READWRITE)) != 0;
        if (readable)
        {
            for (auto p = addr; p + SIGNATURE.size() <= regionEnd; ++p)
            {
                if (std::memcmp(reinterpret_cast<const void*>(p), SIGNATURE.data(), SIGNATURE.size()) == 0)
                {
                    return p;
                }
            }
        }
        addr = regionEnd;
    }
    return 0;
}

// True once hooked; call again until then.
auto tryInstall() -> bool
{
    for (const auto* name : { "polcore.dll", "polcoreeu.dll" })
    {
        const auto module = reinterpret_cast<uintptr_t>(::GetModuleHandleA(name));
        if (module == 0)
        {
            continue;
        }
        const auto address = findSignature(module);
        if (address == 0)
        {
            return false; // loaded, not unpacked yet
        }
        auto* trampoline = static_cast<uint8_t*>(::VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (trampoline == nullptr)
        {
            return false;
        }
        std::memcpy(trampoline, reinterpret_cast<const void*>(address), PATCH_SIZE);
        const auto back = jumpTo(reinterpret_cast<uintptr_t>(trampoline) + PATCH_SIZE, address + PATCH_SIZE);
        std::memcpy(trampoline + PATCH_SIZE, back.data(), back.size());
        std::memcpy(original_.data(), reinterpret_cast<const void*>(address), PATCH_SIZE);
        base_           = module;
        trampoline_     = trampoline;
        const auto jump = jumpTo(address, reinterpret_cast<uintptr_t>(&hookedCrypt));
        if (!write(address, jump.data(), jump.size()))
        {
            ::VirtualFree(trampoline, 0, MEM_RELEASE);
            trampoline_ = nullptr;
            return false;
        }
        target_ = address;
        return true;
    }
    return false;
}

// The trampoline stays: a call may still be running through it.
auto removeHook() -> void
{
    if (target_ != 0)
    {
        write(target_, original_.data(), original_.size());
        target_ = 0;
    }
}

} // namespace

auto PolPlugin::GetName() const -> const char*
{
    return "Memoria";
}

auto PolPlugin::GetAuthor() const -> const char*
{
    return "sruon";
}

auto PolPlugin::GetDescription() const -> const char*
{
    return "Records PlayOnline IRC and profile traffic for Memoria.";
}

auto PolPlugin::GetLink() const -> const char*
{
    return "https://github.com/ffxi-dev/memoria";
}

auto PolPlugin::GetVersion() const -> double
{
    return MEMORIA_VERSION;
}

auto PolPlugin::GetFlags() const -> uint32_t
{
    return 0;
}

auto PolPlugin::Initialize(IAshitaCore* core, ILogManager* logger, uint32_t id) -> bool
{
    UNREFERENCED_PARAMETER(core);
    UNREFERENCED_PARAMETER(logger);
    UNREFERENCED_PARAMETER(id);
    // POL plugins load before polcore is loaded and unpacked: retry until the hook takes.
    watcher_ = std::thread([this]
                           {
                               while (!stop_ && !tryInstall())
                               {
                                   std::this_thread::sleep_for(std::chrono::milliseconds(10));
                               }
                           });
    return true;
}

auto PolPlugin::Release() -> void
{
    stop_ = true;
    if (watcher_.joinable())
    {
        watcher_.join();
    }
    removeHook();
}

} // namespace memoria

extern "C" __declspec(dllexport) void __cdecl memoriaPolTake(memoria::PolEach each, void* context)
{
    std::vector<memoria::Event> events;
    memoria::queue_.drain(events);
    for (const auto& e : events)
    {
        const memoria::PolRecord record{
            .timestamp      = e.timestamp,
            .channel        = e.channel,
            .clientToServer = static_cast<uint8_t>(e.clientToServer),
            .opcode         = e.opcode,
            .size           = static_cast<uint32_t>(e.data.size()),
            .data           = e.data.data(),
        };
        each(context, &record);
    }
}
