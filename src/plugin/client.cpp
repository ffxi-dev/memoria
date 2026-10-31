#include "plugin/client.h"

#include <Ashita.h>

#include <array>
#include <cstring>
#include <vector>

namespace memoria::client
{
namespace
{

auto moduleRange(const char* name) -> std::pair<uintptr_t, uintptr_t>
{
    const auto base = reinterpret_cast<uintptr_t>(::GetModuleHandleA(name));
    if (base == 0)
    {
        return { 0, 0 };
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    return { base, nt->OptionalHeader.SizeOfImage };
}

auto read32(uintptr_t address) -> uint32_t
{
    uint32_t v{};
    std::memcpy(&v, reinterpret_cast<const void*>(address), sizeof(v));
    return v;
}

auto write32(uintptr_t address, uint32_t value) -> bool
{
    DWORD protection = 0;
    if (!::VirtualProtect(reinterpret_cast<void*>(address), sizeof(value), PAGE_EXECUTE_READWRITE, &protection))
    {
        return false;
    }
    std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(value));
    ::VirtualProtect(reinterpret_cast<void*>(address), sizeof(value), protection, &protection);
    ::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(address), sizeof(value));
    return true;
}

} // namespace

auto find(const char* pattern) -> uintptr_t
{
    const auto [base, size] = moduleRange("FFXiMain.dll");
    return base == 0 ? 0 : Ashita::Memory::FindPattern(base, size, pattern, 0, 0);
}

namespace search
{
namespace
{

// All three are cdecl: each ends in a plain ret, and the callers clean up.
using Encrypt  = int(__cdecl*)(void* context, uint8_t* packet, int size);
using Decrypt  = bool(__cdecl*)(void* context, uint8_t* packet);
using NextPage = int(__cdecl*)(void* connection);

constexpr uint32_t MAX_SIZE = 0x1000;

// The next page request is encrypted once per connection and resent as is:
// this is its plaintext.
constexpr std::array<uint8_t, 16> NEXT_PAGE = { 0x10, 0x00, 0x00, 0x00, 0x49, 0x58, 0x46, 0x46, 0x10, 0x00, 0x80, 0x10, 0x00, 0x00, 0x00, 0x00 };

struct Patch
{
    uintptr_t operand;
    uint32_t  backup;
};

Sink               sink_{};
Encrypt            encrypt_{};
Decrypt            decrypt_{};
NextPage           nextPage_{};
uintptr_t          connection_{}; // the signature that reads the search connection
std::vector<Patch> patches_;

auto record(bool clientToServer, const uint8_t* data, uint32_t size) -> void
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

// The second dword of a reply is non-zero until it is decrypted.
auto __cdecl decryptHook(void* context, uint8_t* packet) -> bool
{
    const auto encrypted = read32(reinterpret_cast<uintptr_t>(packet) + 4) != 0;
    const bool ok        = decrypt_(context, packet);
    if (ok && encrypted)
    {
        record(false, packet, read32(reinterpret_cast<uintptr_t>(packet)));
    }
    return ok;
}

// Returns 0 when the request was queued.
auto __cdecl nextPageHook(void* connection) -> int
{
    const int queued = nextPage_(connection);
    if (queued == 0)
    {
        record(true, NEXT_PAGE.data(), NEXT_PAGE.size());
    }
    return queued;
}

// Points the call whose rel32 operand is at operand to target, if it calls expected.
auto redirect(uintptr_t operand, uintptr_t expected, uintptr_t target) -> bool
{
    const auto backup = read32(operand);
    if (operand + 4 + backup != expected)
    {
        return false;
    }
    if (!write32(operand, static_cast<uint32_t>(target - (operand + 4))))
    {
        return false;
    }
    patches_.push_back({ operand, backup });
    return true;
}

// Where the search connection keeps its encrypt function, or 0 before it exists.
auto encryptPointer() -> uintptr_t
{
    try
    {
        const auto base = read32(read32(connection_ + 1));
        if (base == 0)
        {
            return 0;
        }
        const auto search = read32(base + read32(connection_ + 12));
        if (search == 0)
        {
            return 0;
        }
        const auto connection = read32(search);
        return connection == 0 ? 0 : connection + 0x18;
    }
    catch (...) // /EHa: a bad read is an access violation
    {
        return 0;
    }
}

} // namespace

auto install(Sink sink) -> std::string
{
    const auto encrypt      = find("B8B0100000E8????????538B9C24B8100000555633F685DB");
    const auto decrypt      = find("B8B0100000E8????????538B9C24BC100000568B430485C0");
    const auto decryptCall  = find("5356C644241B00E8????????8BB424"); // the only caller of decrypt
    const auto nextPage     = find("C7866004000007000000");           // inside the next page request
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
    if (!redirect(decryptCall + 8, decrypt, reinterpret_cast<uintptr_t>(&decryptHook)) ||
        !redirect(nextPageCall + 10, reinterpret_cast<uintptr_t>(nextPage_), reinterpret_cast<uintptr_t>(&nextPageHook)))
    {
        remove();
        return "a search call site calls something else";
    }
    return {};
}

auto tick() -> void
{
    if (!sink_)
    {
        return;
    }
    if (const auto pointer = encryptPointer(); pointer && read32(pointer) == reinterpret_cast<uintptr_t>(encrypt_))
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

// build(packet, type, context), cdecl: the callers clean up.
using Build = int(__cdecl*)(uint8_t* packet, uint32_t type, const uint8_t* context);

constexpr uintptr_t VERSION_OFFSET = 0x1A08; // in the lobby context, 15 bytes
constexpr size_t    VERSION_SIZE   = 15;

Build                          build_{};
uintptr_t                      operand_{};
uint32_t                       backup_{};
std::array<char, VERSION_SIZE> version_{};

auto __cdecl buildHook(uint8_t* packet, uint32_t type, const uint8_t* context) -> int
{
    try
    {
        std::memcpy(version_.data(), context + VERSION_OFFSET, VERSION_SIZE);
    }
    catch (...) // /EHa
    {
    }
    return build_(packet, type, context);
}

} // namespace

auto install() -> std::string
{
    // mov ecx,[edi+4F1ACh] (the lobby context); mov edx,[esp+10h]; push ecx; push 26h; push edx; call build
    const auto site = find("8B8FACF104008B542410516A2652E8");
    if (site == 0)
    {
        return "the lobby signature is missing";
    }
    operand_ = site + 15;
    backup_  = read32(operand_);
    build_   = reinterpret_cast<Build>(operand_ + 4 + backup_);
    if (!write32(operand_, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&buildHook) - (operand_ + 4))))
    {
        operand_ = 0;
        return "cannot patch the lobby call";
    }
    return {};
}

auto version() -> std::string
{
    return { version_.data(), strnlen(version_.data(), version_.size()) };
}

auto remove() -> void
{
    if (operand_ != 0)
    {
        write32(operand_, backup_);
        operand_ = 0;
    }
}

} // namespace lobby

} // namespace memoria::client
