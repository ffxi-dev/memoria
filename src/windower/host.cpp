// The Windower 4 host: hands Windower's callbacks to the session. docs/windower-interface.md says what each call
// rests on; what only a live client can confirm is marked (live) there and in interface.h.

#include "plugin/session.h"
#include "windower/interface.h"

#include <Windows.h>

#include <filesystem>
#include <format>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace memoria::windower
{
namespace
{

// The chat mode Hook.dll 4.4.2.0 printed its bind list with; Hook.dll hands the mode to the game as is.
constexpr int CHAT_MODE{ 0x8d };

// Hook.dll hands a console line starting with it to HandleCommand, without the keyword.
constexpr const char* COMMAND{ "memoria" };

// <Windower>, two levels above this DLL in <Windower>/plugins.
auto windowerFolder() -> std::filesystem::path
{
    HMODULE self{ nullptr };
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&windowerFolder), &self);
    std::wstring path(MAX_PATH, L'\0');
    path.resize(::GetModuleFileNameW(self, path.data(), static_cast<DWORD>(path.size())));
    return std::filesystem::path{ path }.parent_path().parent_path();
}

// The host's shape: each slot Memoria calls must point into Hook.dll's image, or this is not the host it was built for.
auto inHook(const void* object, const std::initializer_list<int> slots) -> bool
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
    static const std::regex command{ R"((^|[^\w/\\:.])/memoria\b)" };
    return std::regex_replace(text, command, "$1//memoria");
}

// A game packet's header: a 9-bit id, then its size in 4-byte words.
auto packetId(const uint8_t* p) -> uint16_t
{
    return static_cast<uint16_t>((p[0] | p[1] << 8) & 0x1FF);
}

auto packetSize(const uint8_t* p) -> uint32_t
{
    return static_cast<uint32_t>(p[1] & 0xFE) * 2;
}

class Plugin final : public IPlugin
{
public:
    auto __stdcall Attach(IWindower* host) -> void override
    {
        // Hook.dll loaded Memoria only on an exact interface version. Memoria calls the host's slots 3 and 7, the
        // console's Write, and the game's AddToChat; until the console checks out, a mismatch can only go to the
        // debugger.
        if (!inHook(host, { 3, 7 }))
        {
            ::OutputDebugStringA("Memoria: not the Windower host Memoria was built for (interface 0x4070300); it records nothing.\n");
            return;
        }
        console_ = host->GetConsole();
        ffxi_    = host->GetFFXI();
        if (!inHook(console_, { 3 }))
        {
            ::OutputDebugStringA("Memoria: Windower's console is not where Memoria expects; it records nothing.\n");
            return;
        }
        if (!inHook(ffxi_, { 0 }))
        {
            console_->Write("Memoria: this Windower is not the one Memoria was built for (Hook.dll 4.7.9.3). Memoria needs an update; it records nothing.");
            return;
        }

        const auto folder = windowerFolder() / "memoria";
        session_          = std::make_unique<plugin::Session>(plugin::Host{
            .config   = folder / "config",
            .captures = folder / "captures",
            .say      = [this](const std::string& text)
            {
                const std::scoped_lock lock(mutex_);
                chat_.push_back("\x1E\x51[\x1E\x06Memoria\x1E\x51]\x1E\x01 " + windowerText(text));
            },
            .log = [this](const std::string& text)
            {
                console_->Write(text.c_str());
            },
        });
        // No late login: Hook.dll exposes no player state, so a load mid-session waits for the next zone-in.
        session_->start(std::nullopt);
    }

    // Hook.dll calls FreeLibrary next. Deleting the plugin stops the session first, as Ashita's Release does.
    auto __stdcall Unload() -> void override
    {
        delete this;
    }

    auto __stdcall GetAuthor() -> const char* override
    {
        return "sruon";
    }

    auto __stdcall GetCommands() -> const char* override
    {
        return COMMAND;
    }

    auto __stdcall IsCore() -> bool override
    {
        return false;
    }

    // The console thread is not known to be the game's, so commands wait for the next frame, as chat does.
    auto __stdcall HandleCommand(const char* args) -> void override
    {
        if (!session_)
        {
            return;
        }
        const std::scoped_lock lock(mutex_);
        commands_.push_back(std::format("{} {}", COMMAND, args));
    }

    auto __stdcall HandleUnhandledCommand(const char* /*line*/) -> bool override
    {
        return false;
    }

    auto __stdcall Prerender() -> void override
    {
        if (!session_)
        {
            return;
        }
        std::vector<std::string> commands;
        {
            const std::scoped_lock lock(mutex_);
            commands.swap(commands_);
        }
        for (const auto& line : commands)
        {
            std::vector<std::string> words;
            for (const auto word : std::views::split(std::string_view{ line }, ' '))
            {
                if (!word.empty())
                {
                    words.emplace_back(std::string_view{ word });
                }
            }
            session_->command(words, line);
        }
        session_->frame();

        std::vector<std::string> chat;
        {
            const std::scoped_lock lock(mutex_);
            chat.swap(chat_);
        }
        for (const auto& text : chat)
        {
            ffxi_->AddToChat(text.c_str(), CHAT_MODE);
        }
    }

    // Hook.dll hands no chunk, so the session finds a chunk's first packet by its sync counter.
    auto __stdcall IncomingChunk(const uint8_t* original, uint8_t* modified, const bool injected, const bool blocked) -> bool override
    {
        if (session_)
        {
            session_->incoming(packetId(original), packetSize(original), original, modified, 0, nullptr, injected, blocked);
        }
        return false;
    }

    auto __stdcall OutgoingChunk(const uint8_t* original, uint8_t* modified, const bool injected, const bool blocked) -> bool override
    {
        if (session_)
        {
            session_->outgoing(packetSize(original), original, modified, 0, nullptr, injected, blocked);
        }
        return false;
    }

    auto __stdcall Postrender() -> void override
    {
    }

    auto __stdcall IncomingText(const char* /*original*/, char* /*modified*/, int* /*mode*/) -> void override
    {
    }

    auto __stdcall OutgoingText(const char* /*original*/, char* /*modified*/, int /*source*/) -> void override
    {
    }

    auto __stdcall Mouse(int /*type*/, int /*x*/, int /*y*/, int /*delta*/, bool /*blocked*/) -> bool override
    {
        return false;
    }

    auto __stdcall Keyboard(int /*dik*/, char /*pressed*/, bool /*blocked*/) -> bool override
    {
        return false;
    }

    auto __stdcall AddItem(uint8_t /*bag*/, int /*index*/, int /*id*/, int /*count*/) -> void override
    {
    }

    auto __stdcall RemoveItem(uint8_t /*bag*/, int /*index*/, int /*id*/, int /*count*/) -> void override
    {
    }

private:
    IConsole*                        console_{ nullptr };
    IFFXI*                           ffxi_{ nullptr };
    std::mutex                       mutex_;
    std::vector<std::string>         chat_;     // printed on the next frame
    std::vector<std::string>         commands_; // from the console, run on the next frame
    std::unique_ptr<plugin::Session> session_;  // last: destroyed first, while what its callbacks use still exists
};

} // namespace
} // namespace memoria::windower

extern "C" auto GetInterfaceVersion() -> uint32_t
{
    return memoria::windower::INTERFACE_VERSION;
}

extern "C" auto CreateInstance() -> memoria::windower::IPlugin*
{
    return new memoria::windower::Plugin();
}
