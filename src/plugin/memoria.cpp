#include "plugin/memoria.h"

#include "core/seed_file.h"
#include "plugin/client.h"
#include "plugin/winhttp.h"

#include <Commands.h>
#include <shellapi.h>
#include <xicap/constants.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace memoria
{
namespace
{

constexpr uint16_t ZONE_IN = 0x00A;
constexpr uint16_t LOGOUT  = 0x00B;

// Square Enix's retail zone servers, 124.150.152.0/21.
constexpr uint32_t RETAIL_FIRST = 0x7C969800;
constexpr uint32_t RETAIL_LAST  = 0x7C969FFF;

// GP_SERV_COMMAND_LOGOUT's LogoutState. Zone changes, the Mog House, and a cancelled logout keep the character in game.
auto leavesGame(uint8_t logoutState) -> bool
{
    constexpr uint8_t ZONECHANGE = 2;
    constexpr uint8_t MYROOM     = 3;
    constexpr uint8_t CANCEL     = 4;
    return logoutState != 0 && logoutState != ZONECHANGE && logoutState != MYROOM && logoutState != CANCEL;
}

// The zone server's IPv4 address in host order (credits: atom0s, via captain).
auto zoneServer() -> uint32_t
{
    static const uintptr_t sig = client::find("8B0D????????8D04808B8481????????C3");
    if (sig == 0)
    {
        return 0;
    }
    try
    {
        const auto ptr = *reinterpret_cast<const uint32_t*>(sig + 2);
        const auto ret = *reinterpret_cast<const uint32_t*>(ptr);
        return std::byteswap(*reinterpret_cast<const uint32_t*>(ret));
    }
    catch (...) // /EHa: a bad read is an access violation
    {
        return 0;
    }
}

auto characterOf(const uint8_t* data, uint32_t size) -> xicap::CharacterIdentity
{
    xicap::CharacterIdentity c{};
    if (size >= xicap::FFXIPacket::LOGIN_NAME_OFFSET + xicap::FFXIPacket::LOGIN_NAME_SIZE)
    {
        std::memcpy(&c.characterId, data + xicap::FFXIPacket::LOGIN_UNIQUE_NO_OFFSET, sizeof(c.characterId));
        const auto* name = reinterpret_cast<const char*>(data + xicap::FFXIPacket::LOGIN_NAME_OFFSET);
        std::memcpy(c.characterName.data(), name, strnlen(name, c.characterName.size() - 1));
    }
    return c;
}

// "key = value" lines; # and ; start comments.
auto readIni(const std::filesystem::path& path) -> std::optional<std::map<std::string, std::string>>
{
    std::ifstream in(path);
    if (!in)
    {
        return std::nullopt;
    }
    const auto trim = [](std::string_view s)
    {
        const auto first = s.find_first_not_of(" \t\r");
        return first == std::string_view::npos ? std::string{} : std::string(s.substr(first, s.find_last_not_of(" \t\r") - first + 1));
    };
    std::map<std::string, std::string> values;
    for (std::string line; std::getline(in, line);)
    {
        const auto eq = line.find('=');
        if (eq != std::string::npos && !line.starts_with('#') && !line.starts_with(';'))
        {
            values[trim(std::string_view(line).substr(0, eq))] = trim(std::string_view(line).substr(eq + 1));
        }
    }
    return values;
}

Plugin* instance_ = nullptr; // for the search hooks, which take a plain function

// Ashita's chat colours, as its chat.lua names them: 0x1E, then a colour number; 0x1E 0x01 goes back to normal.
enum class Color : uint8_t
{
    Green  = 2,
    Cyan   = 6,
    Grey   = 67,
    Red    = 68,
    Yellow = 104,
};

auto paint(Color c, std::string_view text) -> std::string
{
    return std::format("\x1E{}{}\x1E\x01", static_cast<char>(c), text);
}

auto size(uint64_t bytes) -> std::string
{
    constexpr std::array units{ "KB", "MB", "GB" };
    if (bytes < 1024)
    {
        return std::format("{} B", bytes);
    }
    auto   value = bytes / 1024.0;
    size_t unit  = 0;
    for (; value >= 1024 && unit + 1 < units.size(); ++unit)
    {
        value /= 1024;
    }
    return std::format("{:.1f} {}", value, units[unit]);
}

auto count(uint64_t n) -> std::string
{
    auto text = std::to_string(n);
    for (auto at = static_cast<std::ptrdiff_t>(text.size()) - 3; at > 0; at -= 3)
    {
        text.insert(static_cast<size_t>(at), ",");
    }
    return text;
}

} // namespace

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
    core_ = core;
    log_  = logger;

    install_        = core_->GetInstallPath();
    const auto seed = loadOrCreateSeed(install_ / "config" / "memoria" / "key");
    if (!seed)
    {
        startError_ = seed.error();
        say("not recording: " + startError_);
        return true; // stay loaded so /memoria status can say why
    }
    seed_ = *seed;
    link_ = loadLink(install_ / "config" / "memoria" / "token");
    if (auto config = readIni(install_ / "config" / "memoria" / "upload.ini"); config && (*config)["server"].starts_with("https://"))
    {
        server_ = (*config)["server"];
        while (server_.ends_with('/'))
        {
            server_.pop_back();
        }
    }

    try
    {
        recorder_ = std::make_unique<Recorder>(RecorderOptions{ .dir = install_ / "captures" / "memoria", .seed = seed_, .captureAgent = "Memoria " MEMORIA_VERSION_TEXT });
        startUploads();
    }
    catch (const std::exception& error)
    {
        startError_ = error.what();
        say("not recording: " + startError_);
        return true;
    }
    instance_    = this;
    lobbyError_  = client::lobby::install();
    searchError_ = client::search::install([](bool clientToServer, const uint8_t* data, uint32_t size)
                                           {
                                               instance_->recordSearch(clientToServer, data, size);
                                           });

    // Loaded while already in a zone: record from here, marked mid-session.
    auto* party = core_->GetMemoryManager()->GetParty();
    if (core_->GetMemoryManager()->GetPlayer()->GetLoginStatus() == 2 && party->GetMemberServerId(0) != 0)
    {
        Login late{ .network = network(), .midSession = true };
        late.character.characterId = party->GetMemberServerId(0);
        const char* name           = party->GetMemberName(0);
        std::memcpy(late.character.characterName.data(), name, strnlen(name, late.character.characterName.size() - 1));
        characterError_ = client::lobby::character(late.character);
        character_      = late.character;
        loggedIn_       = true;
        loggedInAs_     = late.character.characterId;
        recorder_->login(clock_.nowMs(), std::move(late));
    }
    log_->Logf(std::to_underlying(Ashita::LogLevel::Info), "Memoria", "Memoria %s recording to %s", MEMORIA_VERSION_TEXT, (install_ / "captures" / "memoria").string().c_str());
    return true;
}

auto Plugin::Release() -> void
{
    linker_ = {}; // stops and joins
    client::search::remove();
    client::lobby::remove();
    if (recorder_)
    {
        recorder_->stop();
        recorder_.reset();
    }
    uploader_.reset();
    if (uploadMutex_)
    {
        ::CloseHandle(std::exchange(uploadMutex_, nullptr));
    }
}

auto Plugin::HandleIncomingPacket(uint16_t id, uint32_t size, const uint8_t* data, uint8_t* modified, uint32_t sizeChunk, const uint8_t* dataChunk, bool injected, bool blocked) -> bool
{
    if (!recorder_)
    {
        return false;
    }
    // A zone-in after character select is a login, and so is one naming another character: a disconnect sends no logout.
    if (id == ZONE_IN)
    {
        auto who = characterOf(data, size);
        if (!loggedIn_ || who.characterId != loggedInAs_)
        {
            characterError_ = client::lobby::character(who);
            character_      = who;
            loggedIn_       = true;
            loggedInAs_     = who.characterId;
            remind();
            recorder_->login(clock_.nowMs(), { .character = who, .network = network(), .clientVersion = client::lobby::version() });
        }
    }
    record(false, size, data, modified, sizeChunk, dataChunk, injected, blocked);
    if (id == LOGOUT && size > 4 && leavesGame(data[4]))
    {
        loggedIn_ = false;
        recorder_->logout(clock_.nowMs());
    }
    return false;
}

auto Plugin::HandleOutgoingPacket(uint16_t id, uint32_t size, const uint8_t* data, uint8_t* modified, uint32_t sizeChunk, const uint8_t* dataChunk, bool injected, bool blocked) -> bool
{
    UNREFERENCED_PARAMETER(id);
    if (recorder_)
    {
        record(true, size, data, modified, sizeChunk, dataChunk, injected, blocked);
    }
    return false;
}

auto Plugin::record(bool clientToServer, uint32_t size, const uint8_t* data, const uint8_t* modified, uint32_t sizeChunk, const uint8_t* dataChunk, bool injected, bool blocked) -> void
{
    uint16_t provenance = 0;
    if (injected)
    {
        provenance |= xicap::PacketFlag::Injected;
    }
    if (blocked)
    {
        provenance |= xicap::PacketFlag::Blocked;
    }
    if (modified && std::memcmp(data, modified, size) != 0)
    {
        provenance |= xicap::PacketFlag::Modified;
    }
    // Ashita hands each packet as a copy, so a new chunk shows as a new chunk pointer, size, or head (its sequence number).
    if (dataChunk && sizeChunk > 0)
    {
        ChunkKey key{ reinterpret_cast<uintptr_t>(dataChunk), sizeChunk, {} };
        std::memcpy(key.head.data(), dataChunk, (std::min<size_t>)(sizeChunk, key.head.size()));
        auto& last = lastChunk_[clientToServer ? 1 : 0];
        if (last != key)
        {
            provenance |= xicap::PacketFlag::FirstInChunk;
        }
        last = key;
    }
    recorder_->packet({
        .timestamp      = clock_.nowMs(),
        .clientToServer = clientToServer,
        .provenance     = provenance,
        .data           = { data, data + size },
    });
}

// The opcode is byte 0x0B, the request type LSB's search server switches on.
auto Plugin::recordSearch(bool clientToServer, const uint8_t* data, uint32_t size) -> void
{
    recorder_->packet({
        .timestamp      = clock_.nowMs(),
        .clientToServer = clientToServer,
        .channel        = std::to_underlying(xicap::Channel::Search),
        .opcode         = data[0x0B],
        .data           = { data, data + size },
    });
}

// Ashita refuses a plugin that asks for Direct3D callbacks and turns the device down.
auto Plugin::Direct3DInitialize(IDirect3DDevice8* device) -> bool
{
    UNREFERENCED_PARAMETER(device);
    return true;
}

auto Plugin::Direct3DBeginScene(bool isRenderingBackBuffer) -> void
{
    UNREFERENCED_PARAMETER(isRenderingBackBuffer);
    client::search::tick();

    std::vector<std::string> inbox;
    std::optional<Linked>    arrived;
    {
        std::lock_guard lock(inboxMutex_);
        inbox.swap(inbox_);
        arrived.swap(arrived_);
    }
    for (const auto& text : inbox)
    {
        say(text);
    }
    if (arrived)
    {
        link_ = std::move(arrived);
        say(paint(Color::Green, std::format("Linked to {}.", link_->account)) + " Your captures upload from now on.");
        uploader_.reset();
        startUploads();
    }
    if (uploader_ && uploader_->unauthorized())
    {
        unlink("Vault no longer accepts this install");
    }
}

auto Plugin::network() -> xicap::Network
{
    const auto ip = zoneServer();
    if (ip == 0)
    {
        return xicap::Network::Unknown;
    }
    return ip >= RETAIL_FIRST && ip <= RETAIL_LAST ? xicap::Network::Retail : xicap::Network::Private;
}

auto Plugin::startUploads() -> void
{
    if (!link_)
    {
        uploadsOff_ = "not linked: /memoria link";
        return;
    }
    if (std::filesystem::exists(install_ / "config" / "memoria" / "paused"))
    {
        uploadsOff_ = "paused: /memoria upload on";
        return;
    }
    UploadOptions options{
        .dir         = install_ / "captures" / "memoria",
        .server      = server_,
        .credentials = { { "Authorization", "Bearer " + link_->token } },
    };
    // Clients sharing an install share its captures folder; the one whose upload thread holds this mutex uploads, and Windows hands it on when that client exits.
    if (!uploadMutex_)
    {
        uploadMutex_ = ::CreateMutexA(nullptr, FALSE, std::format("Local\\Memoria-upload-{:x}", std::hash<std::wstring>{}(options.dir.wstring())).c_str());
    }
    options.mayRun = [mutex = uploadMutex_]
    {
        const auto r = ::WaitForSingleObject(mutex, 0);
        return r == WAIT_OBJECT_0 || r == WAIT_ABANDONED;
    };
    uploader_ = std::make_unique<Uploader>(std::move(options), std::make_unique<WinHttp>("Memoria/" MEMORIA_VERSION_TEXT));
    uploader_->start();
}

auto Plugin::say(const std::string& text) -> void
{
    core_->GetChatManager()->Writef(1, false, "\x1E\x51[\x1E\x06Memoria\x1E\x51]\x1E\x01 %s", text.c_str());
}

auto Plugin::post(std::string text) -> void
{
    std::lock_guard lock(inboxMutex_);
    inbox_.push_back(std::move(text));
}

// Once a day while unlinked: the date of the last reminder is kept across restarts.
auto Plugin::remind() -> void
{
    const auto  path  = install_ / "config" / "memoria" / "reminded";
    const auto  today = std::format("{:%F}", std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now()));
    std::string last;
    std::getline(std::ifstream(path), last);
    if (link_ || last == today)
    {
        return;
    }
    std::ofstream(path, std::ios::trunc) << today;
    say(paint(Color::Yellow, "Your captures stay on this PC.") + " Run " + paint(Color::Cyan, "/memoria link") + " to share them with Vault.");
}

// Asks for a code, then polls until the player approves it on the link page.
auto Plugin::link() -> void
{
    linker_ = std::jthread([this](std::stop_token stop)
                           {
                               WinHttp    http("Memoria/" MEMORIA_VERSION_TEXT);
                               const auto now  = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                               const auto code = startLink(http, server_, seed_, static_cast<uint64_t>(now));
                               if (!code)
                               {
                                   post(paint(Color::Red, "Cannot start linking: " + code.error()));
                                   return;
                               }
                               post("Sign in on the page that opened in your browser and enter " + paint(Color::Yellow, code->code));
                               post(paint(Color::Grey, "No browser? Open " + code->url));
                               // Only Mogsafe's own link page: never a URL the server could swap for another.
                               if (code->url.starts_with(server_ + "/link"))
                               {
                                   const auto com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
                                   ::ShellExecuteW(nullptr, L"open", std::wstring(code->url.begin(), code->url.end()).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                                   if (SUCCEEDED(com))
                                   {
                                       ::CoUninitialize();
                                   }
                               }
                               std::mutex                  mutex;
                               std::condition_variable_any wake;
                               const auto                  until = std::chrono::steady_clock::now() + code->expires;
                               while (std::chrono::steady_clock::now() < until)
                               {
                                   std::unique_lock lock(mutex);
                                   if (wake.wait_for(lock, stop, code->interval, []
                                                     {
                                                         return false;
                                                     }) ||
                                       stop.stop_requested())
                                   {
                                       return;
                                   }
                                   const auto result = pollLink(http, server_, code->poll);
                                   if (!result)
                                   {
                                       post(paint(Color::Red, "Not linked: " + result.error() + ".") + " Run " + paint(Color::Cyan, "/memoria link") + " again.");
                                       return;
                                   }
                                   if (*result)
                                   {
                                       saveLink(install_ / "config" / "memoria" / "token", **result);
                                       std::lock_guard inbox(inboxMutex_);
                                       arrived_ = **result;
                                       return;
                                   }
                               }
                               post(paint(Color::Red, "The code expired.") + " Run " + paint(Color::Cyan, "/memoria link") + " again.");
                           });
}

auto Plugin::unlink(const std::string& why) -> void
{
    uploader_.reset();
    link_.reset();
    std::error_code ec;
    std::filesystem::remove(install_ / "config" / "memoria" / "token", ec);
    uploadsOff_ = "not linked: /memoria link";
    say(paint(Color::Yellow, why + ".") + " Your captures stay on this PC until you run " + paint(Color::Cyan, "/memoria link") + ".");
}

auto Plugin::status() -> void
{
    const auto s = recorder_->status();
    if (s.capture.empty())
    {
        say(paint(Color::Grey, "Waiting for a login."));
    }
    else
    {
        std::error_code ec;
        const auto      onDisk  = std::filesystem::file_size(s.capture, ec);
        const auto      raw     = s.bytesIn + s.bytesOut;
        const auto      seconds = (std::max)(uint64_t{ 1 }, (s.lastMs - s.firstMs) / 1000);
        say(paint(Color::Green, "Recording") + " " + s.capture.filename().string());
        say(std::format("Packets: {} ({} in, {} out), {:.1f}/s", count(s.packets), count(s.packetsIn), count(s.packetsOut), static_cast<double>(s.packets) / static_cast<double>(seconds)));
        say(std::format("Size: {} raw, {} on disk{}", size(raw), size(onDisk), onDisk > 0 ? std::format(" ({:.1f}x)", static_cast<double>(raw) / static_cast<double>(onDisk)) : ""));
        say(std::format("Search: {}, dropped: {}", count(s.search), count(s.dropped)));
    }
    if (!s.session.empty())
    {
        say("Session: " + s.session.string());
    }
    if (!s.error.empty())
    {
        say(paint(Color::Red, "Last write error: " + s.error));
    }
    if (loggedIn_)
    {
        const std::string name(character_.characterName.data());
        say(characterError_.empty() ? std::format("Character: {} on {}, content id {}", name, character_.worldName.data(), character_.contentId) : "Character: " + name + paint(Color::Grey, " (world unknown: " + characterError_ + ")"));
    }
    const auto version = client::lobby::version();
    say("Client: " + (version.empty() ? paint(Color::Grey, "known after character select" + (lobbyError_.empty() ? "" : " (" + lobbyError_ + ")")) : version));
    if (!searchError_.empty())
    {
        say(paint(Color::Red, "Search not recorded: " + searchError_));
    }
    if (!uploader_)
    {
        say("Uploads: " + paint(Color::Yellow, uploadsOff_));
        return;
    }
    const auto files   = uploader_->status();
    uint64_t   total   = 0;
    uint64_t   sent    = 0;
    size_t     pending = 0;
    for (const auto& u : files)
    {
        total += u.size;
        sent += u.closed ? u.size : u.sent;
        pending += u.closed ? 0 : 1;
    }
    say(std::format("Uploads: linked to {}, {} of {} sent, {} of {} captures still open", link_->account, size(sent), size(total), pending, files.size()));
    for (const auto& u : files | std::views::filter([](const UploadStatus& u)
                                                    {
                                                        return !u.error.empty();
                                                    }))
    {
        say(paint(Color::Red, std::format("{}: {}", u.name, u.error)));
    }
}

auto Plugin::HandleCommand(int32_t mode, const char* command, bool injected) -> bool
{
    UNREFERENCED_PARAMETER(mode);
    UNREFERENCED_PARAMETER(injected);
    std::vector<std::string> args;
    Ashita::Commands::GetCommandArgs(command, &args);
    if (args.empty() || args[0] != "/memoria")
    {
        return false;
    }
    if (!recorder_)
    {
        say("not recording: " + startError_);
        return true;
    }
    const auto sub = args.size() > 1 ? args[1] : "status";
    if (sub == "status")
    {
        status();
    }
    else if (sub == "key")
    {
        const auto s = recorder_->status();
        say(s.capture.empty() ? "the key shows once a capture opens" : "public key " + std::ranges::fold_left(s.publicKey, std::string{}, [](std::string hex, uint8_t b)
                                                                                                              {
                                                                                                                  return hex + std::format("{:02x}", b);
                                                                                                              }));
    }
    else if (sub == "note" && args.size() > 2)
    {
        recorder_->note(clock_.nowMs(), args | std::views::drop(2) | std::views::join_with(' ') | std::ranges::to<std::string>());
        say("noted");
    }
    else if (sub == "session" && args.size() > 2 && args[2] == "start")
    {
        // The rest of the line as typed: a path can hold spaces.
        std::string_view path(command);
        path.remove_prefix(path.find("start") + 5);
        path.remove_prefix((std::min)(path.find_first_not_of(" \""), path.size()));
        path = path.substr(0, path.find_last_not_of(" \"") + 1);
        if (path.empty())
        {
            say("/memoria session start <file.xicap>");
            return true;
        }
        recorder_->sessionStart(clock_.nowMs(), std::u8string(path.begin(), path.end()));
        say(std::format("session to {}", path));
    }
    else if (sub == "session" && args.size() > 2 && args[2] == "stop")
    {
        recorder_->sessionStop(clock_.nowMs());
        say("session closed");
    }
    else if (sub == "link")
    {
        say(paint(Color::Grey, "Asking Vault for a link code..."));
        link();
    }
    else if (sub == "unlink" && link_)
    {
        unlink("Unlinked");
    }
    else if (sub == "upload" && args.size() > 2 && (args[2] == "off" || args[2] == "on"))
    {
        const auto      paused = install_ / "config" / "memoria" / "paused";
        std::error_code ec;
        if (args[2] == "off")
        {
            std::ofstream{ paused };
            uploader_.reset();
            uploadsOff_ = "paused: /memoria upload on";
            say(paint(Color::Yellow, "Uploads paused.") + " " + paint(Color::Cyan, "/memoria upload on") + " resumes them.");
        }
        else
        {
            std::filesystem::remove(paused, ec);
            if (!uploader_)
            {
                startUploads();
            }
            say(uploader_ ? paint(Color::Green, "Uploads on.") : paint(Color::Yellow, "Uploads off: " + uploadsOff_));
        }
    }
    else
    {
        say("/memoria status | link | unlink | upload on|off | key | note <text> | session start <file.xicap> | session stop");
    }
    return true;
}

} // namespace memoria

extern "C" __declspec(dllexport) IPlugin* __stdcall expCreatePlugin(const char* args)
{
    UNREFERENCED_PARAMETER(args);
    return new memoria::Plugin();
}

extern "C" __declspec(dllexport) void __stdcall expDestroyPlugin(void* instance)
{
    delete static_cast<memoria::Plugin*>(instance);
}

extern "C" __declspec(dllexport) double __stdcall expGetInterfaceVersion()
{
    return ASHITA_INTERFACE_VERSION;
}
