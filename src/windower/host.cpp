// The Windower 4 host: hands Windower's callbacks to the session. docs/windower-interface.md says what each call
// rests on; what only a live client can confirm is marked (probe) there and in interface.h.

#include "plugin/session.h"
#include "windower/interface.h"

#include <cstring>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <ranges>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace memoria
{
namespace
{

// The chat mode Hook.dll itself prints with (bind listing, 0x10037157 in its decompile).
constexpr int CHAT_MODE = 0x8d;

// <Windower>, two levels above this DLL in <Windower>/plugins.
auto windowerFolder() -> std::filesystem::path
{
    HMODULE self = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&windowerFolder), &self);
    std::wstring path(MAX_PATH, L'\0');
    path.resize(::GetModuleFileNameW(self, path.data(), static_cast<DWORD>(path.size())));
    return std::filesystem::path(path).parent_path().parent_path();
}

// The host's shape: each slot Memoria calls must point into Hook.dll's image, or this is not the host it was built for.
auto inHook(const void* object, std::initializer_list<int> slots) -> bool
{
    const auto* hook = reinterpret_cast<const uint8_t*>(::GetModuleHandleW(L"Hook.dll"));
    if (hook == nullptr || object == nullptr)
    {
        return false;
    }
    try // /EHa: a bad read is an access violation
    {
        const auto* nt    = reinterpret_cast<const IMAGE_NT_HEADERS*>(hook + reinterpret_cast<const IMAGE_DOS_HEADER*>(hook)->e_lfanew);
        const auto  first = reinterpret_cast<uintptr_t>(hook);
        const auto  last  = first + nt->OptionalHeader.SizeOfImage;
        const auto* table = *static_cast<const uintptr_t* const*>(object);
        for (const auto slot : slots)
        {
            if (table[slot] < first || table[slot] >= last)
            {
                return false;
            }
        }
        return true;
    }
    catch (...)
    {
        return false;
    }
}

// Commands in chat text, as a Windower player types them in the chat line.
auto windowerText(const std::string& text) -> std::string
{
    static const std::regex command(R"((^|[^\w/\\:.])/memoria\b)");
    return std::regex_replace(text, command, "$1//memoria");
}

class Plugin final : public windower::IPlugin
{
public:
    void __stdcall Attach(windower::IWindower* host) override
    {
        // Memoria calls the host's slots 0, 5, and 10, the console's Write, and the game's AddToChat. Until the version
        // matches, no other slot is safe to call, so a mismatch can only go to the debugger.
        if (!inHook(host, { 0, 5, 10 }) || host->GetVersion() != windower::HOST_VERSION)
        {
            ::OutputDebugStringA("Memoria: not the Windower host Memoria was built for (interface 0x40004); it records nothing.\n");
            return;
        }
        console_ = host->GetConsole();
        ffxi_    = host->GetFFXI();
        if (!inHook(console_, { 4 }))
        {
            ::OutputDebugStringA("Memoria: Windower's console is not where Memoria expects; it records nothing.\n");
            return;
        }
        if (!inHook(ffxi_, { 1 }))
        {
            console_->Write("Memoria: this Windower is not the one Memoria was built for (Hook.dll 4.4.2.0). Memoria needs an update; it records nothing.");
            return;
        }

        const auto folder = windowerFolder();
        session_          = std::make_unique<Session>(Host{
            .config   = folder / "memoria" / "config",
            .captures = folder / "memoria" / "captures",
            .captain  = folder / "addons" / "captain",
            .say      = [this](const std::string& text)
            {
                std::lock_guard lock(mutex_);
                chat_.push_back("\x1E\x51[\x1E\x06Memoria\x1E\x51]\x1E\x01 " + windowerText(text));
            },
            .log = [this](const std::string& text)
            {
                console_->Write(text.c_str());
            },
        });
        // ponytail: no late login; Hook.dll exposes no player state, so a load mid-session waits for the next zone-in.
        session_->start(std::nullopt);
    }

    void __stdcall Unload() override
    {
        session_.reset(); // joins every thread and unhooks the client before Hook.dll frees this DLL
    }

    void __stdcall Release() override
    {
        delete this;
    }

    windower::PluginInfo* __stdcall GetInfo(windower::PluginInfo* out) override
    {
        std::memset(out, 0, sizeof(*out));
        strcpy_s(out->author, "sruon");
        strcpy_s(out->commands, "memoria");
        return out;
    }

    // The console thread is not known to be the game's, so commands wait for the next frame, as chat does.
    void __stdcall HandleCommand(const char* args, const char* keyword) override
    {
        if (!session_)
        {
            return;
        }
        std::lock_guard lock(mutex_);
        commands_.emplace_back(std::format("{} {}", keyword, args));
    }

    bool __stdcall HandleUnhandledCommand(const char*) override
    {
        return false;
    }

    void __stdcall Prerender() override
    {
        if (!session_)
        {
            return;
        }
        std::vector<std::string> commands;
        {
            std::lock_guard lock(mutex_);
            commands.swap(commands_);
        }
        for (const auto& line : commands)
        {
            std::vector<std::string> words;
            for (const auto word : std::views::split(std::string_view(line), ' '))
            {
                if (!word.empty())
                {
                    words.emplace_back(std::string_view(word));
                }
            }
            session_->command(words, line);
        }
        session_->frame();

        std::vector<std::string> chat;
        {
            std::lock_guard lock(mutex_);
            chat.swap(chat_);
        }
        for (const auto& text : chat)
        {
            ffxi_->AddToChat(text.c_str(), CHAT_MODE);
        }
    }

    bool __stdcall IncomingChunk(const uint8_t* original, uint8_t* modified, bool injected, bool blocked) override
    {
        if (session_)
        {
            session_->incoming(id(original), size(original), original, modified, 0, nullptr, injected, blocked);
        }
        return false;
    }

    bool __stdcall OutgoingChunk(const uint8_t* original, uint8_t* modified, bool injected, bool blocked) override
    {
        if (session_)
        {
            session_->outgoing(size(original), original, modified, 0, nullptr, injected, blocked);
        }
        return false;
    }

    void __stdcall DeviceReset1() override
    {
    }
    void __stdcall DeviceReset2() override
    {
    }
    void __stdcall Postrender() override
    {
    }
    void __stdcall IncomingText(const char*, char*, int*) override
    {
    }
    void __stdcall OutgoingText(const char*, char*, int) override
    {
    }
    void __stdcall unknown14(void*) override
    {
    }
    bool __stdcall KeepOnUnloadAll() override
    {
        return false;
    }
    void __stdcall unknown16() override
    {
    }
    bool __stdcall Mouse(int, int, int, int, bool) override
    {
        return false;
    }
    bool __stdcall Keyboard(int, char, bool) override
    {
        return false;
    }
    void __stdcall AddItem(uint8_t, int, int, int) override
    {
    }
    void __stdcall RemoveItem(uint8_t, int, int, int) override
    {
    }

private:
    // A game packet's header: a 9-bit id, then its size in 4-byte words.
    static auto id(const uint8_t* p) -> uint16_t
    {
        return static_cast<uint16_t>((p[0] | p[1] << 8) & 0x1FF);
    }

    static auto size(const uint8_t* p) -> uint32_t
    {
        return static_cast<uint32_t>(p[1] & 0xFE) * 2;
    }

    windower::IConsole*      console_{ nullptr };
    windower::IFFXI*         ffxi_{ nullptr };
    std::mutex               mutex_;
    std::vector<std::string> chat_;     // from any thread, printed on the next frame
    std::vector<std::string> commands_; // from the console, run on the next frame
    std::unique_ptr<Session> session_;  // last: destroyed first, while what its callbacks use still exists
};

} // namespace
} // namespace memoria

extern "C" uint32_t GetInterfaceVersion()
{
    return windower::INTERFACE_VERSION;
}

extern "C" windower::IPlugin* CreateInstance()
{
    return new memoria::Plugin();
}
