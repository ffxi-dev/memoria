// MemoriaProbe.dll: a Windower 4 plugin built from interface.h alone. It logs every call Hook.dll makes, with its
// arguments and thread, to <Windower>/memoria-probe.log, and prints once to the console and the chat, so a live
// client settles what reading the binaries left open. docs/windower-interface.md says how to run it.

#include "windower/interface.h"

#include <Windows.h>

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#ifndef PROBE_INTERFACE_VERSION
#define PROBE_INTERFACE_VERSION windower::INTERFACE_VERSION
#endif

namespace
{

std::mutex  logMutex;
FILE*       logFile = nullptr;
const DWORD started = ::GetTickCount();

auto hookBase() -> uintptr_t
{
    return reinterpret_cast<uintptr_t>(::GetModuleHandleW(L"Hook.dll"));
}

auto log(const char* format, ...) -> void
{
    std::lock_guard lock(logMutex);
    if (logFile == nullptr)
    {
        return;
    }
    std::fprintf(logFile, "%8lu ms  tid %5lu  ", ::GetTickCount() - started, ::GetCurrentThreadId());
    va_list args;
    va_start(args, format);
    std::vfprintf(logFile, format, args);
    va_end(args);
    std::fputc('\n', logFile);
    std::fflush(logFile);
}

// An address as Hook.dll+offset when it lies there, so the log reads against docs/windower-interface.md.
auto where(const void* p) -> std::string
{
    const auto a    = reinterpret_cast<uintptr_t>(p);
    const auto base = hookBase();
    char       text[48];
    if (base != 0 && a >= base && a < base + 0x200000)
    {
        std::snprintf(text, sizeof(text), "%08zx (Hook.dll+%zx)", a, a - base);
    }
    else
    {
        std::snprintf(text, sizeof(text), "%08zx", a);
    }
    return text;
}

auto vtable(const char* name, const void* object, int slots) -> void
{
    if (object == nullptr)
    {
        log("%s is null", name);
        return;
    }
    const auto* table = *static_cast<const void* const* const*>(object);
    log("%s %s, vtable %s", name, where(object).c_str(), where(table).c_str());
    for (int i = 0; i < slots; ++i)
    {
        log("  %s[%d] %s", name, i, where(table[i]).c_str());
    }
}

auto hex(const uint8_t* p, uint32_t n) -> std::string
{
    std::string text;
    for (uint32_t i = 0; i < n; ++i)
    {
        char b[4];
        std::snprintf(b, sizeof(b), "%02x", p[i]);
        text += b;
    }
    return text;
}

// Frequent calls are logged in full the first few times, then once every 1000.
auto often(int slot, unsigned limit) -> unsigned
{
    static std::array<unsigned, 21> counts{};
    const auto                      n = ++counts[slot];
    return n <= limit || n % 1000 == 0 ? n : 0;
}

class Probe final : public windower::IPlugin
{
public:
    void __stdcall Attach(windower::IWindower* host) override
    {
        log("Attach host=%s Hook.dll base %08zx", where(host).c_str(), hookBase());
        vtable("IWindower", host, 11);
        if (host == nullptr)
        {
            return;
        }
        log("GetVersion() = %f", host->GetVersion());
        log("GetWindow() = %s", where(host->GetWindow()).c_str());
        static std::array<char, 0x8000> settings{};
        log("GetSettings(out) = %s, out = %s; out+0x2130 = \"%.260s\"", where(host->GetSettings(settings.data())).c_str(), where(settings.data()).c_str(), settings.data() + 0x2130);
        console_ = host->GetConsole();
        vtable("IConsole", console_, 8);
        ffxi_ = host->GetFFXI();
        vtable("IFFXI", ffxi_, 3);
        log("GetTextHandler() = %s, GetPrimitiveHandler() = %s, GetPacketStreamHandler() = %s, unknown9() = %s", where(host->GetTextHandler()).c_str(), where(host->GetPrimitiveHandler()).c_str(), where(host->GetPacketStreamHandler()).c_str(), where(host->unknown9()).c_str());
        if (console_ != nullptr)
        {
            console_->Write("MemoriaProbe loaded: logging to memoria-probe.log. Type: memoriaprobe hello");
        }
    }

    void __stdcall Unload() override
    {
        log("Unload");
    }

    void __stdcall Release() override
    {
        log("Release");
        delete this;
    }

    windower::PluginInfo* __stdcall GetInfo(windower::PluginInfo* out) override
    {
        log("GetInfo out=%s", where(out).c_str());
        std::memset(out, 0, sizeof(*out));
        strcpy_s(out->author, "sruon");
        strcpy_s(out->commands, "memoriaprobe|mprobe");
        return out;
    }

    void __stdcall HandleCommand(const char* args, const char* keyword) override
    {
        log("HandleCommand args=\"%s\" keyword=\"%s\"", args, keyword);
        // memoriaprobe chat <mode> <text>: tries AddToChat with a given mode.
        int  mode = 0x8d;
        char text[256]{};
        if (sscanf_s(args, "chat %i %255[^\n]", &mode, text, static_cast<unsigned>(sizeof(text))) == 2)
        {
            ffxi_->AddToChat(text, mode);
            return;
        }
        std::snprintf(text, sizeof(text), "MemoriaProbe heard \"%s\" (keyword \"%s\")", args, keyword);
        console_->Write(text);
        ffxi_->AddToChat(text, 0x8d);
    }

    bool __stdcall HandleUnhandledCommand(const char* line) override
    {
        log("HandleUnhandledCommand line=\"%s\"", line);
        return false;
    }

    void __stdcall DeviceReset1() override
    {
        log("DeviceReset1 (slot 6)");
    }

    void __stdcall DeviceReset2() override
    {
        log("DeviceReset2 (slot 7)");
    }

    void __stdcall Prerender() override
    {
        if (const auto n = often(8, 3))
        {
            log("Prerender #%u", n);
        }
        if (!greeted_ && ffxi_ != nullptr)
        {
            greeted_ = true;
            ffxi_->AddToChat("\x1E\x51[\x1E\x06MemoriaProbe\x1E\x51]\x1E\x01 loaded: AddToChat works from Prerender.", 0x8d);
            log("AddToChat called from Prerender");
        }
    }

    void __stdcall Postrender() override
    {
        if (const auto n = often(9, 3))
        {
            log("Postrender #%u", n);
        }
    }

    void __stdcall IncomingText(const char* original, char* modified, int* mode) override
    {
        if (const auto n = often(10, 20))
        {
            log("IncomingText #%u mode=%d same=%d original=\"%.120s\"", n, mode ? *mode : -1, original == modified, original);
        }
    }

    void __stdcall OutgoingText(const char* original, char* modified, int source) override
    {
        log("OutgoingText source=%d same=%d original=\"%.120s\"", source, original == modified, original);
    }

    bool __stdcall IncomingChunk(const uint8_t* original, uint8_t* modified, bool injected, bool blocked) override
    {
        chunk(12, "IncomingChunk", original, modified, injected, blocked);
        return false;
    }

    bool __stdcall OutgoingChunk(const uint8_t* original, uint8_t* modified, bool injected, bool blocked) override
    {
        chunk(13, "OutgoingChunk", original, modified, injected, blocked);
        return false;
    }

    void __stdcall unknown14(void* a) override
    {
        log("unknown14 (slot 14) a=%s", where(a).c_str());
    }

    bool __stdcall KeepOnUnloadAll() override
    {
        log("KeepOnUnloadAll (slot 15)");
        return false;
    }

    void __stdcall unknown16() override
    {
        log("unknown16 (slot 16)");
    }

    bool __stdcall Mouse(int type, int x, int y, int delta, bool blocked) override
    {
        if (const auto n = often(17, 3))
        {
            log("Mouse #%u type=%d x=%d y=%d delta=%d blocked=%d", n, type, x, y, delta, blocked);
        }
        return false;
    }

    bool __stdcall Keyboard(int dik, char pressed, bool blocked) override
    {
        if (const auto n = often(18, 10))
        {
            log("Keyboard #%u dik=%d pressed=%d blocked=%d", n, dik, pressed, blocked);
        }
        return false;
    }

    void __stdcall AddItem(uint8_t bag, int index, int id, int count) override
    {
        log("AddItem (slot 19) bag=%u index=%d id=%d count=%d", bag, index, id, count);
    }

    void __stdcall RemoveItem(uint8_t bag, int index, int id, int count) override
    {
        log("RemoveItem (slot 20) bag=%u index=%d id=%d count=%d", bag, index, id, count);
    }

private:
    // id, size, and sync from the header; whether the two buffers differ; the first 32 bytes. Logins and zone-outs always.
    static void chunk(int slot, const char* name, const uint8_t* original, uint8_t* modified, bool injected, bool blocked)
    {
        const auto id   = (original[0] | original[1] << 8) & 0x1FF;
        const auto size = static_cast<uint32_t>(original[1] & 0xFE) * 2;
        const auto n    = often(slot, 300);
        if (n != 0 || id == 0x00A || id == 0x00B)
        {
            log("%s #%u id=%03x size=%u sync=%u injected=%d blocked=%d same-buffer=%d changed=%d original=%s modified=%s bytes=%s", name, n, id, size, original[2] | original[3] << 8, injected, blocked, original == modified, std::memcmp(original, modified, size) != 0, where(original).c_str(), where(modified).c_str(), hex(original, size < 32 ? size : 32).c_str());
        }
    }

    windower::IConsole* console_{ nullptr };
    windower::IFFXI*    ffxi_{ nullptr };
    bool                greeted_{ false };
};

// <Windower>/memoria-probe.log: two levels above this DLL in <Windower>/plugins.
auto openLog() -> void
{
    HMODULE self = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&openLog), &self);
    wchar_t path[MAX_PATH]{};
    ::GetModuleFileNameW(self, path, MAX_PATH);
    for (int up = 0; up < 2; ++up)
    {
        if (auto* slash = std::wcsrchr(path, L'\\'))
        {
            *slash = L'\0';
        }
    }
    wcscat_s(path, L"\\memoria-probe.log");
    std::lock_guard lock(logMutex);
    if (logFile == nullptr)
    {
        logFile = _wfsopen(path, L"a", _SH_DENYWR);
    }
}

} // namespace

extern "C" uint32_t GetInterfaceVersion()
{
    openLog();
    log("GetInterfaceVersion -> %#x", static_cast<unsigned>(PROBE_INTERFACE_VERSION));
    return PROBE_INTERFACE_VERSION;
}

extern "C" windower::IPlugin* CreateInstance()
{
    log("CreateInstance");
    return new Probe();
}
