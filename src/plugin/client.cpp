#include "plugin/client.h"

#include <Ashita.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <optional>
#include <tuple>
#include <vector>

namespace memoria::client
{
namespace
{

auto read32(const uintptr_t address) -> uint32_t
{
    uint32_t v{};
    std::memcpy(&v, reinterpret_cast<const void*>(address), sizeof(v));
    return v;
}

auto write32(const uintptr_t address, const uint32_t value) -> bool
{
    DWORD protection{ 0 };
    if (!::VirtualProtect(reinterpret_cast<void*>(address), sizeof(value), PAGE_EXECUTE_READWRITE, &protection))
    {
        return false;
    }
    std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(value));
    ::VirtualProtect(reinterpret_cast<void*>(address), sizeof(value), protection, &protection);
    ::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(address), sizeof(value));
    return true;
}

struct Patch
{
    uintptr_t operand; // a call's rel32
    uint32_t  backup;
};

// Points the call at operand to target, if it calls expected.
auto redirect(const uintptr_t operand, const uintptr_t expected, const uintptr_t target) -> std::optional<Patch>
{
    const Patch patch{ operand, read32(operand) };
    if (operand + 4 + patch.backup != expected || !write32(operand, static_cast<uint32_t>(target - (operand + 4))))
    {
        return std::nullopt;
    }
    return patch;
}

} // namespace

auto find(const char* pattern) -> uintptr_t
{
    const auto base = reinterpret_cast<uintptr_t>(::GetModuleHandleA("FFXiMain.dll"));
    if (base == 0)
    {
        return 0;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    return Ashita::Memory::FindPattern(base, nt->OptionalHeader.SizeOfImage, pattern, 0, 0);
}

auto serverKind() -> xicap::ServerKind
{
    // Square Enix's retail zone servers, 124.150.152.0/21.
    constexpr uint32_t RETAIL_FIRST = 0x7C969800;
    constexpr uint32_t RETAIL_LAST  = 0x7C969FFF;

    // The zone server's IPv4 address (credits: atom0s, via captain).
    static const uintptr_t sig = find("8B0D????????8D04808B8481????????C3");
    if (sig == 0)
    {
        return xicap::ServerKind::Unknown;
    }
    try
    {
        const auto ip = std::byteswap(read32(read32(read32(sig + 2))));
        if (ip == 0)
        {
            return xicap::ServerKind::Unknown;
        }
        if (ip >= RETAIL_FIRST && ip <= RETAIL_LAST)
        {
            return xicap::ServerKind::Retail;
        }
        return xicap::ServerKind::Private;
    }
    catch (...) // /EHa: a bad read is an access violation
    {
        return xicap::ServerKind::Unknown;
    }
}

namespace search
{
namespace
{

// All cdecl: each ends in a plain ret.
using Encrypt  = int(__cdecl*)(void* context, uint8_t* packet, int size);
using Decrypt  = bool(__cdecl*)(void* context, uint8_t* packet);
using NextPage = int(__cdecl*)(void* connection);

constexpr uint32_t MAX_SIZE = 0x1000;

// The next page request is encrypted once per connection and resent as is: this is its plaintext.
constexpr std::array<uint8_t, 16> NEXT_PAGE = { 0x10, 0x00, 0x00, 0x00, 0x49, 0x58, 0x46, 0x46, 0x10, 0x00, 0x80, 0x10, 0x00, 0x00, 0x00, 0x00 };

Sink               sink_{};
Encrypt            encrypt_{};
Decrypt            decrypt_{};
NextPage           nextPage_{};
uintptr_t          connection_{};
std::vector<Patch> patches_;

auto record(const bool clientToServer, const uint8_t* data, const uint32_t size) -> void
{
    if (sink_ && size >= 12 && size <= MAX_SIZE)
    {
        sink_(clientToServer, data, size);
    }
}

auto __cdecl encryptHook(void* context, uint8_t* packet, int size) -> int
{
    record(true, packet, static_cast<uint32_t>(size));
    return encrypt_(context, packet, size);
}

// A reply's second dword is non-zero until it is decrypted.
auto __cdecl decryptHook(void* context, uint8_t* packet) -> bool
{
    const auto encrypted = read32(reinterpret_cast<uintptr_t>(packet) + 4) != 0;
    const bool ok{ decrypt_(context, packet) };
    if (ok && encrypted)
    {
        record(false, packet, read32(reinterpret_cast<uintptr_t>(packet)));
    }
    return ok;
}

auto __cdecl nextPageHook(void* connection) -> int
{
    const int result{ nextPage_(connection) };
    if (result == 0) // the request was queued
    {
        record(true, NEXT_PAGE.data(), NEXT_PAGE.size());
    }
    return result;
}

// Where the search connection keeps its encrypt function, or 0 before there is a connection.
auto encryptPointer() -> uintptr_t
{
    try
    {
        const auto base   = read32(read32(connection_ + 1));
        const auto search = base ? read32(base + read32(connection_ + 12)) : 0;
        const auto conn   = search ? read32(search) : 0;
        return conn ? conn + 0x18 : 0;
    }
    catch (...) // /EHa: a bad read is an access violation
    {
        return 0;
    }
}

} // namespace

auto install(const Sink sink) -> std::string
{
    const auto encrypt      = find("B8B0100000E8????????538B9C24B8100000555633F685DB");
    const auto decrypt      = find("B8B0100000E8????????538B9C24BC100000568B430485C0");
    const auto decryptCall  = find("5356C644241B00E8????????8BB424");
    const auto nextPage     = find("C7866004000007000000"); // 0x36 into the next page request
    const auto nextPageCall = find("66396F1E76138B1652E8????????83C40485C0750466896F1E");
    connection_             = find("A1????????85C07501C38B80????????50E8");
    if (!encrypt || !decrypt || !decryptCall || !nextPage || !nextPageCall || !connection_)
    {
        return "a search signature is missing";
    }
    encrypt_  = reinterpret_cast<Encrypt>(encrypt);
    decrypt_  = reinterpret_cast<Decrypt>(decrypt);
    nextPage_ = reinterpret_cast<NextPage>(nextPage - 0x36);
    sink_     = sink;
    for (const auto& [operand, expected, target] : { std::tuple{ decryptCall + 8, decrypt, reinterpret_cast<uintptr_t>(&decryptHook) },
                                                     std::tuple{ nextPageCall + 10, reinterpret_cast<uintptr_t>(nextPage_), reinterpret_cast<uintptr_t>(&nextPageHook) } })
    {
        const auto patch = redirect(operand, expected, target);
        if (!patch)
        {
            remove();
            return "a search call site calls something else";
        }
        patches_.push_back(*patch);
    }
    return {};
}

auto tick() -> void
{
    if (const auto pointer = sink_ ? encryptPointer() : 0; pointer && read32(pointer) == reinterpret_cast<uintptr_t>(encrypt_))
    {
        *reinterpret_cast<uint32_t*>(pointer) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&encryptHook));
    }
}

auto remove() -> void
{
    for (const auto& patch : patches_)
    {
        write32(patch.operand, patch.backup);
    }
    patches_.clear();
    if (const auto pointer = encryptPointer(); pointer && read32(pointer) == reinterpret_cast<uintptr_t>(&encryptHook))
    {
        *reinterpret_cast<uint32_t*>(pointer) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(encrypt_));
    }
    sink_ = nullptr;
}

} // namespace search

namespace lobby
{
namespace
{

constexpr uintptr_t LIST_OFFSET    = 0x148; // in the lobby context
constexpr uintptr_t VERSION_OFFSET = 0x1A08;

// The content id getter: mov ecx,[esp+4]; index * 0x8C; mov ecx,[client]; mov eax,[ecx+eax*4+list]; ret
auto getter() -> uintptr_t
{
    static const auto address = find("8B4C24048D04CD000000002BC18B0D????????8D04808B8481????????C3");
    return address;
}

// The character list in FFXiMain's copy of the lobby context, which the client keeps after the login; 0 before it has one.
auto list() -> uintptr_t
{
    const auto client = read32(read32(getter() + 15));
    return client ? client + read32(getter() + 25) : 0;
}

} // namespace

auto version() -> std::string
{
    if (getter() == 0)
    {
        return {};
    }
    try
    {
        const auto l = list();
        if (l == 0)
        {
            return {};
        }
        const auto*       text = reinterpret_cast<const char*>(l - LIST_OFFSET + VERSION_OFFSET);
        const std::string version(text, strnlen(text, 15));
        // Anything but digits and underscores means the layout moved: say nothing rather than record noise.
        return std::ranges::all_of(version, [](char ch)
                                   {
                                       return (ch >= '0' && ch <= '9') || ch == '_';
                                   })
                   ? version
                   : std::string{};
    }
    catch (...)
    {
        return {};
    }
}

auto character(capture::Login& who) -> std::string
{
    if (getter() == 0)
    {
        return "the character signature is missing";
    }
    // The list holds lobby packet 0x20's entries (LSB's lpkt_chr_info_sub2); the context keeps the count before it and the index sent in packet 0x07 before that.
    constexpr uint32_t ENTRY_SIZE      = 0x8C;
    constexpr size_t   WORLD_NAME_SIZE = 16;
    try
    {
        const auto list = lobby::list();
        if (list == 0)
        {
            return "no character list";
        }
        const auto index = read32(list - 8);
        if (index >= read32(list - 4) || index >= 16)
        {
            return "no character selected";
        }
        const auto* entry = reinterpret_cast<const uint8_t*>(list + index * ENTRY_SIZE);
        uint16_t    low{};
        std::memcpy(&low, entry + 4, sizeof(low));
        if ((static_cast<uint32_t>(entry[11]) << 16 | low) != who.character.characterId)
        {
            return "the selected character is another one";
        }
        std::memcpy(&who.character.contentId, entry, sizeof(who.character.contentId));
        std::memcpy(&who.worldId, entry + 6, sizeof(who.worldId));
        const auto* world   = reinterpret_cast<const char*>(entry + 28);
        who.character.world = std::string(world, strnlen(world, WORLD_NAME_SIZE));
        return {};
    }
    catch (...)
    {
        return "cannot read the character list";
    }
}

} // namespace lobby

} // namespace memoria::client
